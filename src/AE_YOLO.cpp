#include "AE_YOLO.h"
#include "YoloEngine.h"
#include "FrameAnalyzer.h"

#ifdef _WIN32
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")
static void DebugLog(const std::string& msg) {
    OutputDebugStringA(("[AE_YOLO] " + msg + "\n").c_str());
}
#elif defined(__APPLE__)
#include <dlfcn.h>
#include <libgen.h>
#include <sys/stat.h>
#include <dirent.h>
#include <os/log.h>
static void DebugLog(const std::string& msg) {
    os_log(OS_LOG_DEFAULT, "[AE_YOLO] %{public}s", msg.c_str());
}
#else
#include <dlfcn.h>
#include <libgen.h>
#include <sys/stat.h>
#include <dirent.h>
static void DebugLog(const std::string&) {}
#endif

// ============================================================================
// Auto-find model in ONNX_models/ subfolder next to the plugin
// ============================================================================
// variant: "x" for best quality, "m" for faster
static std::string FindDefaultModel(const char* variant = "x") {
    // Build a variant-specific search token, e.g. "26x" or "26m"
    std::string varToken = std::string("26") + variant;

#ifdef _WIN32
    wchar_t dllPath[MAX_PATH] = {};
    HMODULE hModule = NULL;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&FindDefaultModel),
        &hModule);
    if (!hModule) return "";
    GetModuleFileNameW(hModule, dllPath, MAX_PATH);
    PathRemoveFileSpecW(dllPath);

    std::wstring searchDir = std::wstring(dllPath) + L"\\ONNX_models\\";
    std::wstring searchPattern = searchDir + L"*pose*.onnx";
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(searchPattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        searchPattern = searchDir + L"*.onnx";
        hFind = FindFirstFileW(searchPattern.c_str(), &fd);
    }
    if (hFind == INVALID_HANDLE_VALUE) return "";

    // Iterate all matches, prefer the one containing the variant token
    std::wstring bestMatch;
    std::wstring fallback;
    do {
        std::wstring fname = fd.cFileName;
        // Convert filename to narrow for matching
        int narrowLen = WideCharToMultiByte(CP_UTF8, 0, fname.c_str(), -1, NULL, 0, NULL, NULL);
        std::string narrowName(narrowLen - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, fname.c_str(), -1, &narrowName[0], narrowLen, NULL, NULL);

        if (narrowName.find(varToken) != std::string::npos) {
            bestMatch = searchDir + fname;
            break;  // exact variant match
        }
        if (fallback.empty()) {
            fallback = searchDir + fname;
        }
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);

    std::wstring& chosen = bestMatch.empty() ? fallback : bestMatch;
    if (chosen.empty()) return "";

    int len = WideCharToMultiByte(CP_UTF8, 0, chosen.c_str(), -1, NULL, 0, NULL, NULL);
    if (len <= 0) return "";
    std::string result(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, chosen.c_str(), -1, &result[0], len, NULL, NULL);
    return result;
#else
    // macOS: use dladdr to find plugin bundle path
    Dl_info info;
    if (!dladdr((void*)&FindDefaultModel, &info)) return "";
    char pathBuf[1024];
    strncpy(pathBuf, info.dli_fname, sizeof(pathBuf) - 1);
    pathBuf[sizeof(pathBuf) - 1] = '\0';
    std::string dir = dirname(pathBuf);

    std::string modelsDir = dir + "/ONNX_models/";
    DIR* dp = opendir(modelsDir.c_str());
    if (!dp) return "";
    std::string bestMatch;
    std::string fallback;
    struct dirent* ep;
    while ((ep = readdir(dp))) {
        std::string name = ep->d_name;
        if (name.size() > 5 && name.substr(name.size() - 5) == ".onnx") {
            if (name.find(varToken) != std::string::npos &&
                name.find("pose") != std::string::npos) {
                bestMatch = modelsDir + name;
                break;
            }
            if (fallback.empty() && name.find("pose") != std::string::npos) {
                fallback = modelsDir + name;
            }
            if (fallback.empty()) fallback = modelsDir + name;
        }
    }
    closedir(dp);
    return bestMatch.empty() ? fallback : bestMatch;
#endif
}

