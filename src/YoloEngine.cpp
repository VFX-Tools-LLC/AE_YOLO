#include "YoloEngine.h"

#define ORT_API_MANUAL_INIT
#include "onnxruntime_cxx_api.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <algorithm>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")
#endif

// Forward-declare DirectML provider
extern "C" OrtStatusPtr ORT_API_CALL OrtSessionOptionsAppendExecutionProvider_DML(
    _In_ OrtSessionOptions* options, int device_id);

// ============================================================================
// Globals
// ============================================================================
static std::unique_ptr<Ort::Env>            g_env;
static std::unique_ptr<Ort::Session>        g_session;
static std::unique_ptr<Ort::SessionOptions> g_options;
static std::string                          g_current_model_path;
static bool                                 g_current_use_gpu = true;
static bool                                 g_initialized     = false;
static bool                                 g_session_ready   = false;
static int                                  g_input_size      = 640;
static std::once_flag                       g_init_flag;

// Cached per-session inference state (avoids per-call ORT allocations)
static std::string                          g_input_name;
static std::string                          g_output_name;
static std::vector<float>                   g_input_buffer;   // reused each inference call

static std::mutex& GetMutex() {
    static std::mutex mtx;
    return mtx;
}

// ============================================================================
// Debug logging
// ============================================================================
#ifdef _WIN32
static void DebugLog(const std::string& msg) {
    OutputDebugStringA(("[AE_YOLO] " + msg + "\n").c_str());
}
#else
static void DebugLog(const std::string&) {}
#endif

// ============================================================================
// DLL path helpers
// ============================================================================
#ifdef _WIN32
static std::wstring GetPluginDirectory() {
    wchar_t dllPath[MAX_PATH] = {};
    HMODULE hModule = NULL;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&GetPluginDirectory),
        &hModule);
    if (hModule) {
        GetModuleFileNameW(hModule, dllPath, MAX_PATH);
        PathRemoveFileSpecW(dllPath);
        return std::wstring(dllPath);
    }
    return L"";
}

static bool PreloadDlls() {
    std::wstring dir = GetPluginDirectory();
    if (dir.empty()) {
        DebugLog("PreloadDlls: could not determine plugin directory");
        return false;
    }

    SetDllDirectoryW(dir.c_str());
    AddDllDirectory(dir.c_str());

    std::wstring ortPath = dir + L"\\onnxruntime.dll";
    HMODULE hOrt = LoadLibraryExW(ortPath.c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!hOrt) {
        DebugLog("PreloadDlls: failed to load onnxruntime.dll");
        return false;
    }
    DebugLog("PreloadDlls: loaded onnxruntime.dll from plugin directory");
    return true;
}
#endif

// ============================================================================
// One-time initialization
// ============================================================================
static void InitializeInternal() {
    try {
#ifdef _WIN32
        if (!PreloadDlls()) {
            DebugLog("InitializeInternal: DLL preload failed");
            return;
        }
#endif
        const OrtApiBase* api_base = OrtGetApiBase();
        if (!api_base) {
            DebugLog("InitializeInternal: OrtGetApiBase returned null");
            return;
        }

        const OrtApi* api = nullptr;
        int api_version = ORT_API_VERSION;
        while (api_version >= 17 && !api) {
            api = api_base->GetApi(static_cast<uint32_t>(api_version));
            if (!api) api_version--;
        }
        if (!api) {
            DebugLog("InitializeInternal: could not find compatible ORT API (tried down to v17)");
            return;
        }
        DebugLog("InitializeInternal: using ORT API version " + std::to_string(api_version));

        Ort::Global<void>::api_ = api;

        OrtEnv* raw_env = nullptr;
        OrtStatus* status = api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "AE_YOLO", &raw_env);
        if (status) {
            const char* msg = api->GetErrorMessage(status);
            DebugLog(std::string("InitializeInternal: CreateEnv failed: ") + (msg ? msg : "unknown"));
            api->ReleaseStatus(status);
            return;
        }

        g_env = std::make_unique<Ort::Env>(raw_env);
        g_initialized = true;
        DebugLog("InitializeInternal: ONNX Runtime environment created");

    } catch (const std::exception& e) {
        DebugLog(std::string("InitializeInternal exception: ") + e.what());
    } catch (...) {
        DebugLog("InitializeInternal: unknown exception");
    }
}

