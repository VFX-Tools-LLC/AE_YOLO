#include "YoloPostprocess.h"
#include <algorithm>
#include <numeric>
#include <cmath>

#ifdef _WIN32
#include <windows.h>
static void DebugLog(const std::string& msg) {
    OutputDebugStringA(("[AE_YOLO] " + msg + "\n").c_str());
}
#elif defined(__APPLE__)
#include <os/log.h>
static void DebugLog(const std::string& msg) {
    os_log(OS_LOG_DEFAULT, "[AE_YOLO] %{public}s", msg.c_str());
}
#else
static void DebugLog(const std::string&) {}
#endif

struct Detection {
    float cx, cy, w, h;    // Bounding box (center x, center y, width, height)
    float confidence;
    float kp_x[NUM_KEYPOINTS];
    float kp_y[NUM_KEYPOINTS];
    float kp_conf[NUM_KEYPOINTS];
};

static float ComputeIoU(const Detection& a, const Detection& b) {
    float a_x1 = a.cx - a.w / 2, a_y1 = a.cy - a.h / 2;
    float a_x2 = a.cx + a.w / 2, a_y2 = a.cy + a.h / 2;
    float b_x1 = b.cx - b.w / 2, b_y1 = b.cy - b.h / 2;
    float b_x2 = b.cx + b.w / 2, b_y2 = b.cy + b.h / 2;

    float inter_x1 = std::max(a_x1, b_x1);
    float inter_y1 = std::max(a_y1, b_y1);
    float inter_x2 = std::min(a_x2, b_x2);
    float inter_y2 = std::min(a_y2, b_y2);

    float inter_w = std::max(0.0f, inter_x2 - inter_x1);
    float inter_h = std::max(0.0f, inter_y2 - inter_y1);
    float inter_area = inter_w * inter_h;

    float area_a = a.w * a.h;
    float area_b = b.w * b.h;
    float union_area = area_a + area_b - inter_area;

    return union_area > 0 ? inter_area / union_area : 0.0f;
}

static std::vector<int> NMS(const std::vector<Detection>& dets, float iou_threshold) {
    std::vector<int> indices(dets.size());
    std::iota(indices.begin(), indices.end(), 0);
    std::sort(indices.begin(), indices.end(), [&](int a, int b) {
        return dets[a].confidence > dets[b].confidence;
    });

    std::vector<bool> suppressed(dets.size(), false);
    std::vector<int> keep;

    for (int idx : indices) {
        if (suppressed[idx]) continue;
        keep.push_back(idx);

        for (int other : indices) {
            if (suppressed[other] || other == idx) continue;
            if (ComputeIoU(dets[idx], dets[other]) > iou_threshold) {
                suppressed[other] = true;
            }
        }
    }

    return keep;
}

// ============================================================================
// Parse YOLO26/v11+ format: [1, N, 57] — already NMS'd
// Layout: [x1, y1, x2, y2, conf, class_id, kp0_x, kp0_y, kp0_conf, ...]
// ============================================================================
static bool ParsePostNMS(
    const float* data, int num_dets, int num_cols,
    const LetterboxInfo& info, float conf_threshold,
    KeypointResult& result)
{
    float best_conf = -1.0f;
    int best_idx = -1;

    for (int i = 0; i < num_dets; i++) {
        const float* row = data + i * num_cols;
        float conf = row[4];
        if (conf >= conf_threshold && conf > best_conf) {
            best_conf = conf;
            best_idx = i;
        }
    }

    if (best_idx < 0) return false;

    const float* row = data + best_idx * num_cols;

    // Keypoints start at index 6 (after x1, y1, x2, y2, conf, class_id)
    for (int k = 0; k < NUM_KEYPOINTS; k++) {
        int base = 6 + k * 3;
        float kp_x = row[base];
        float kp_y = row[base + 1];
        float kp_conf = row[base + 2];

        LetterboxRemap(info, kp_x, kp_y, result.x[k], result.y[k]);
        result.conf[k] = kp_conf;
    }

    return true;
}