// ============================================================================
// About
// ============================================================================
static PF_Err About(PF_InData* in_data, PF_OutData* out_data,
                     PF_ParamDef* params[], PF_LayerDef* output) {
    PF_SPRINTF(out_data->return_msg,
        "%s v%d.%d\r\rYOLO Pose Estimation for After Effects.\r"
        "Analyzes footage and writes body keypoints as keyframed parameters.",
        PLUGIN_NAME, MAJOR_VERSION, MINOR_VERSION);
    return PF_Err_NONE;
}

// ============================================================================
// GlobalSetup
// ============================================================================
static PF_Err GlobalSetup(PF_InData* in_data, PF_OutData* out_data,
                           PF_ParamDef* params[], PF_LayerDef* output) {
    out_data->my_version = PF_VERSION(MAJOR_VERSION, MINOR_VERSION,
                                       BUG_VERSION, PF_Stage_DEVELOP, BUILD_VERSION);

    out_data->out_flags =
        PF_OutFlag_DEEP_COLOR_AWARE |
        PF_OutFlag_SEQUENCE_DATA_NEEDS_FLATTENING |
        PF_OutFlag_SEND_UPDATE_PARAMS_UI;

    out_data->out_flags2 =
        PF_OutFlag2_SUPPORTS_SMART_RENDER |
        PF_OutFlag2_FLOAT_COLOR_AWARE |
        PF_OutFlag2_SUPPORTS_GET_FLATTENED_SEQUENCE_DATA;

    DebugLog("GlobalSetup: flags=0x" + std::to_string(out_data->out_flags) +
             " flags2=0x" + std::to_string(out_data->out_flags2));
    return PF_Err_NONE;
}

// ============================================================================
// GlobalSetdown
// ============================================================================
static PF_Err GlobalSetdown(PF_InData* in_data, PF_OutData* out_data,
                             PF_ParamDef* params[], PF_LayerDef* output) {
    YoloEngine::Shutdown();
    DebugLog("GlobalSetdown");
    return PF_Err_NONE;
}

// ============================================================================
// ParamsSetup — 43 parameters
// ============================================================================
static PF_Err ParamsSetup(PF_InData* in_data, PF_OutData* out_data,
                           PF_ParamDef* params[], PF_LayerDef* output) {
    PF_Err err = PF_Err_NONE;
    PF_ParamDef def;

    // Param 1: Analyze button
    AEFX_CLR_STRUCT(def);
    PF_ADD_BUTTON("Analyze", "Analyze",
                  0, PF_ParamFlag_SUPERVISE, ANALYZE_DISK_ID);

    // Param 3: Model Quality popup
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Model Quality",
                 2,                     // num choices
                 MODEL_QUALITY_BEST,    // default = Best Quality
                 "Best Quality (x)|Faster (m)",
                 MODEL_QUALITY_DISK_ID);

    // Param 4: Confidence threshold
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Confidence",
                          0.0, 1.0, 0.0, 1.0, 0.25,
                          PF_Precision_HUNDREDTHS, 0, 0,
                          CONFIDENCE_DISK_ID);

    // Param 4: Use GPU checkbox
    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Use GPU (DirectML)",
                     TRUE, 0, USE_GPU_DISK_ID);

    // Param 5: SavGol smoothing window size (odd, 1 = no smoothing)
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Smooth Window",
                          1.0, 51.0, 1.0, 51.0, 7.0,
                          PF_Precision_INTEGER, 0, 0,
                          SMOOTH_WINDOW_DISK_ID);

    // Param 6: SavGol polynomial order (must be < window)
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Poly Order",
                          1.0, 5.0, 1.0, 5.0, 3.0,
                          PF_Precision_INTEGER, 0, 0,
                          SMOOTH_ORDER_DISK_ID);

    // Param 7: Detection stride (1 = every frame, N = every Nth frame)
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Detection Stride",
                          1.0, 10.0, 1.0, 10.0, 3.0,
                          PF_Precision_INTEGER, 0, 0,
                          SKIP_FRAMES_DISK_ID);

    // Param 10: Group start - Keypoints
    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPICX("Keypoints", PF_ParamFlag_START_COLLAPSED, GROUP_START_DISK_ID);

    // 17 keypoints × 2 (Point2D + Conf) = 34 params
    for (int k = 0; k < NUM_KEYPOINTS; k++) {
        char name_buf[64];

        // Point param (combined X, Y)
        AEFX_CLR_STRUCT(def);
        snprintf(name_buf, sizeof(name_buf), "%s", kKeypointNames[k]);
        PF_ADD_POINT(name_buf, 50, 50, FALSE, KP_POINT_DISK_ID(k));

        // Confidence param
        AEFX_CLR_STRUCT(def);
        snprintf(name_buf, sizeof(name_buf), "%s_Conf", kKeypointNames[k]);
        PF_ADD_FLOAT_SLIDERX(name_buf,
                              0.0, 1.0, 0.0, 1.0, 0.0,
                              PF_Precision_HUNDREDTHS, 0, 0,
                              KP_CONF_DISK_ID(k));
    }

    // Group end
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(GROUP_END_DISK_ID);

    out_data->num_params = PARAM_NUM_PARAMS;

    DebugLog("ParamsSetup: " + std::to_string(PARAM_NUM_PARAMS) + " params registered");
    return err;
}

