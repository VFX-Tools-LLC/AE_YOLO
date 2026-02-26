#pragma once

#include <vector>
#include <cmath>
#include <algorithm>

// Letterbox preprocessing info (needed to remap coordinates back)
struct LetterboxInfo {
    float scale;        // Scale factor applied to original image
    float pad_x;        // Left padding in pixels (in model input space)
    float pad_y;        // Top padding in pixels (in model input space)
    int orig_w;         // Original image width
    int orig_h;         // Original image height
    int input_size;     // Model input size (e.g. 640)
};

// Letterbox resize: scale + pad to target_size x target_size, then convert HWC→CHW.
// Input: ARGB 8-bit pixels (PF_Pixel8 layout: alpha, red, green, blue).
// Output: CHW float [0,1] of size [3 * target_size * target_size].
inline LetterboxInfo LetterboxPreprocess(
    const unsigned char* argb_pixels,   // ARGB 8-bit pixel data
    int width, int height, int rowbytes,
    int target_size,
    std::vector<float>& output_chw)
{
    LetterboxInfo info;
    info.orig_w = width;
    info.orig_h = height;
    info.input_size = target_size;

    // Calculate scale and padding
    info.scale = std::min(
        static_cast<float>(target_size) / width,
        static_cast<float>(target_size) / height);
    int new_w = static_cast<int>(std::round(width * info.scale));
    int new_h = static_cast<int>(std::round(height * info.scale));

    // Use integer division so placement (pad_left/pad_top) and remapping
    // (info.pad_x/pad_y) always agree — avoids sub-pixel systematic error.
    int pad_left = (target_size - new_w) / 2;
    int pad_top  = (target_size - new_h) / 2;
    info.pad_x = static_cast<float>(pad_left);
    info.pad_y = static_cast<float>(pad_top);

    // Reusable scratch buffer — avoids per-frame heap allocation.
    // Sized for the largest expected input (target_size × target_size × 3).
    static std::vector<float> hwc;
    size_t total = static_cast<size_t>(target_size) * target_size;
    hwc.assign(total * 3, 114.0f / 255.0f);

    // Bilinear interpolation: resize original into center of padded buffer
    for (int y = 0; y < new_h; y++) {
        float src_y = y / info.scale;
        int sy0 = static_cast<int>(src_y);
        int sy1 = std::min(sy0 + 1, height - 1);
        float fy = src_y - sy0;

        for (int x = 0; x < new_w; x++) {
            float src_x = x / info.scale;
            int sx0 = static_cast<int>(src_x);
            int sx1 = std::min(sx0 + 1, width - 1);
            float fx = src_x - sx0;

            // AE pixel layout: ARGB (alpha at offset 0, red at 1, green at 2, blue at 3)
            const unsigned char* p00 = argb_pixels + sy0 * rowbytes + sx0 * 4;
            const unsigned char* p01 = argb_pixels + sy0 * rowbytes + sx1 * 4;
            const unsigned char* p10 = argb_pixels + sy1 * rowbytes + sx0 * 4;
            const unsigned char* p11 = argb_pixels + sy1 * rowbytes + sx1 * 4;

            for (int c = 0; c < 3; c++) {
                int ae_offset = c + 1; // skip alpha: R=1, G=2, B=3
                float v00 = p00[ae_offset] / 255.0f;
                float v01 = p01[ae_offset] / 255.0f;
                float v10 = p10[ae_offset] / 255.0f;
                float v11 = p11[ae_offset] / 255.0f;

                float val = v00 * (1 - fx) * (1 - fy) +
                            v01 * fx * (1 - fy) +
                            v10 * (1 - fx) * fy +
                            v11 * fx * fy;

                int dst_y = pad_top + y;
                int dst_x = pad_left + x;
                hwc[(dst_y * target_size + dst_x) * 3 + c] = val;
            }
        }
    }

    // Convert HWC → CHW (output_chw is provided by caller; resize only if needed)
    if (output_chw.size() != total * 3)
        output_chw.resize(total * 3);
    for (int y = 0; y < target_size; y++) {
        for (int x = 0; x < target_size; x++) {
            size_t px = y * target_size + x;
            output_chw[0 * total + px] = hwc[px * 3 + 0]; // R
            output_chw[1 * total + px] = hwc[px * 3 + 1]; // G
            output_chw[2 * total + px] = hwc[px * 3 + 2]; // B
        }
    }

    return info;
}

// Remap a coordinate from model input space back to original image space
inline void LetterboxRemap(const LetterboxInfo& info, float model_x, float model_y,
                            float& orig_x, float& orig_y) {
    orig_x = (model_x - info.pad_x) / info.scale;
    orig_y = (model_y - info.pad_y) / info.scale;
    // Clamp to image bounds
    orig_x = std::max(0.0f, std::min(orig_x, static_cast<float>(info.orig_w - 1)));
    orig_y = std::max(0.0f, std::min(orig_y, static_cast<float>(info.orig_h - 1)));
}
