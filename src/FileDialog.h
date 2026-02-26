#pragma once

#include <string>

// Show a native Win32 file dialog for .onnx model selection.
// Returns true if user selected a file, false if cancelled.
bool ShowOnnxFileDialog(std::wstring& out_path);