// ============================================================================
// Sequence Data lifecycle
// ============================================================================
static PF_Err SequenceSetup(PF_InData* in_data, PF_OutData* out_data,
                             PF_ParamDef* params[], PF_LayerDef* output) {
    PF_Err err = PF_Err_NONE;
    PF_Handle h = PF_NEW_HANDLE(sizeof(UnflatSeqData));
    if (!h) return PF_Err_OUT_OF_MEMORY;

    auto* seq = reinterpret_cast<UnflatSeqData*>(PF_LOCK_HANDLE(h));
    memset(seq, 0, sizeof(UnflatSeqData));
    seq->is_flat = FALSE;
    seq->has_model = FALSE;
    seq->model_input_size = 0;
    PF_UNLOCK_HANDLE(h);

    out_data->sequence_data = h;
    DebugLog("SequenceSetup: created unflat sequence data");
    return err;
}

static PF_Err SequenceSetdown(PF_InData* in_data, PF_OutData* out_data,
                               PF_ParamDef* params[], PF_LayerDef* output) {
    if (in_data->sequence_data) {
        PF_DISPOSE_HANDLE(in_data->sequence_data);
    }
    DebugLog("SequenceSetdown");
    return PF_Err_NONE;
}

static PF_Err SequenceFlatten(PF_InData* in_data, PF_OutData* out_data,
                               PF_ParamDef* params[], PF_LayerDef* output) {
    PF_Err err = PF_Err_NONE;
    if (!in_data->sequence_data) return err;

    auto* seq = reinterpret_cast<UnflatSeqData*>(PF_LOCK_HANDLE(in_data->sequence_data));
    if (!seq || seq->is_flat) {
        PF_UNLOCK_HANDLE(in_data->sequence_data);
        return err;
    }

    FlatSeqData flat;
    memset(&flat, 0, sizeof(flat));
    flat.is_flat = TRUE;
    flat.has_model = seq->has_model;
    memcpy(flat.model_path, seq->model_path, MAX_MODEL_PATH);

    PF_UNLOCK_HANDLE(in_data->sequence_data);
    PF_DISPOSE_HANDLE(in_data->sequence_data);

    PF_Handle h = PF_NEW_HANDLE(sizeof(FlatSeqData));
    if (!h) return PF_Err_OUT_OF_MEMORY;

    auto* dst = reinterpret_cast<FlatSeqData*>(PF_LOCK_HANDLE(h));
    memcpy(dst, &flat, sizeof(FlatSeqData));
    PF_UNLOCK_HANDLE(h);

    out_data->sequence_data = h;
    DebugLog("SequenceFlatten: flattened (model=" + std::string(flat.model_path) + ")");
    return err;
}

