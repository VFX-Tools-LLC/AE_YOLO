#include "FileDialog.h"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shobjidl.h>

bool ShowOnnxFileDialog(std::string& out_path_utf8) {
    out_path_utf8.clear();

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool com_inited = SUCCEEDED(hr) || hr == S_FALSE;

    IFileOpenDialog* pFileOpen = nullptr;
    hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_ALL,
                          IID_IFileOpenDialog, (void**)&pFileOpen);
    if (FAILED(hr)) {
        if (com_inited) CoUninitialize();
        return false;
    }

    COMDLG_FILTERSPEC fileTypes[] = {
        { L"ONNX Models", L"*.onnx" },
        { L"All Files",   L"*.*"    }
    };
    pFileOpen->SetFileTypes(2, fileTypes);
    pFileOpen->SetFileTypeIndex(1);
    pFileOpen->SetTitle(L"Select YOLO Pose ONNX Model");

    HWND hwnd = GetForegroundWindow();
    hr = pFileOpen->Show(hwnd);

    if (SUCCEEDED(hr)) {
        IShellItem* pItem = nullptr;
        hr = pFileOpen->GetResult(&pItem);
        if (SUCCEEDED(hr)) {
            PWSTR pszPath = nullptr;
            hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszPath);
            if (SUCCEEDED(hr) && pszPath) {
                int utf8len = WideCharToMultiByte(CP_UTF8, 0, pszPath, -1,
                                                   NULL, 0, NULL, NULL);
                if (utf8len > 0) {
                    out_path_utf8.resize(utf8len - 1);
                    WideCharToMultiByte(CP_UTF8, 0, pszPath, -1,
                                        &out_path_utf8[0], utf8len, NULL, NULL);
                }
                CoTaskMemFree(pszPath);
            }
            pItem->Release();
        }
    }

    pFileOpen->Release();
    if (com_inited) CoUninitialize();
    return !out_path_utf8.empty();
}

#endif // _WIN32
