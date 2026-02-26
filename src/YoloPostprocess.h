#pragma once

#include "AE_YOLO.h"
#include "Letterbox.h"
#include <vector>

// Process raw YOLO pose output into keypoints for a single frame.
// raw_output: flattened output tensor from the model
// out_shape: tensor shape (e.g. [1, 56, 8400])
// info: letterbox info for coordinate remapping
// conf_threshold: minimum detection confidence
// result: output keypoints in original image pixel coordinates
// Returns true if a valid person detection was found.
bool YoloPostprocess(
    const std::vector<float>& raw_output,
    const std::vector<int64_t>& out_shape,
    const LetterboxInfo& info,
    float conf_threshold,
    KeypointResult& result);