// ============================================================================
// Parse YOLOv8 format: [1, 56, 8400] — raw anchors, needs NMS
// Layout per anchor column: [cx, cy, w, h, conf, kp0_x, kp0_y, kp0_conf, ...]
// Data is in [features, anchors] layout: data[feature * num_anchors + anchor]
// ============================================================================
static bool ParseRawAnchors(
    const float* data, int num_features, int num_anchors,
    const LetterboxInfo& info, float conf_threshold,
    KeypointResult& result)
{
    std::vector<Detection> dets;
    dets.reserve(100);

    for (int a = 0; a < num_anchors; a++) {
        float conf = data[4 * num_anchors + a]; // confidence at row 4
        if (conf < conf_threshold) continue;

        Detection det;
        det.cx = data[0 * num_anchors + a];
        det.cy = data[1 * num_anchors + a];
        det.w  = data[2 * num_anchors + a];
        det.h  = data[3 * num_anchors + a];
        det.confidence = conf;

        for (int k = 0; k < NUM_KEYPOINTS; k++) {
            int base = 5 + k * 3; // 5 = 4 bbox + 1 conf
            det.kp_x[k]    = data[base * num_anchors + a];
            det.kp_y[k]    = data[(base + 1) * num_anchors + a];
            det.kp_conf[k] = data[(base + 2) * num_anchors + a];
        }

        dets.push_back(det);
    }

    if (dets.empty()) return false;

    // NMS
    auto keep = NMS(dets, 0.45f);
    if (keep.empty()) return false;

    // Take highest-confidence detection
    const Detection& best = dets[keep[0]];

    // Remap keypoints from model space to original image space
    for (int k = 0; k < NUM_KEYPOINTS; k++) {
        LetterboxRemap(info, best.kp_x[k], best.kp_y[k],
                        result.x[k], result.y[k]);
        result.conf[k] = best.kp_conf[k];
    }

    return true;
}

// ============================================================================
// Main entry point — auto-detects format
// ============================================================================
bool YoloPostprocess(
    const std::vector<float>& raw_output,
    const std::vector<int64_t>& out_shape,
    const LetterboxInfo& info,
    float conf_threshold,
    KeypointResult& result)
{
    memset(&result, 0, sizeof(result));

    if (out_shape.size() < 2) {
        DebugLog("YoloPostprocess: unexpected shape dimension count: " +
                 std::to_string(out_shape.size()));
        return false;
    }

    // Determine format from shape
    // YOLO26+:  [1, N, 57]  where N <= ~300, last dim = 57
    // YOLOv8:   [1, 56, M]  where M >= 1000 (e.g. 8400), second dim = 56
    int ndim = static_cast<int>(out_shape.size());
    int64_t dim1 = out_shape[ndim >= 3 ? 1 : 0];
    int64_t dim2 = out_shape[ndim >= 3 ? 2 : 1];

    // Log format detection once
    static bool s_logged_format = false;
    if (!s_logged_format) {
        DebugLog("YoloPostprocess: shape=[" +
                 (ndim >= 3 ? std::to_string(out_shape[0]) + "," : "") +
                 std::to_string(dim1) + "," + std::to_string(dim2) + "]");
    }

    const float* data = raw_output.data();

    // Heuristic: if last dim is 57 and second dim is small, it's post-NMS format
    if (dim2 == 57 || (dim2 >= 56 && dim2 <= 60 && dim1 <= 1000)) {
        if (!s_logged_format) {
            DebugLog("YoloPostprocess: detected post-NMS format (YOLO26+)");
            s_logged_format = true;
        }
        return ParsePostNMS(data, static_cast<int>(dim1), static_cast<int>(dim2),
                            info, conf_threshold, result);
    }
    // If second dim is 56 and last dim is large, it's raw anchor format
    else if (dim1 == 56 || (dim1 >= 50 && dim1 <= 60 && dim2 >= 1000)) {
        if (!s_logged_format) {
            DebugLog("YoloPostprocess: detected raw anchor format (YOLOv8)");
            s_logged_format = true;
        }
        return ParseRawAnchors(data, static_cast<int>(dim1), static_cast<int>(dim2),
                               info, conf_threshold, result);
    }
    else {
        DebugLog("YoloPostprocess: unrecognized output shape — dim1=" +
                 std::to_string(dim1) + " dim2=" + std::to_string(dim2));
        return false;
    }
}
