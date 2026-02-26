# AE_YOLO — Development Documentation

## Overview

AE_YOLO is an After Effects plugin that runs YOLO pose estimation on video layer frames and writes the detected 17 COCO body keypoints as keyframed AE parameters. This allows motion tracking data from AI pose estimation to be used natively in AE compositions — keypoints can drive expressions, parent layers, or be exported.

## Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│  After Effects Host                                              │
│                                                                  │
│  ┌─────────────┐    ┌───────────────────┐    ┌────────────────┐ │
│  │  AE_YOLO.cpp│───>│ FrameAnalyzer.cpp │───>│  YoloEngine.cpp│ │
│  │  (plugin    │    │ (render frames,   │    │  (ONNX Runtime │ │
│  │   entry,    │    │  run inference,   │    │   + DirectML)  │ │
│  │   params,   │    │  write keyframes) │    │                │ │
│  │   UI,       │    └────────┬──────────┘    └────────────────┘ │
│  │   skeleton  │             │                                   │
│  │   preview)  │    ┌────────┴──────────┐    ┌────────────────┐ │
│  └─────────────┘    │  Letterbox.h      │    │ YoloPostprocess│ │
│                     │  (ARGB→CHW,       │    │ (parse YOLO    │ │
│                     │   bilinear resize,│    │  output, NMS,  │ │
│                     │   pad, remap)     │    │  remap coords) │ │
│                     └───────────────────┘    └────────────────┘ │
└──────────────────────────────────────────────────────────────────┘
```

### Source Files

| File | Purpose |
|------|---------|
| `src/AE_YOLO.h` | Header: param IDs (46 params), keypoint names, skeleton pairs, data structs |
| `src/AE_YOLO.cpp` | Plugin entry point, `EffectMain` dispatcher, `ParamsSetup`, `UserChangedParam` (button handlers), `SmartRender` (passthrough + skeleton overlay) |
| `src/FrameAnalyzer.h/cpp` | Core analysis engine: renders frames via AEGP, runs YOLO inference, writes keyframes + smoothing expressions |
| `src/YoloEngine.h/cpp` | ONNX Runtime session management with DirectML GPU acceleration |
| `src/YoloPostprocess.h/cpp` | Parses YOLO output tensors, auto-detects format (YOLOv8 raw anchors vs YOLO26+ post-NMS) |
| `src/Letterbox.h` | Letterbox preprocessing: ARGB→CHW conversion, bilinear resize, coordinate remapping |
| `src/FileDialog.h/cpp` | Win32 file open dialog for manual ONNX model selection |
| `resources/AE_YOLOPiPL.r` | PiPL resource descriptor |
| `CMakeLists.txt` | Build configuration (CMake, VS 2022, x64) |
| `build.bat` | One-command build script |

## Build System

### Requirements
- Visual Studio 2022 (v17)
- After Effects SDK (headers + PiPL tooling)
- ONNX Runtime with DirectML (bundled in `onnxruntime/onnxruntime-directml/`)
- CMake 3.20+

### Building
```bash
build.bat
# or manually:
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

### Deployment
The build produces `AE_YOLO.aex` + `onnxruntime.dll`. Both must be placed in AE's plugin directory:
```
C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\
```

ONNX model files go in an `ONNX_models/` subfolder next to the `.aex`:
```
MediaCore/
├── AE_YOLO.aex
├── onnxruntime.dll
└── ONNX_models/
    └── yolo26x-pose.onnx
```

The `install_plugin` CMake target automates this copy.

### Important: CRT Linkage
The plugin uses `/MD` (dynamic CRT) to match ONNX Runtime's linkage. Using `/MT` would cause crashes.

## Plugin Parameters (46 total)

