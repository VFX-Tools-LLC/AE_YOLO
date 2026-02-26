# AE YOLO Pose

An After Effects plugin that uses YOLO AI pose estimation to automatically detect and track 17 body keypoints in video footage. Keypoints are written as native AE parameters with full keyframe support, enabling expression-driven motion graphics, character rigging, and motion analysis workflows.

This project utilizes [Ultralytics YOLO](https://github.com/ultralytics/ultralytics), licensed under AGPL-3.0. For more details on Ultralytics licensing and contributing guidelines, see the [Ultralytics Contributing Guide](https://docs.ultralytics.com/help/contributing/).

## Features

- **17 COCO Body Keypoints** — Nose, eyes, ears, shoulders, elbows, wrists, hips, knees, ankles
- **GPU Accelerated** — DirectML on Windows, CoreML on macOS, with automatic CPU fallback
- **Native AE Integration** — Keypoints written as keyframed Point2D parameters; use with expressions, parent layers, or export
- **Non-destructive Smoothing** — Built-in `smooth()` expression with adjustable window and sample count
- **Skeleton Preview** — Real-time 2D skeleton overlay in the comp viewer
- **Detection Stride** — Analyze every Nth frame for faster processing on long clips
- **Model Auto-discovery** — Automatically finds ONNX models placed next to the plugin
- **ScriptUI Panel** — Companion script creates null layers expression-linked to each keypoint
- **Multiple Model Support** — Choose between high-quality (YOLO 26x) and faster (YOLO 26m) variants

## Requirements

- Adobe After Effects 2024 or later
- Windows 10/11 (x64) or macOS 12+ (Intel / Apple Silicon)
- A YOLO pose estimation model in ONNX format (e.g. `yolo11x-pose.onnx`)

## Installation

### Pre-built Binaries

1. Download the latest release for your platform from the [Releases](../../releases) page.
2. Copy the plugin files to your After Effects plugins directory:
   - **Windows:** `C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\`
     - `AE_YOLO.aex`
     - `onnxruntime.dll`
   - **macOS:** `/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore/`
     - `AE_YOLO.plugin`
3. Create an `ONNX_models` subfolder in the same directory and place your YOLO pose ONNX model inside:
   ```
   MediaCore/
   ├── AE_YOLO.aex  (or AE_YOLO.plugin on macOS)
   ├── onnxruntime.dll  (Windows only)
   └── ONNX_models/
       └── yolo11x-pose.onnx
   ```
4. Restart After Effects.

### Getting a YOLO Pose Model

Export a pose model to ONNX with [Ultralytics](https://docs.ultralytics.com/tasks/pose/):

```python
from ultralytics import YOLO

model = YOLO("yolo11x-pose.pt")   # best quality
# model = YOLO("yolo11m-pose.pt") # faster alternative
model.export(format="onnx", opset=17, simplify=True)
```

Copy the resulting `.onnx` file into the `ONNX_models/` folder next to the plugin.

## Building from Source

### Prerequisites

| Dependency | Where to get it |
|---|---|
| **CMake 3.20+** | [cmake.org](https://cmake.org/download/) |
| **After Effects SDK** | [Adobe Developer Console](https://developer.adobe.com/) |
| **ONNX Runtime** (Windows) | [onnxruntime DirectML release](https://github.com/microsoft/onnxruntime/releases) — extract into `onnxruntime/onnxruntime-directml/` |
| **ONNX Runtime** (macOS) | [onnxruntime macOS release](https://github.com/microsoft/onnxruntime/releases) — extract into `onnxruntime/onnxruntime-osx/` |

### Windows

**Requires:** Visual Studio 2022 with the **Desktop development with C++** workload.

```bash
# Configure (set your AE SDK path)
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64 ^
    -DAESDK_ROOT="C:/path/to/AfterEffectsSDK/Examples"

# Build
cmake --build . --config Release
```

Or use the one-liner build script (edit `AESDK_ROOT` in `CMakeLists.txt` first):

```bash
build.bat
```

Output: `build/Release/AE_YOLO.aex` + `build/Release/onnxruntime.dll`

### macOS

**Requires:** Xcode 14+ with Command Line Tools.

```bash
# Configure (set your AE SDK path)
mkdir build && cd build
cmake .. -DAESDK_ROOT="/path/to/AfterEffectsSDK/Examples"

# Build
cmake --build . --config Release
```

Output: `build/Release/AE_YOLO.plugin`

### Install to After Effects

```bash
# Windows (run as administrator)
cmake --build build --target install_plugin --config Release

# macOS
sudo cp -R build/Release/AE_YOLO.plugin \
    "/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore/"
```

## Usage

### Basic Workflow

1. Apply the **YOLO Pose** effect (found under **AI/ML**) to a video layer.
2. Set **Model Quality** to *Best Quality (x)* or *Faster (m)*.
3. Adjust **Confidence** threshold and **Detection Stride** as needed.
4. Click **Analyze**. The plugin renders every frame, runs YOLO inference, and writes keypoints.
5. Expand the **Keypoints** group in the Effect Controls to see all 17 body points.
6. Toggle **Preview Lines** to overlay the skeleton on the comp viewer.

### Parameters

| Parameter | Description |
|---|---|
| **Load Model** | Manually browse for an ONNX model file |
| **Analyze** | Run pose detection on all frames and write keyframes |
| **Model Quality** | Best Quality (26x) / Faster (26m) |
| **Confidence** | Minimum detection confidence, 0 – 1 (default 0.25) |
| **Use GPU** | Enable GPU acceleration (DirectML on Windows, CoreML on macOS) |
| **Smooth Window** | Temporal smoothing window in frames (1 = off, default 5) |
| **Smooth Samples** | Sample count for the `smooth()` expression (default 5) |
| **Preview Lines** | Draw skeleton overlay on the comp viewer |
| **Detection Stride** | Analyze every Nth frame (default 3; 1 = every frame) |

### ScriptUI Panel

A companion ExtendScript panel (`scripts/AE_YOLO_Panel.jsx`) can create 17 null layers expression-linked to the detected keypoints:

1. Copy `AE_YOLO_Panel.jsx` to your AE Scripts folder.
2. In AE, open **Window > AE_YOLO_Panel.jsx**.
3. Select a layer that has the YOLO Pose effect applied.
4. Click **Create Nulls from YOLO Pose**.

Each null's position tracks a keypoint, and its opacity reflects detection confidence.

## Architecture

```
┌──────────────┐    ┌────────────────┐    ┌──────────────────┐
│  AE_YOLO.cpp │───▶│ FrameAnalyzer  │───▶│   YoloEngine     │
│  (UI + cmds) │    │ (AEGP render   │    │ (ONNX Runtime    │
│              │    │  + keyframes)  │    │  + GPU accel)    │
└──────────────┘    └───────┬────────┘    └────────┬─────────┘
                            │                      │
                    ┌───────▼────────┐    ┌────────▼─────────┐
                    │  Letterbox.h   │    │ YoloPostprocess  │
                    │ (resize + pad) │    │ (parse output)   │
                    └────────────────┘    └──────────────────┘
```

See [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) for detailed technical documentation including time handling, coordinate remapping math, and debugging lessons.

## License

This project is licensed under the **GNU Affero General Public License v3.0 (AGPL-3.0)**. See [LICENSE](LICENSE) for the full text.

This project uses [Ultralytics YOLO](https://github.com/ultralytics/ultralytics), which is also licensed under AGPL-3.0. If you cannot comply with the AGPL-3.0 terms, Ultralytics offers an [Enterprise License](https://www.ultralytics.com/license) as an alternative.

## Acknowledgments

- [Ultralytics YOLO](https://github.com/ultralytics/ultralytics) — State-of-the-art pose estimation
- [ONNX Runtime](https://github.com/microsoft/onnxruntime) — Cross-platform ML inference engine
- [Adobe After Effects SDK](https://developer.adobe.com/) — Plugin development framework
