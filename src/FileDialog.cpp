#include "FileDialog.h"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shobjidl.h>
#endif

bool ShowOnnxFileDialog(std::wstring& out_path) {
#ifdef _WIN32
    out_path.clear();

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
                out_path = pszPath;
                CoTaskMemFree(pszPath);
            }
            pItem->Release();
        }
    }

    pFileOpen->Release();
    if (com_inited) CoUninitialize();
    return !out_path.empty();
#else
    return false;
#endif
}