// ============================================================================
// Public API
// ============================================================================
void YoloEngine::EnsureSession(const char* model_path_utf8, bool use_gpu) {
    std::lock_guard<std::mutex> lock(GetMutex());

    std::call_once(g_init_flag, InitializeInternal);
    if (!g_initialized) return;

    if (g_session_ready &&
        g_current_model_path == model_path_utf8 &&
        g_current_use_gpu == use_gpu) {
        return;
    }

    g_session.reset();
    g_options.reset();
    g_session_ready = false;

    DebugLog(std::string("EnsureSession: loading model: ") + model_path_utf8);

    try {
        g_options = std::make_unique<Ort::SessionOptions>();
        g_options->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        bool gpu_ok = false;
        if (use_gpu) {
            try {
                g_options->DisableMemPattern();
                g_options->SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);

                OrtStatus* dml_status = OrtSessionOptionsAppendExecutionProvider_DML(
                    *g_options, 0);
                if (dml_status) {
                    const char* err = Ort::Global<void>::api_->GetErrorMessage(dml_status);
                    DebugLog(std::string("DirectML failed: ") + (err ? err : "unknown"));
                    Ort::Global<void>::api_->ReleaseStatus(dml_status);
                } else {
                    gpu_ok = true;
                    DebugLog("EnsureSession: DirectML execution provider added (device 0)");
                }
            } catch (const Ort::Exception& e) {
                DebugLog(std::string("DirectML exception: ") + e.what());
            }
        }

        if (!gpu_ok) {
            g_options = std::make_unique<Ort::SessionOptions>();
            g_options->SetIntraOpNumThreads(4);
            g_options->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            DebugLog("EnsureSession: using CPU execution provider");
        }

        int wlen = MultiByteToWideChar(CP_UTF8, 0, model_path_utf8, -1, NULL, 0);
        std::wstring wide_path(static_cast<size_t>(wlen), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, model_path_utf8, -1, &wide_path[0], wlen);

        g_session = std::make_unique<Ort::Session>(*g_env, wide_path.c_str(), *g_options);

        // Auto-detect input size from model shape [N, 3, H, W]
        Ort::TypeInfo input_info = g_session->GetInputTypeInfo(0);
        auto tensor_info = input_info.GetTensorTypeAndShapeInfo();
        auto shape = tensor_info.GetShape();
        if (shape.size() == 4 && shape[2] > 0 && shape[3] > 0) {
            g_input_size = static_cast<int>(shape[2]);
            DebugLog("EnsureSession: input size from model = " + std::to_string(g_input_size));
        } else {
            g_input_size = 640;
            DebugLog("EnsureSession: using default input size 640");
        }

        // Cache input/output names to avoid per-call ORT allocation
        {
            Ort::AllocatorWithDefaultOptions alloc;
            g_input_name  = g_session->GetInputNameAllocated(0, alloc).get();
            g_output_name = g_session->GetOutputNameAllocated(0, alloc).get();
        }

        // Pre-allocate inference input buffer
        g_input_buffer.assign(static_cast<size_t>(3) * g_input_size * g_input_size, 0.0f);

        g_current_model_path = model_path_utf8;
        g_current_use_gpu = use_gpu;
        g_session_ready = true;
        DebugLog("EnsureSession: model loaded successfully");

    } catch (const Ort::Exception& e) {
        DebugLog(std::string("EnsureSession failed: ") + e.what());
        g_session.reset();
        g_options.reset();
        g_session_ready = false;
    } catch (const std::exception& e) {
        DebugLog(std::string("EnsureSession exception: ") + e.what());
        g_session.reset();
        g_options.reset();
        g_session_ready = false;
    }
}

bool YoloEngine::IsReady() {
    return g_session_ready;
}

int YoloEngine::GetInputSize() {
    return g_session_ready ? g_input_size : 0;
}

bool YoloEngine::RunInference(const float* input_chw,
                               std::vector<float>& raw_output,
                               std::vector<int64_t>& out_shape) {
    if (!g_session_ready || !g_session) return false;

    try {
        static Ort::MemoryInfo mem_info =
            Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeCPUInput);

        size_t tensor_size = static_cast<size_t>(3) * g_input_size * g_input_size;
        std::vector<int64_t> input_shape = {1, 3, g_input_size, g_input_size};

        // Copy input to a dedicated buffer so DirectML sees a fresh allocation
        // each call and doesn't serve a stale GPU-side cache of the previous frame.
        g_input_buffer.assign(input_chw, input_chw + tensor_size);

        Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
            mem_info,
            g_input_buffer.data(),
            tensor_size,
            input_shape.data(),
            input_shape.size());

        const char* input_names[]  = { g_input_name.c_str() };
        const char* output_names[] = { g_output_name.c_str() };

        auto outputs = g_session->Run(
            Ort::RunOptions{nullptr},
            input_names, &input_tensor, 1,
            output_names, 1);

        // Get output tensor info
        auto type_info = outputs[0].GetTensorTypeAndShapeInfo();
        out_shape = type_info.GetShape();
        size_t out_count = type_info.GetElementCount();

        const float* out_data = outputs[0].GetTensorData<float>();
        raw_output.assign(out_data, out_data + out_count);

        return true;
    } catch (const Ort::Exception& e) {
        DebugLog(std::string("RunInference failed: ") + e.what());
        return false;
    } catch (...) {
        DebugLog("RunInference: unknown exception");
        return false;
    }
}

void YoloEngine::Shutdown() {
    std::lock_guard<std::mutex> lock(GetMutex());
    g_session.reset();
    g_options.reset();
    g_env.reset();
    g_session_ready = false;
    g_initialized = false;
    DebugLog("Shutdown: ONNX Runtime resources released");
}
