#pragma once

#include <string>

// Show a native file dialog for .onnx model selection.
// Returns true if user selected a file, false if cancelled.
// out_path receives the file path as a UTF-8 encoded string.
bool ShowOnnxFileDialog(std::string& out_path_utf8);