static PF_Err SequenceResetup(PF_InData* in_data, PF_OutData* out_data,
                               PF_ParamDef* params[], PF_LayerDef* output) {
    PF_Err err = PF_Err_NONE;
    if (!in_data->sequence_data) return SequenceSetup(in_data, out_data, params, output);

    auto* flat = reinterpret_cast<FlatSeqData*>(PF_LOCK_HANDLE(in_data->sequence_data));
    if (!flat || !flat->is_flat) {
        PF_UNLOCK_HANDLE(in_data->sequence_data);
        return err;
    }

    FlatSeqData saved;
    memcpy(&saved, flat, sizeof(FlatSeqData));
    PF_UNLOCK_HANDLE(in_data->sequence_data);
    PF_DISPOSE_HANDLE(in_data->sequence_data);

    PF_Handle h = PF_NEW_HANDLE(sizeof(UnflatSeqData));
    if (!h) return PF_Err_OUT_OF_MEMORY;

    auto* seq = reinterpret_cast<UnflatSeqData*>(PF_LOCK_HANDLE(h));
    memset(seq, 0, sizeof(UnflatSeqData));
    seq->is_flat = FALSE;
    seq->has_model = saved.has_model;
    memcpy(seq->model_path, saved.model_path, MAX_MODEL_PATH);
    seq->model_input_size = 0;
    PF_UNLOCK_HANDLE(h);

    out_data->sequence_data = h;
    DebugLog("SequenceResetup: unflattened (model=" + std::string(saved.model_path) + ")");
    return err;
}

static PF_Err GetFlattenedSequenceData(PF_InData* in_data, PF_OutData* out_data,
                                         PF_ParamDef* params[], PF_LayerDef* output) {
    PF_Err err = PF_Err_NONE;
    if (!in_data->sequence_data) return err;

    auto* seq = reinterpret_cast<UnflatSeqData*>(PF_LOCK_HANDLE(in_data->sequence_data));
    if (!seq) {
        PF_UNLOCK_HANDLE(in_data->sequence_data);
        return err;
    }

    PF_Handle h = PF_NEW_HANDLE(sizeof(FlatSeqData));
    if (!h) {
        PF_UNLOCK_HANDLE(in_data->sequence_data);
        return PF_Err_OUT_OF_MEMORY;
    }

    auto* flat = reinterpret_cast<FlatSeqData*>(PF_LOCK_HANDLE(h));
    memset(flat, 0, sizeof(FlatSeqData));
    flat->is_flat = TRUE;
    flat->has_model = seq->has_model;
    memcpy(flat->model_path, seq->model_path, MAX_MODEL_PATH);
    PF_UNLOCK_HANDLE(h);

    PF_UNLOCK_HANDLE(in_data->sequence_data);
    out_data->sequence_data = h;
    return err;
}

