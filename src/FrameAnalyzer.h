#pragma once

#include "AE_YOLO.h"

// Analyze all frames of the layer, run YOLO pose inference,
// apply SavGol smoothing, and write keypoints as keyframes.
// Called from PF_Cmd_USER_CHANGED_PARAM when Analyze button is clicked.
// conf_threshold: minimum detection confidence (0-1)
// smooth_window: SavGol window size (odd, 1=disabled)
// smooth_order: SavGol polynomial order (1-5)
// Returns PF_Err_NONE on success.
PF_Err AnalyzeAndWriteKeyframes(
    PF_InData* in_data,
    PF_OutData* out_data,
    float conf_threshold = 0.25f,
    int smooth_window = 7,
    int smooth_order = 3,
    int skip_frames = 1);
