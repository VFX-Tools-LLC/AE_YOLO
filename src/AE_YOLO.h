#pragma once

#ifdef _WIN32
    #define NOMINMAX
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #include <shlwapi.h>
    #pragma comment(lib, "shlwapi.lib")
#endif

#include <vector>
#include <cmath>
#include <algorithm>
#include <string>
#include <memory>
#include <mutex>
#include <cstring>

#include "AEConfig.h"
#include "entry.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_EffectUI.h"
#include "AE_EffectSuites.h"
#include "AE_Macros.h"
#include "Param_Utils.h"
#include "AE_EffectCBSuites.h"
#include "String_Utils.h"
#include "AE_GeneralPlug.h"
#include "AEFX_SuiteHelper.h"
#include "AEGP_SuiteHandler.h"

// ============================================================================
// Plugin identity
// ============================================================================
#define PLUGIN_NAME         "YOLO Pose"
#define PLUGIN_MATCH_NAME   "YOLO Pose Estimation"
#define PLUGIN_CATEGORY     "AI/ML"
#define MAJOR_VERSION       1
#define MINOR_VERSION       0
#define BUG_VERSION         0
#define BUILD_VERSION       0

#define MAX_MODEL_PATH      1024
#define NUM_KEYPOINTS       17
#define YOLO_INPUT_SIZE     640

// ============================================================================
// 17 COCO keypoint names
// ============================================================================
static const char* const kKeypointNames[NUM_KEYPOINTS] = {
    "Nose",      "LEye",      "REye",      "LEar",      "REar",
    "LShldr",    "RShldr",    "LElbow",    "RElbow",    "LWrist",
    "RWrist",    "LHip",      "RHip",      "LKnee",     "RKnee",
    "LAnkle",    "RAnkle"
};

// ============================================================================
// Parameter IDs — 46 total
// ============================================================================
enum ParamID {
    PARAM_INPUT = 0,
    PARAM_LOAD_MODEL_BUTTON,    // 1
    PARAM_ANALYZE_BUTTON,       // 2
    PARAM_MODEL_QUALITY,        // 3 — popup: Best Quality (x) / Faster (m)
    PARAM_CONFIDENCE,           // 4
    PARAM_USE_GPU,              // 5
    PARAM_SMOOTH_WINDOW,        // 6 — SavGol window size (odd, 1=off)
    PARAM_SMOOTH_ORDER,         // 7 — sample count for smooth() (1–25, odd preferred)
    PARAM_PREVIEW_LINES,        // 8 — checkbox: draw skeleton lines on preview
    PARAM_SKIP_FRAMES,          // 9 — detection stride (1=every frame, N=every Nth)
    PARAM_GROUP_START,          // 10

    // 17 keypoints × 2 (Point, Conf) = 34 params, indices 11–44
    PARAM_KP_FIRST = 11,
    PARAM_KP_LAST  = 44,        // PARAM_KP_FIRST + NUM_KEYPOINTS * 2 - 1

    PARAM_GROUP_END,            // 45
    PARAM_NUM_PARAMS            // 46
};

// Helper: get param index for keypoint k (0–16) position (Point2D)
inline PF_ParamIndex KP_POINT_PARAM(int k) {
    return static_cast<PF_ParamIndex>(PARAM_KP_FIRST + k * 2);
}

// Helper: get param index for keypoint k (0–16) confidence (float)
inline PF_ParamIndex KP_CONF_PARAM(int k) {
    return static_cast<PF_ParamIndex>(PARAM_KP_FIRST + k * 2 + 1);
}

// Disk IDs for params (must be stable across versions)
#define LOAD_MODEL_DISK_ID      1
#define ANALYZE_DISK_ID         2
#define MODEL_QUALITY_DISK_ID   8
#define CONFIDENCE_DISK_ID      3
#define USE_GPU_DISK_ID         4
#define SMOOTH_WINDOW_DISK_ID   6
#define SMOOTH_ORDER_DISK_ID    7
#define GROUP_START_DISK_ID     5
#define PREVIEW_LINES_DISK_ID   9
#define SKIP_FRAMES_DISK_ID     10

// Model quality popup values (1-indexed for AE popups)
#define MODEL_QUALITY_BEST      1   // yolo26x-pose (Best Quality)
#define MODEL_QUALITY_FASTER    2   // yolo26m-pose (Faster)
// Keypoint disk IDs: Point = 100 + k*2, Conf = 100 + k*2 + 1
#define KP_POINT_DISK_ID(k)    (100 + (k) * 2)
#define KP_CONF_DISK_ID(k)     (100 + (k) * 2 + 1)
#define GROUP_END_DISK_ID       200

// ============================================================================
// Sequence Data — flat (serializable) and unflat (runtime)
// ============================================================================
struct FlatSeqData {
    A_Boolean   is_flat;
    A_Boolean   has_model;
    A_u_short   padding;
    char        model_path[MAX_MODEL_PATH];
};

struct UnflatSeqData {
    A_Boolean   is_flat;
    A_Boolean   has_model;
    A_u_short   padding;
    char        model_path[MAX_MODEL_PATH];
    int         model_input_size;   // auto-detected, typically 640
};

// ============================================================================
// COCO skeleton connections (19 bone pairs)
// ============================================================================
static const int kSkeletonPairs[][2] = {
    {0, 1},  {0, 2},           // Nose → Eyes
    {1, 2},                    // LEye ↔ REye
    {1, 3},  {2, 4},           // Eyes → Ears
    {3, 5},  {4, 6},           // Ears → Shoulders
    {5, 6},                    // Shoulders
    {5, 7},  {7, 9},           // Left arm
    {6, 8},  {8, 10},          // Right arm
    {5, 11}, {6, 12},          // Torso
    {11, 12},                  // Hips
    {11, 13}, {13, 15},        // Left leg
    {12, 14}, {14, 16}         // Right leg
};
static const int kNumSkeletonPairs =
    static_cast<int>(sizeof(kSkeletonPairs) / sizeof(kSkeletonPairs[0]));

// ============================================================================
// Pre-render data
// ============================================================================
struct PreRenderData {
    char        model_path[MAX_MODEL_PATH];
};

// ============================================================================
// Keypoint result for one frame
// ============================================================================
struct KeypointResult {
    float x[NUM_KEYPOINTS];
    float y[NUM_KEYPOINTS];
    float conf[NUM_KEYPOINTS];
};

// ============================================================================
// Entry points
// ============================================================================
#ifdef __cplusplus
extern "C" {
#endif

DllExport PF_Err EffectMain(
    PF_Cmd          cmd,
    PF_InData       *in_data,
    PF_OutData      *out_data,
    PF_ParamDef     *params[],
    PF_LayerDef     *output,
    void            *extra);

DllExport PF_Err PluginDataEntryFunction2(
    PF_PluginDataPtr        inPtr,
    PF_PluginDataCB2        inPluginDataCallBackPtr,
    SPBasicSuite            *inSPBasicSuitePtr,
    const char              *inHostName,
    const char              *inHostVersion);

#ifdef __cplusplus
}
#endif