// ============================================================================
// UserChangedParam — handle button clicks
// ============================================================================
static PF_Err UserChangedParam(PF_InData* in_data, PF_OutData* out_data,
                                PF_ParamDef* params[],
                                const PF_UserChangedParamExtra* which_hit) {
    PF_Err err = PF_Err_NONE;

    if (which_hit->param_index == PARAM_ANALYZE_BUTTON) {
        DebugLog("UserChangedParam: Analyze button clicked");

        if (!in_data->sequence_data) return PF_Err_NONE;

        auto* seq = reinterpret_cast<UnflatSeqData*>(
            PF_LOCK_HANDLE(in_data->sequence_data));
        if (!seq) {
            PF_UNLOCK_HANDLE(in_data->sequence_data);
            return PF_Err_NONE;
        }

        // Read model quality dropdown
        const char* variant = "x";  // default: Best Quality
        PF_ParamDef quality_param;
        AEFX_CLR_STRUCT(quality_param);
        if (!PF_CHECKOUT_PARAM(in_data, PARAM_MODEL_QUALITY,
                                in_data->current_time, in_data->time_step,
                                in_data->time_scale, &quality_param)) {
            if (quality_param.u.pd.value == MODEL_QUALITY_FASTER) {
                variant = "m";
            }
            PF_CHECKIN_PARAM(in_data, &quality_param);
        }

        // Auto-find model (always re-resolve based on quality dropdown)
        {
            std::string defaultModel = FindDefaultModel(variant);
            if (!defaultModel.empty()) {
                strncpy(seq->model_path, defaultModel.c_str(), MAX_MODEL_PATH - 1);
                seq->model_path[MAX_MODEL_PATH - 1] = '\0';
                seq->has_model = TRUE;
                DebugLog("UserChangedParam: auto-found model: " + defaultModel);
            } else {
                DebugLog("UserChangedParam: no model found in ONNX_models/ subfolder");
                PF_UNLOCK_HANDLE(in_data->sequence_data);
                return PF_Err_NONE;
            }
        }

        // Ensure model is loaded
        bool use_gpu = true;
        PF_ParamDef gpu_param;
        AEFX_CLR_STRUCT(gpu_param);
        ERR(PF_CHECKOUT_PARAM(in_data, PARAM_USE_GPU,
                               in_data->current_time, in_data->time_step,
                               in_data->time_scale, &gpu_param));
        if (!err) {
            use_gpu = gpu_param.u.bd.value != 0;
            ERR(PF_CHECKIN_PARAM(in_data, &gpu_param));
        }

        YoloEngine::EnsureSession(seq->model_path, use_gpu);
        PF_UNLOCK_HANDLE(in_data->sequence_data);

        if (!YoloEngine::IsReady()) {
            DebugLog("UserChangedParam: model failed to load");
            return PF_Err_NONE;
        }

        // Read confidence threshold
        float conf_threshold = 0.25f;
        PF_ParamDef conf_param;
        AEFX_CLR_STRUCT(conf_param);
        if (!PF_CHECKOUT_PARAM(in_data, PARAM_CONFIDENCE,
                                in_data->current_time, in_data->time_step,
                                in_data->time_scale, &conf_param)) {
            conf_threshold = static_cast<float>(conf_param.u.fs_d.value);
            PF_CHECKIN_PARAM(in_data, &conf_param);
        }
        DebugLog("Confidence threshold from param: " + std::to_string(conf_threshold));

        // Read smoothing params
        int smooth_window = 5;
        int smooth_order = 2;
        PF_ParamDef sw_param, so_param;
        AEFX_CLR_STRUCT(sw_param);
        AEFX_CLR_STRUCT(so_param);
        if (!PF_CHECKOUT_PARAM(in_data, PARAM_SMOOTH_WINDOW,
                                in_data->current_time, in_data->time_step,
                                in_data->time_scale, &sw_param)) {
            smooth_window = static_cast<int>(sw_param.u.fs_d.value);
            PF_CHECKIN_PARAM(in_data, &sw_param);
        }
        if (!PF_CHECKOUT_PARAM(in_data, PARAM_SMOOTH_ORDER,
                                in_data->current_time, in_data->time_step,
                                in_data->time_scale, &so_param)) {
            smooth_order = static_cast<int>(so_param.u.fs_d.value);
            PF_CHECKIN_PARAM(in_data, &so_param);
        }
        // Ensure window is odd
        if (smooth_window > 1 && smooth_window % 2 == 0) smooth_window++;

        // Read detection stride
        int skip_frames = 1;
        PF_ParamDef sf_param;
        AEFX_CLR_STRUCT(sf_param);
        if (!PF_CHECKOUT_PARAM(in_data, PARAM_SKIP_FRAMES,
                                in_data->current_time, in_data->time_step,
                                in_data->time_scale, &sf_param)) {
            skip_frames = std::max(1, static_cast<int>(sf_param.u.fs_d.value));
            PF_CHECKIN_PARAM(in_data, &sf_param);
        }

        // Run analysis
        err = AnalyzeAndWriteKeyframes(in_data, out_data, conf_threshold, smooth_window, smooth_order, skip_frames);

        out_data->out_flags |= PF_OutFlag_FORCE_RERENDER;
    }

    return err;
}

// ============================================================================
// SmartPreRender — passthrough
// ============================================================================
static PF_Err SmartPreRender(PF_InData* in_data, PF_OutData* out_data,
                              PF_PreRenderExtra* extra) {
    PF_Err err = PF_Err_NONE;
    AEGP_SuiteHandler suites(in_data->pica_basicP);

    PF_RenderRequest req = extra->input->output_request;
    PF_CheckoutResult in_result;

    ERR(extra->cb->checkout_layer(in_data->effect_ref,
        PARAM_INPUT, PARAM_INPUT, &req,
        in_data->current_time, in_data->time_step, in_data->time_scale,
        &in_result));

    extra->output->result_rect = in_result.result_rect;
    extra->output->max_result_rect = in_result.max_result_rect;

    return err;
}