| Index | Name | Type | Description |
|-------|------|------|-------------|
| 0 | Input | Layer | Implicit input layer |
| 1 | Load Model | Button | Manual ONNX model file picker |
| 2 | Analyze | Button | Triggers pose analysis on all frames |
| 3 | Model Quality | Popup | "Best Quality (x)" or "Faster (m)" |
| 4 | Confidence | Float [0,1] | Minimum detection confidence (default 0.25) |
| 5 | Use GPU | Checkbox | Enable DirectML GPU acceleration (default on) |
| 6 | Smooth Window | Float [1,51] | Temporal smoothing window in frames (odd, 1=off, default 5) |
| 7 | Smooth Order | Float [1,5] | Smoothing polynomial order (default 2) |
| 8 | Preview Lines | Checkbox | Draw skeleton overlay on preview |
| 9 | Detection Stride | Float [1,10] | Analyze every Nth frame (default 3) |
| 10 | Group Start | — | "Keypoints" group (starts collapsed) |
| 11–44 | Keypoints | Point2D + Float | 17 keypoints × (position + confidence) |
| 45 | Group End | — | — |

Disk IDs are stable across versions. Keypoint disk IDs use the formula: Point = `100 + k*2`, Conf = `100 + k*2 + 1`.

## How It Works

### 1. Model Loading

When the user clicks **Analyze**, the plugin auto-discovers the ONNX model:
1. Finds the plugin DLL's directory via `GetModuleHandleExW`
2. Searches `ONNX_models/` for files matching `*pose*.onnx`
3. Prefers the variant matching the Model Quality dropdown ("26x" or "26m")
4. Calls `YoloEngine::EnsureSession()` which:
   - Preloads `onnxruntime.dll` from the plugin directory (via `SetDllDirectoryW` + `LoadLibraryExW`)
   - Creates the ORT environment with manual API initialization (`ORT_API_MANUAL_INIT`)
   - Negotiates the API version (tries current version down to v17)
   - Configures DirectML execution provider for GPU, falling back to CPU on failure
   - Auto-detects model input size from the input tensor shape `[N, 3, H, W]`
   - Caches input/output names and pre-allocates the inference buffer

### 2. Frame Rendering (The Critical Part)

`FrameAnalyzer::AnalyzeAndWriteKeyframes()` renders each frame through the AEGP suite:

```
For each frame f in [0, num_frames):
    1. Compute COMP TIME:  comp_time = in_point + f * (1/fps)
    2. Convert to LAYER TIME:  AEGP_ConvertCompToLayerTime(layerH, &comp_time, &render_time)
    3. Create fresh render options:  AEGP_NewFromUpstreamOfEffect(...)
    4. Configure: SetWorldType(8-bit), SetDownsampleFactor(1,1), SetTime(render_time)
    5. Render: AEGP_RenderAndCheckoutLayerFrame(...)
    6. Read pixels: AEGP_GetBaseAddr8(worldH, ...)
    7. Run YOLO inference
    8. Check in frame + dispose render options
```

#### CRITICAL: Comp Time vs Layer Time

