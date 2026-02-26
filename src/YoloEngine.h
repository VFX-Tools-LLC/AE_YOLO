#pragma once

#include <vector>

namespace YoloEngine {

    // Ensure a session is loaded for the given model path + GPU preference.
    // Thread-safe. If the model is already loaded, does nothing.
    void EnsureSession(const char* model_path_utf8, bool use_gpu);

    // Check if a model is currently loaded and ready for inference.
    bool IsReady();

    // Get model input size (e.g. 640). Returns 0 if not ready.
    int GetInputSize();

    // Run inference on a single preprocessed image.
    // input_chw: [3 * input_size * input_size] float32, values in [0,1], CHW layout
    // raw_output: receives the raw model output tensor (flattened)
    // out_shape: receives the output tensor shape
    // Returns true on success.
    bool RunInference(const float* input_chw,
                      std::vector<float>& raw_output,
                      std::vector<int64_t>& out_shape);

    // Cleanup all ONNX Runtime resources.
    void Shutdown();
}