// ============================================================================
// SmartRender — passthrough
// ============================================================================
static PF_Err SmartRender(PF_InData* in_data, PF_OutData* out_data,
                           PF_SmartRenderExtra* extra) {
    PF_Err err = PF_Err_NONE;

    PF_EffectWorld* input_world = nullptr;
    PF_EffectWorld* output_world = nullptr;

    ERR(extra->cb->checkout_layer_pixels(in_data->effect_ref, PARAM_INPUT, &input_world));
    ERR(extra->cb->checkout_output(in_data->effect_ref, &output_world));

    if (!err && input_world && output_world) {
        ERR(PF_COPY(input_world, output_world, NULL, NULL));
    }

    return err;
}

// ============================================================================
// PluginDataEntryFunction2
// ============================================================================
extern "C" DllExport PF_Err PluginDataEntryFunction2(
    PF_PluginDataPtr        inPtr,
    PF_PluginDataCB2        inPluginDataCallBackPtr,
    SPBasicSuite            *inSPBasicSuitePtr,
    const char              *inHostName,
    const char              *inHostVersion)
{
    PF_Err result = PF_Err_INVALID_CALLBACK;

    result = PF_REGISTER_EFFECT_EXT2(
        inPtr,
        inPluginDataCallBackPtr,
        PLUGIN_NAME,
        PLUGIN_MATCH_NAME,
        PLUGIN_CATEGORY,
        AE_RESERVED_INFO,
        "EffectMain",
        "PluginDataEntryFunction2");

    return result;
}

// ============================================================================
// EffectMain — command dispatcher
// ============================================================================
extern "C" DllExport PF_Err EffectMain(
    PF_Cmd      cmd,
    PF_InData   *in_data,
    PF_OutData  *out_data,
    PF_ParamDef *params[],
    PF_LayerDef *output,
    void        *extra)
{
    PF_Err err = PF_Err_NONE;

    try {
        switch (cmd) {
            case PF_Cmd_ABOUT:
                err = About(in_data, out_data, params, output);
                break;
            case PF_Cmd_GLOBAL_SETUP:
                err = GlobalSetup(in_data, out_data, params, output);
                break;
            case PF_Cmd_GLOBAL_SETDOWN:
                err = GlobalSetdown(in_data, out_data, params, output);
                break;
            case PF_Cmd_PARAMS_SETUP:
                err = ParamsSetup(in_data, out_data, params, output);
                break;
            case PF_Cmd_SEQUENCE_SETUP:
                err = SequenceSetup(in_data, out_data, params, output);
                break;
            case PF_Cmd_SEQUENCE_SETDOWN:
                err = SequenceSetdown(in_data, out_data, params, output);
                break;
            case PF_Cmd_SEQUENCE_FLATTEN:
                err = SequenceFlatten(in_data, out_data, params, output);
                break;
            case PF_Cmd_SEQUENCE_RESETUP:
                err = SequenceResetup(in_data, out_data, params, output);
                break;
            case PF_Cmd_GET_FLATTENED_SEQUENCE_DATA:
                err = GetFlattenedSequenceData(in_data, out_data, params, output);
                break;
            case PF_Cmd_USER_CHANGED_PARAM:
                err = UserChangedParam(in_data, out_data, params,
                    reinterpret_cast<const PF_UserChangedParamExtra*>(extra));
                break;
            case PF_Cmd_SMART_PRE_RENDER:
                err = SmartPreRender(in_data, out_data,
                    reinterpret_cast<PF_PreRenderExtra*>(extra));
                break;
            case PF_Cmd_SMART_RENDER:
                err = SmartRender(in_data, out_data,
                    reinterpret_cast<PF_SmartRenderExtra*>(extra));
                break;
            default:
                break;
        }
    } catch (PF_Err& thrown_err) {
        err = thrown_err;
    } catch (const std::exception& e) {
        DebugLog(std::string("EffectMain exception: ") + e.what());
        err = PF_Err_INTERNAL_STRUCT_DAMAGED;
    } catch (...) {
        DebugLog("EffectMain: unknown exception");
        err = PF_Err_INTERNAL_STRUCT_DAMAGED;
    }

    return err;
}