**This was the hardest bug to solve.** `AEGP_SetTime` on `LayerRenderOptionsH` expects **layer time** (source-relative), not comp time. When a layer is trimmed (doesn't start at frame 0 of the source footage), comp time and layer time diverge:

```
Source footage:  [=====VISIBLE PORTION=====]
                  ^                         ^
                  Layer in-point            Layer out-point
                  (comp time 0)             (comp time = duration)

Layer time:       Accounts for layer offset, so layer_time = comp_time - offset
                  This maps to the correct position in the source footage.
```

Without this conversion, passing raw comp times causes AE to render from the *beginning* of the source footage (a nearly-static section in our test case), rather than the visible trimmed portion where the person was actually moving.

**The fix** (line ~261–264 of FrameAnalyzer.cpp):
```cpp
// Convert comp time → layer time for rendering
A_Time render_time = {};
suites.LayerSuite8()->AEGP_ConvertCompToLayerTime(layerH, &comp_time, &render_time);
```

Keyframe writing still uses comp time correctly:
```cpp
suites.KeyframeSuite5()->AEGP_AddKeyframes(akH, AEGP_LTimeMode_CompTime, &frame_time, &key_idx);
```

#### Per-Frame Render Options

Each frame creates a fresh `AEGP_LayerRenderOptionsH` rather than reusing one. While not strictly the root cause of the timing bug, this prevents potential state leakage between frames on some AE versions.

### 3. Letterbox Preprocessing

Before inference, each frame is preprocessed (Letterbox.h):
1. **Bilinear resize** to fit within `input_size × input_size` (typically 640×640) while maintaining aspect ratio
2. **Pad** with gray (114/255) to fill the square
3. **Convert** from AE's ARGB 8-bit pixel layout to CHW float32 `[0,1]`
4. **Track** scale + padding offsets for coordinate remapping back to original image space

Key detail: AE pixels are ARGB (alpha=offset 0, R=1, G=2, B=3), not RGBA.

Padding uses integer division for consistency between the forward transform and coordinate remapping:
```cpp
int pad_left = (target_size - new_w) / 2;  // integer, not float
```

### 4. YOLO Inference

`YoloEngine::RunInference()`:
- Creates a CPU tensor from the preprocessed CHW buffer
- Copies input to a dedicated buffer each call so DirectML sees a fresh allocation (prevents stale GPU cache)
- Runs the ONNX session and returns the raw output tensor + shape

### 5. Postprocessing (Format Auto-Detection)

`YoloPostprocess()` handles two YOLO output formats:

| Format | Shape | Layout | Needs NMS? |
|--------|-------|--------|------------|
| YOLO26+ (post-NMS) | `[1, N, 57]` (N≤300) | `[x1, y1, x2, y2, conf, class_id, kp0_x, kp0_y, kp0_conf, ...]` | No |
| YOLOv8 (raw anchors) | `[1, 56, 8400]` | Column-major: `[cx, cy, w, h, conf, kp0_x, kp0_y, kp0_conf, ...]` | Yes |

Format is auto-detected from tensor shape. The highest-confidence person detection is selected. Keypoint coordinates are remapped from model space back to original image coordinates via `LetterboxRemap()`.

### 6. Keyframe Writing

For each of the 17 keypoints, the plugin writes:
- **Point2D keyframes** (X, Y position in layer pixel coordinates)
- **Confidence keyframes** (per-keypoint confidence 0–1)

Keyframes are only written for frames where YOLO detected a person (sparse keyframes). AE linearly interpolates between them.

Key AEGP pattern for batch keyframe insertion:
```
AEGP_StartAddKeyframes → loop { AEGP_AddKeyframes + AEGP_SetAddKeyframe } → AEGP_EndAddKeyframes
```

All keyframe operations are wrapped in a single undo group (`AEGP_StartUndoGroup` / `AEGP_EndUndoGroup`). The undo group only wraps keyframe writing, NOT frame rendering, to avoid locking AE's undo system during the long render phase.

### 7. Temporal Smoothing

Instead of baking smoothed values into keyframes (which would be destructive), the plugin applies AE's native `smooth()` expression to each keypoint stream:

```cpp
// e.g., smooth(0.2083, 5) for a 5-frame window at 24fps
wchar_t expr[128];
swprintf(expr, 128, L"smooth(%.4f, 5)", width_sec);
suites.StreamSuite6()->AEGP_SetExpression(g_aegp_plugin_id, streamH,
    reinterpret_cast<const A_UTF16Char*>(expr));
suites.StreamSuite6()->AEGP_SetExpressionState(g_aegp_plugin_id, streamH, TRUE);
```

Note: `AEGP_SetExpression` expects `A_UTF16Char*` (wide strings on Windows), hence the `wchar_t` buffer and reinterpret_cast.

This approach lets the user:
- Adjust smoothing after analysis by changing the Smooth Window parameter
- Disable smoothing by setting Smooth Window to 1
- See raw keyframe values vs smoothed output separately in the Graph Editor

### 8. Detection Stride

For long clips, running YOLO on every frame is slow. The Detection Stride parameter (default 3) runs inference every Nth frame. The first and last frames are always processed. Skipped frames have no keyframes — AE's interpolation fills the gaps.

Total YOLO calls = `ceil(num_frames / stride)`, so stride=3 cuts inference time by ~67%.

### 9. Skeleton Preview

When "Preview Lines" is enabled, `SmartRender` draws a 2-pixel-wide green skeleton overlay by:
1. Reading all 17 keypoint positions and confidences at the current time
2. Drawing Bresenham lines between connected joints (19 bone pairs) where both endpoints have confidence > 0.01
3. Scaling coordinates by the current downsample factor for half/quarter-res previews

The skeleton is drawn directly into the output `PF_EffectWorld` after copying the input passthrough.

## Key Debugging Lessons

### Problem: "Same frame every time"
**Symptom**: YOLO tracking appeared to detect the same pose for every frame, with coordinates barely moving (~15px over 12 frames on a 2960×3840 image).

**Root cause**: The layer was trimmed — it showed a portion of a longer clip, not starting at frame 0 of the source footage. Passing comp time directly to `AEGP_SetTime` on `LayerRenderOptionsH` caused AE to render from the source footage start (a nearly-static section), not the visible trimmed region.

**Diagnosis path**:
1. Added center pixel diagnostics → pixels were nearly identical
2. Created per-frame render options → no change
3. Added FNV-1a frame hashing → hashes were different, proving AE rendered different frames
4. The different hashes but nearly-identical pixels proved the issue was *which* frames were being rendered, not a caching/reuse bug
5. User mentioned "I'm running this on a portion of a clip" → immediately pointed to comp time vs layer time
6. `AEGP_ConvertCompToLayerTime` fixed it

**Key takeaway**: When using AEGP render APIs, always convert comp time to layer time with `AEGP_ConvertCompToLayerTime()`. Keyframe APIs (`AEGP_AddKeyframes`) still use comp time with `AEGP_LTimeMode_CompTime`.

### Problem: Skeleton preview crash
**Cause**: `kNumSkeletonPairs` was computed incorrectly (off by 3 from the actual array size), causing out-of-bounds reads in the skeleton drawing loop.

**Fix**: Compute from `sizeof(kSkeletonPairs) / sizeof(kSkeletonPairs[0])`.

### Problem: DirectML stale GPU cache
**Symptom**: Inference produced identical results for different frames when using GPU.

**Cause**: Reusing the same host buffer for the input tensor caused DirectML to serve a stale GPU-side copy.

**Fix**: Copy input data to `g_input_buffer` via `.assign()` each call, creating a fresh host-side buffer that forces DirectML to re-upload.

### Problem: Letterbox coordinate drift at high resolution
**Symptom**: At 2960×3840, keypoints had a systematic offset.

**Fix**: Use integer division for padding (`(target_size - new_w) / 2`) in both the forward transform and the remap function, ensuring they always agree.

## AEGP Suites Used

| Suite | Purpose |
|-------|---------|
| `AEGP_PFInterfaceSuite1` | Get layer and effect refs from `PF_InData` |
| `AEGP_LayerSuite8` | Layer timing (in-point, duration, offset, flags, stretch), comp↔layer time conversion |
| `AEGP_LayerRenderOptionsSuite1` | Create render options, set time/format, render frames |
| `AEGP_RenderSuite5` | `RenderAndCheckoutLayerFrame`, `CheckinFrame`, `GetReceiptWorld` |
| `AEGP_WorldSuite3` | Get rendered frame dimensions, row bytes, pixel base address |
| `AEGP_CompSuite11` | Get composition framerate |
| `AEGP_StreamSuite6` | Get effect streams, set expressions |
| `AEGP_KeyframeSuite5` | Batch keyframe insertion |
| `AEGP_EffectSuite4` | Dispose effect refs |
| `AEGP_ItemSuite9` | Get source item duration/dimensions |
| `AEGP_UtilitySuite6` | AEGP registration, undo groups |
| `PF_AppSuite6` | Progress dialog (create, update, dispose) |

## Time Handling Reference

```
Comp Time:  Timeline position in the composition
Layer Time: Position relative to the layer's source (accounts for offset, stretch, time remap)

For rendering:    Use LAYER TIME  → AEGP_SetTime(renderOptsH, layer_time)
For keyframes:    Use COMP TIME   → AEGP_AddKeyframes(akH, AEGP_LTimeMode_CompTime, &comp_time, ...)

Conversion:       AEGP_ConvertCompToLayerTime(layerH, &comp_time, &layer_time)

Time representation:  A_Time { A_long value; A_u_long scale; }
                      Actual seconds = value / scale
```
