#include "FrameAnalyzer.h"
#include "YoloEngine.h"
#include "YoloPostprocess.h"
#include "Letterbox.h"
// SavGol smoothing is applied as an AE expression, not baked into keyframes.

#include "AEGP_SuiteHandler.h"
#include "AE_GeneralPlug.h"

#include <vector>
#include <string>

#ifdef _WIN32
#include <windows.h>
static void DebugLog(const std::string& msg) {
    OutputDebugStringA(("[AE_YOLO] " + msg + "\n").c_str());
}
#elif defined(__APPLE__)
#include <os/log.h>
static void DebugLog(const std::string& msg) {
    os_log(OS_LOG_DEFAULT, "[AE_YOLO] %{public}s", msg.c_str());
}
#else
static void DebugLog(const std::string&) {}
#endif

static AEGP_PluginID g_aegp_plugin_id = 0;

PF_Err AnalyzeAndWriteKeyframes(
    PF_InData* in_data,
    PF_OutData* out_data,
    float conf_threshold,
    int smooth_window,
    int smooth_order,
    int skip_frames)
{
    PF_Err err = PF_Err_NONE;

    AEGP_SuiteHandler suites(in_data->pica_basicP);

    // --- 1. Register with AEGP (once) ---
    if (g_aegp_plugin_id == 0) {
        err = suites.UtilitySuite6()->AEGP_RegisterWithAEGP(
            NULL, "AE_YOLO", &g_aegp_plugin_id);
        if (err) {
            DebugLog("AEGP_RegisterWithAEGP failed err=" + std::to_string(err));
            return err;
        }
        DebugLog("Registered AEGP plugin ID = " + std::to_string(g_aegp_plugin_id));
    }

    // --- 2. Get the layer and effect refs ---
    DebugLog("Step 2: Getting layer and effect refs...");
    AEGP_LayerH layerH = NULL;
    err = suites.PFInterfaceSuite1()->AEGP_GetEffectLayer(
        in_data->effect_ref, &layerH);
    if (err || !layerH) {
        DebugLog("AEGP_GetEffectLayer failed err=" + std::to_string(err));
        return err ? err : PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
    DebugLog("Step 2: Got layerH OK");

    AEGP_EffectRefH effectRefH = NULL;
    err = suites.PFInterfaceSuite1()->AEGP_GetNewEffectForEffect(
        g_aegp_plugin_id, in_data->effect_ref, &effectRefH);
    if (err || !effectRefH) {
        DebugLog("AEGP_GetNewEffectForEffect failed err=" + std::to_string(err));
        return err ? err : PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
    DebugLog("Step 2: Got effectRefH OK");

    // --- 3. Render options are created per-frame inside the loop ---
    // Creating fresh LayerRenderOptionsH for each frame ensures AEGP_SetTime
    // is respected. Reusing a single renderOptsH from NewFromUpstreamOfEffect
    // can lock to the creation-time context on some AE versions.
    DebugLog("Step 3: Render options will be created per-frame");

    // --- 4. Get layer timing info ---
    DebugLog("Step 4: Getting timing info...");
    A_Time layer_offset;
    err = suites.LayerSuite8()->AEGP_GetLayerOffset(layerH, &layer_offset);
    if (err) {
        DebugLog("GetLayerOffset failed err=" + std::to_string(err));
        suites.EffectSuite4()->AEGP_DisposeEffect(effectRefH);
        return err;
    }
    DebugLog("Step 4: layer_offset=" + std::to_string(layer_offset.value) + "/" + std::to_string(layer_offset.scale));

    A_Time in_point, out_point_duration;
    err = suites.LayerSuite8()->AEGP_GetLayerInPoint(layerH, AEGP_LTimeMode_CompTime, &in_point);
    if (err) {
        DebugLog("GetLayerInPoint failed err=" + std::to_string(err));
        suites.EffectSuite4()->AEGP_DisposeEffect(effectRefH);
        return err;
    }
    DebugLog("Step 4: in_point=" + std::to_string(in_point.value) + "/" + std::to_string(in_point.scale));

    err = suites.LayerSuite8()->AEGP_GetLayerDuration(layerH, AEGP_LTimeMode_CompTime, &out_point_duration);
    if (err) {
        DebugLog("GetLayerDuration failed err=" + std::to_string(err));
        suites.EffectSuite4()->AEGP_DisposeEffect(effectRefH);
        return err;
    }
    DebugLog("Step 4: duration=" + std::to_string(out_point_duration.value) + "/" + std::to_string(out_point_duration.scale));

    AEGP_CompH compH = NULL;
    err = suites.LayerSuite8()->AEGP_GetLayerParentComp(layerH, &compH);
    if (err || !compH) {
        DebugLog("GetLayerParentComp failed err=" + std::to_string(err));
        suites.EffectSuite4()->AEGP_DisposeEffect(effectRefH);
        return err ? err : PF_Err_INTERNAL_STRUCT_DAMAGED;
    }

    A_FpLong fps = 0;
    err = suites.CompSuite11()->AEGP_GetCompFramerate(compH, &fps);
    if (err || fps <= 0) {
        fps = 24.0;
        DebugLog("Could not get framerate, using 24");
    }

    // Calculate frame time step
    A_long time_scale = static_cast<A_long>(fps * 100);
    A_long frame_step = 100; // 1/fps in time_scale units

    // Calculate total frame count
    double duration_sec = static_cast<double>(out_point_duration.value) / out_point_duration.scale;
    int num_frames = static_cast<int>(std::ceil(duration_sec * fps));
    if (num_frames <= 0) num_frames = 1;

    DebugLog("Step 4: fps=" + std::to_string(fps) +
             " duration=" + std::to_string(duration_sec) +
             "s frames=" + std::to_string(num_frames) +
             " time_scale=" + std::to_string(time_scale) +
             " frame_step=" + std::to_string(frame_step));

    // --- 4b. Layer diagnostics ---
    {
        AEGP_LayerFlags layer_flags = AEGP_LayerFlag_NONE;
        suites.LayerSuite8()->AEGP_GetLayerFlags(layerH, &layer_flags);
        DebugLog("Step 4b: layer_flags=0x" + ([](int f){
            char buf[9]; snprintf(buf, 9, "%08X", f); return std::string(buf);
        })(static_cast<int>(layer_flags)) +
            " TIME_REMAP=" + std::to_string(!!(layer_flags & AEGP_LayerFlag_TIME_REMAPPING)) +
            " FRAME_BLEND=" + std::to_string(!!(layer_flags & AEGP_LayerFlag_FRAME_BLENDING)) +
            " ADV_FRAME_BLEND=" + std::to_string(!!(layer_flags & AEGP_LayerFlag_ADVANCED_FRAME_BLENDING)));

        A_Ratio stretch = {};
        suites.LayerSuite8()->AEGP_GetLayerStretch(layerH, &stretch);
        DebugLog("Step 4b: layer_stretch=" + std::to_string(stretch.num) +
                 "/" + std::to_string(stretch.den));

        // Get source item duration to check for still/single-frame footage
        AEGP_ItemH srcItemH = NULL;
        suites.LayerSuite8()->AEGP_GetLayerSourceItem(layerH, &srcItemH);
        if (srcItemH) {
            A_Time src_dur = {};
            suites.ItemSuite9()->AEGP_GetItemDuration(srcItemH, &src_dur);
            A_long src_w = 0, src_h = 0;
            suites.ItemSuite9()->AEGP_GetItemDimensions(srcItemH, &src_w, &src_h);
            DebugLog("Step 4b: source_item dur=" + std::to_string(src_dur.value) +
                     "/" + std::to_string(src_dur.scale) +
                     " dims=" + std::to_string(src_w) + "x" + std::to_string(src_h));
        }

        // Convert comp time of first and last frame to layer time to check mapping
        A_Time comp_t0 = {}, comp_tN = {};
        A_Time layer_t0 = {}, layer_tN = {};
        comp_t0.scale = time_scale;
        comp_t0.value = in_point.value * time_scale / in_point.scale;
        comp_tN.scale = time_scale;
        comp_tN.value = in_point.value * time_scale / in_point.scale + (num_frames - 1) * frame_step;
        suites.LayerSuite8()->AEGP_ConvertCompToLayerTime(layerH, &comp_t0, &layer_t0);
        suites.LayerSuite8()->AEGP_ConvertCompToLayerTime(layerH, &comp_tN, &layer_tN);
        DebugLog("Step 4b: comp_t0=" + std::to_string(comp_t0.value) + "/" + std::to_string(comp_t0.scale) +
                 " -> layer_t0=" + std::to_string(layer_t0.value) + "/" + std::to_string(layer_t0.scale));
        DebugLog("Step 4b: comp_tN=" + std::to_string(comp_tN.value) + "/" + std::to_string(comp_tN.scale) +
                 " -> layer_tN=" + std::to_string(layer_tN.value) + "/" + std::to_string(layer_tN.scale));
    }

    // --- 5. Check model is loaded ---
    if (!YoloEngine::IsReady()) {
        DebugLog("Model not loaded, aborting");
        suites.EffectSuite4()->AEGP_DisposeEffect(effectRefH);
        return PF_Err_NONE;
    }

    int input_size = YoloEngine::GetInputSize();
    DebugLog("Step 5: Model ready, input_size=" + std::to_string(input_size));

    // conf_threshold is now passed in from the UI param
    DebugLog("Step 6: Using confidence threshold=" + std::to_string(conf_threshold));

    // --- 7. Process each frame (NO undo group here — rendering only) ---
    std::vector<KeypointResult> all_results(num_frames);
    std::vector<bool> frame_valid(num_frames, false);

    DebugLog("Step 7: Rendering " + std::to_string(num_frames) + " frames...");

    // Create progress dialog via PFAppSuite6
    PF_AppProgressDialogP prog_dlg = NULL;
    bool have_progress_dialog = false;
    try {
        // Use A_UTF16Char arrays directly — wchar_t is 32-bit on macOS,
        // so reinterpret_cast from wchar_t* is only safe on Windows.
        static const A_UTF16Char title[] = {
            'Y','O','L','O',' ','P','o','s','e',' ',
            'A','n','a','l','y','s','i','s', 0 };
        static const A_UTF16Char cancel[] = { 'C','a','n','c','e','l', 0 };
        PF_Err prog_err = suites.AppSuite6()->PF_CreateNewAppProgressDialog(
            title, cancel, FALSE, &prog_dlg);
        if (!prog_err && prog_dlg) {
            have_progress_dialog = true;
            DebugLog("Progress dialog created OK");
        }
    } catch (...) {
        DebugLog("PFAppSuite6 not available, using PF_PROGRESS fallback");
    }

    skip_frames = std::max(1, skip_frames);

    // Pre-allocate buffers outside the frame loop to avoid per-frame heap churn.
    std::vector<float> input_chw;
    std::vector<float> raw_output;
    std::vector<int64_t> out_shape;

    DebugLog("Step 7: Detection stride=" + std::to_string(skip_frames) +
             " (" + std::to_string((num_frames + skip_frames - 1) / skip_frames) +
             " YOLO calls for " + std::to_string(num_frames) + " frames)");

    int detect_count = 0;
    bool user_cancelled = false;
    for (int f = 0; f < num_frames; f++) {
        // Update progress
        if (have_progress_dialog) {
            PF_Err prog_err = suites.AppSuite6()->PF_AppProgressDialogUpdate(
                prog_dlg, static_cast<A_long>(f), static_cast<A_long>(num_frames));
            if (prog_err == PF_Interrupt_CANCEL) {
                DebugLog("User cancelled via progress dialog at frame " + std::to_string(f));
                user_cancelled = true;
                break;
            }
        } else {
            // Fallback: PF_PROGRESS shows AE's built-in progress bar
            PF_Err prog_err = PF_PROGRESS(in_data, f, num_frames);
            if (prog_err == PF_Interrupt_CANCEL) {
                DebugLog("User cancelled via PF_PROGRESS at frame " + std::to_string(f));
                user_cancelled = true;
                break;
            }
        }

        // Log progress every 10 frames
        if (f % 10 == 0) {
            DebugLog("Rendering frame " + std::to_string(f) + "/" + std::to_string(num_frames) +
                     " (" + std::to_string(detect_count) + " detections so far)");
        }

        // Skip frames not in this stride (always process first and last frame)
        if (skip_frames > 1 && f % skip_frames != 0 && f != num_frames - 1) {
            continue;
        }

        // Compute comp time for this frame (also used for keyframe writing)
        A_Time comp_time;
        comp_time.scale = time_scale;
        comp_time.value = in_point.value * time_scale / in_point.scale + f * frame_step;

        // Convert comp time → layer time for rendering.
        // AEGP_SetTime on LayerRenderOptionsH expects layer time (source-relative).
        A_Time render_time = {};
        suites.LayerSuite8()->AEGP_ConvertCompToLayerTime(layerH, &comp_time, &render_time);

        // Create fresh render options per frame to ensure AEGP_SetTime is respected.
        AEGP_LayerRenderOptionsH frameOptsH = NULL;
        err = suites.LayerRenderOptionsSuite1()->AEGP_NewFromUpstreamOfEffect(
            g_aegp_plugin_id, effectRefH, &frameOptsH);
        if (err || !frameOptsH) continue;

        suites.LayerRenderOptionsSuite1()->AEGP_SetWorldType(frameOptsH, AEGP_WorldType_8);
        suites.LayerRenderOptionsSuite1()->AEGP_SetDownsampleFactor(frameOptsH, 1, 1);
        err = suites.LayerRenderOptionsSuite1()->AEGP_SetTime(frameOptsH, render_time);
        if (err) {
            suites.LayerRenderOptionsSuite1()->AEGP_Dispose(frameOptsH);
            continue;
        }

        AEGP_FrameReceiptH receiptH = NULL;
        err = suites.RenderSuite5()->AEGP_RenderAndCheckoutLayerFrame(
            frameOptsH, NULL, NULL, &receiptH);
        if (err || !receiptH) {
            suites.LayerRenderOptionsSuite1()->AEGP_Dispose(frameOptsH);
            continue;
        }

        AEGP_WorldH worldH = NULL;
        err = suites.RenderSuite5()->AEGP_GetReceiptWorld(receiptH, &worldH);
        if (err || !worldH) {
            suites.RenderSuite5()->AEGP_CheckinFrame(receiptH);
            continue;
        }

        A_long width = 0, height = 0;
        A_u_long row_bytes = 0;
        suites.WorldSuite3()->AEGP_GetSize(worldH, &width, &height);
        suites.WorldSuite3()->AEGP_GetRowBytes(worldH, &row_bytes);

        PF_Pixel8* base_addr = NULL;
        suites.WorldSuite3()->AEGP_GetBaseAddr8(worldH, &base_addr);

        if (base_addr && width > 0 && height > 0) {
            // Diagnostic: comprehensive frame analysis for first 5 processed frames
            if (detect_count < 5) {
                // Read back the time actually set on the render options
                A_Time actual_time = {};
                suites.LayerRenderOptionsSuite1()->AEGP_GetTime(frameOptsH, &actual_time);

                // Hash the entire frame (FNV-1a on every 100th pixel for speed)
                uint32_t frame_hash = 2166136261u;
                for (int row = 0; row < height; row += 10) {
                    const unsigned char* row_ptr =
                        reinterpret_cast<const unsigned char*>(base_addr) + row * row_bytes;
                    for (int col = 0; col < width; col += 10) {
                        const unsigned char* p = row_ptr + col * 4;
                        frame_hash ^= p[1]; frame_hash *= 16777619u; // R
                        frame_hash ^= p[2]; frame_hash *= 16777619u; // G
                        frame_hash ^= p[3]; frame_hash *= 16777619u; // B
                    }
                }

                // Sample 5 pixels across the diagonal
                std::string diag_pixels;
                for (int i = 0; i < 5; i++) {
                    int sx = width * (i + 1) / 6;
                    int sy = height * (i + 1) / 6;
                    const PF_Pixel8* px = reinterpret_cast<const PF_Pixel8*>(
                        reinterpret_cast<const char*>(base_addr) + sy * row_bytes) + sx;
                    diag_pixels += "(" + std::to_string(px->red) + "," +
                                   std::to_string(px->green) + "," +
                                   std::to_string(px->blue) + ") ";
                }

                DebugLog("DIAG f=" + std::to_string(f) +
                         " comp=" + std::to_string(comp_time.value) + "/" + std::to_string(comp_time.scale) +
                         " layer=" + std::to_string(render_time.value) + "/" + std::to_string(render_time.scale) +
                         " actual=" + std::to_string(actual_time.value) + "/" + std::to_string(actual_time.scale) +
                         " size=" + std::to_string(width) + "x" + std::to_string(height) +
                         " hash=0x" + ([](uint32_t h){
                             char buf[9]; snprintf(buf, 9, "%08X", h); return std::string(buf);
                         })(frame_hash));
                DebugLog("DIAG f=" + std::to_string(f) + " diag_px: " + diag_pixels);
            }

            LetterboxInfo lb_info = LetterboxPreprocess(
                reinterpret_cast<const unsigned char*>(base_addr),
                static_cast<int>(width), static_cast<int>(height),
                static_cast<int>(row_bytes),
                input_size,
                input_chw);

            if (YoloEngine::RunInference(input_chw.data(), raw_output, out_shape)) {
                if (YoloPostprocess(raw_output, out_shape, lb_info,
                                     conf_threshold, all_results[f])) {
                    frame_valid[f] = true;
                    detect_count++;

                    // Log nose keypoint for first 5 detections to verify tracking
                    if (detect_count <= 5) {
                        DebugLog("DIAG f=" + std::to_string(f) +
                                 " nose=(" + std::to_string(all_results[f].x[0]) + "," +
                                 std::to_string(all_results[f].y[0]) + ")" +
                                 " lwrist=(" + std::to_string(all_results[f].x[9]) + "," +
                                 std::to_string(all_results[f].y[9]) + ")");
                    }
                }
            }
        }

        suites.RenderSuite5()->AEGP_CheckinFrame(receiptH);
        suites.LayerRenderOptionsSuite1()->AEGP_Dispose(frameOptsH);
    }

    // Dispose progress dialog
    if (have_progress_dialog && prog_dlg) {
        suites.AppSuite6()->PF_DisposeAppProgressDialog(prog_dlg);
        prog_dlg = NULL;
        DebugLog("Progress dialog disposed");
    }

    DebugLog("Step 7: Frame loop complete");

    // If user cancelled, clean up and bail
    if (user_cancelled) {
        DebugLog("Analysis cancelled by user, skipping keyframe writing");
        suites.EffectSuite4()->AEGP_DisposeEffect(effectRefH);
        return PF_Err_NONE;
    }

    // Count valid detections (keyframes are written only for detected frames;
    // AE interpolates between them, and the optional smooth() expression
    // provides temporal smoothing non-destructively).
    int valid_count = 0;
    for (bool v : frame_valid) if (v) valid_count++;
    DebugLog("Detection complete: " + std::to_string(valid_count) +
             " valid frames out of " + std::to_string(num_frames));

    // --- 8. Write keyframes (Point2D + Conf per keypoint) ---
    // Undo group ONLY wraps keyframe writing — NOT rendering
    DebugLog("Step 8: Starting undo group for keyframe writing...");
    suites.UtilitySuite6()->AEGP_StartUndoGroup("YOLO Pose Analysis");

    DebugLog("Step 8: Writing keyframes for " + std::to_string(valid_count) +
             " valid frames across " + std::to_string(NUM_KEYPOINTS) + " keypoints");

    for (int k = 0; k < NUM_KEYPOINTS; k++) {
        // --- 8a. Write Point2D keyframes (combined X, Y) ---
        {
            PF_ParamIndex param_idx = KP_POINT_PARAM(k);
            DebugLog("KP " + std::to_string(k) + " point: GetNewEffectStreamByIndex idx=" + std::to_string(param_idx));

            AEGP_StreamRefH streamH = NULL;
            err = suites.StreamSuite6()->AEGP_GetNewEffectStreamByIndex(
                g_aegp_plugin_id, effectRefH, param_idx, &streamH);
            if (err || !streamH) {
                DebugLog("KP " + std::to_string(k) + " point: GetStream FAILED err=" + std::to_string(err) +
                         " streamH=" + std::to_string((uintptr_t)streamH));
                continue;
            }
            DebugLog("KP " + std::to_string(k) + " point: Got stream OK");

            AEGP_AddKeyframesInfoH akH = NULL;
            err = suites.KeyframeSuite5()->AEGP_StartAddKeyframes(streamH, &akH);
            if (err || !akH) {
                DebugLog("KP " + std::to_string(k) + " point: StartAddKeyframes FAILED err=" + std::to_string(err));
                suites.StreamSuite6()->AEGP_DisposeStream(streamH);
                continue;
            }
            DebugLog("KP " + std::to_string(k) + " point: StartAddKeyframes OK");

            int kf_count = 0;
            for (int f = 0; f < num_frames; f++) {
                if (!frame_valid[f]) continue;

                A_Time frame_time;
                frame_time.scale = time_scale;
                frame_time.value = in_point.value * time_scale / in_point.scale + f * frame_step;

                A_long key_idx = 0;
                err = suites.KeyframeSuite5()->AEGP_AddKeyframes(
                    akH, AEGP_LTimeMode_CompTime, &frame_time, &key_idx);
                if (err) {
                    DebugLog("KP " + std::to_string(k) + " point f=" + std::to_string(f) +
                             ": AddKeyframes FAILED err=" + std::to_string(err));
                    continue;
                }

                AEGP_StreamValue2 sv;
                memset(&sv, 0, sizeof(sv));
                sv.streamH = streamH;
                sv.val.two_d.x = static_cast<A_FpLong>(all_results[f].x[k]);
                sv.val.two_d.y = static_cast<A_FpLong>(all_results[f].y[k]);

                err = suites.KeyframeSuite5()->AEGP_SetAddKeyframe(akH, key_idx, &sv);
                if (err) {
                    DebugLog("KP " + std::to_string(k) + " point f=" + std::to_string(f) +
                             ": SetAddKeyframe FAILED err=" + std::to_string(err));
                }
                kf_count++;
            }

            DebugLog("KP " + std::to_string(k) + " point: EndAddKeyframes (" + std::to_string(kf_count) + " keyframes)");
            suites.KeyframeSuite5()->AEGP_EndAddKeyframes(TRUE, akH);

            // Set smooth() expression — references the plugin's own slider so
            // the user can tweak smoothing interactively without re-analyzing.
            {
                // Build expression as A_UTF16Char for cross-platform safety
                // (wchar_t is 32-bit on macOS, A_UTF16Char is always 16-bit).
                const char* expr_utf8 =
                    "smooth(effect(\"YOLO Pose\")(\"Smooth Window\") / thisComp.frameRate, "
                    "effect(\"YOLO Pose\")(\"Smooth Samples\"))";
                std::vector<A_UTF16Char> expr16;
                for (const char* p = expr_utf8; *p; ++p)
                    expr16.push_back(static_cast<A_UTF16Char>(*p));
                expr16.push_back(0);
                PF_Err expr_err = suites.StreamSuite6()->AEGP_SetExpression(
                    g_aegp_plugin_id, streamH, expr16.data());
                if (!expr_err) {
                    suites.StreamSuite6()->AEGP_SetExpressionState(
                        g_aegp_plugin_id, streamH, TRUE);
                }
                DebugLog("KP " + std::to_string(k) + " smooth expr (interactive) err=" +
                         std::to_string(expr_err));
            }

            suites.StreamSuite6()->AEGP_DisposeStream(streamH);
            DebugLog("KP " + std::to_string(k) + " point: Done");
        }

        // --- 8b. Write Confidence keyframes (1D float) ---
        {
            PF_ParamIndex param_idx = KP_CONF_PARAM(k);
            DebugLog("KP " + std::to_string(k) + " conf: GetNewEffectStreamByIndex idx=" + std::to_string(param_idx));

            AEGP_StreamRefH streamH = NULL;
            err = suites.StreamSuite6()->AEGP_GetNewEffectStreamByIndex(
                g_aegp_plugin_id, effectRefH, param_idx, &streamH);
            if (err || !streamH) {
                DebugLog("KP " + std::to_string(k) + " conf: GetStream FAILED err=" + std::to_string(err));
                continue;
            }

            AEGP_AddKeyframesInfoH akH = NULL;
            err = suites.KeyframeSuite5()->AEGP_StartAddKeyframes(streamH, &akH);
            if (err || !akH) {
                DebugLog("KP " + std::to_string(k) + " conf: StartAddKeyframes FAILED err=" + std::to_string(err));
                suites.StreamSuite6()->AEGP_DisposeStream(streamH);
                continue;
            }

            int kf_count = 0;
            for (int f = 0; f < num_frames; f++) {
                if (!frame_valid[f]) continue;

                A_Time frame_time;
                frame_time.scale = time_scale;
                frame_time.value = in_point.value * time_scale / in_point.scale + f * frame_step;

                A_long key_idx = 0;
                err = suites.KeyframeSuite5()->AEGP_AddKeyframes(
                    akH, AEGP_LTimeMode_CompTime, &frame_time, &key_idx);
                if (err) continue;

                AEGP_StreamValue2 sv;
                memset(&sv, 0, sizeof(sv));
                sv.streamH = streamH;
                sv.val.one_d = static_cast<A_FpLong>(all_results[f].conf[k]);

                suites.KeyframeSuite5()->AEGP_SetAddKeyframe(akH, key_idx, &sv);
                kf_count++;
            }

            DebugLog("KP " + std::to_string(k) + " conf: EndAddKeyframes (" + std::to_string(kf_count) + " keyframes)");
            suites.KeyframeSuite5()->AEGP_EndAddKeyframes(TRUE, akH);
            suites.StreamSuite6()->AEGP_DisposeStream(streamH);
        }
    }

    // End undo group
    DebugLog("Step 8: Ending undo group...");
    suites.UtilitySuite6()->AEGP_EndUndoGroup();
    DebugLog("Step 8: Undo group ended");

    // Cleanup
    suites.EffectSuite4()->AEGP_DisposeEffect(effectRefH);

    DebugLog("AnalyzeAndWriteKeyframes: COMPLETE (" +
             std::to_string(valid_count) + "/" + std::to_string(num_frames) + " detected)");
    return PF_Err_NONE;
}
