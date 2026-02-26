#pragma once

#include <vector>
#include <cmath>
#include <algorithm>

// Savitzky-Golay smoothing filter (header-only)
// Fits a polynomial of degree `poly_order` to a sliding window of `window_size`
// points, evaluating the polynomial at the center point.

namespace SavGol {

// Solve a small least-squares system using normal equations.
// Fits polynomial of degree `order` to points (-half, ..., 0, ..., +half)
// Returns the smoothing coefficients for the center value.
inline std::vector<double> ComputeCoefficients(int window_size, int poly_order) {
    int half = window_size / 2;
    int n = window_size;
    int m = poly_order + 1;

    // Build Vandermonde matrix J[n][m] where J[i][j] = x_i^j
    // x_i = i - half (centered at 0)
    std::vector<std::vector<double>> J(n, std::vector<double>(m, 0.0));
    for (int i = 0; i < n; i++) {
        double x = static_cast<double>(i - half);
        double xp = 1.0;
        for (int j = 0; j < m; j++) {
            J[i][j] = xp;
            xp *= x;
        }
    }

    // Compute J^T * J (m x m)
    std::vector<std::vector<double>> JtJ(m, std::vector<double>(m, 0.0));
    for (int i = 0; i < m; i++) {
        for (int j = 0; j < m; j++) {
            double sum = 0.0;
            for (int k = 0; k < n; k++) {
                sum += J[k][i] * J[k][j];
            }
            JtJ[i][j] = sum;
        }
    }

    // Invert JtJ via Gauss-Jordan elimination
    std::vector<std::vector<double>> inv(m, std::vector<double>(m, 0.0));
    for (int i = 0; i < m; i++) inv[i][i] = 1.0;

    auto aug = JtJ; // copy
    for (int col = 0; col < m; col++) {
        // Partial pivoting
        int pivot = col;
        for (int row = col + 1; row < m; row++) {
            if (std::abs(aug[row][col]) > std::abs(aug[pivot][col]))
                pivot = row;
        }
        std::swap(aug[col], aug[pivot]);
        std::swap(inv[col], inv[pivot]);

        double diag = aug[col][col];
        if (std::abs(diag) < 1e-15) continue;

        for (int j = 0; j < m; j++) {
            aug[col][j] /= diag;
            inv[col][j] /= diag;
        }

        for (int row = 0; row < m; row++) {
            if (row == col) continue;
            double factor = aug[row][col];
            for (int j = 0; j < m; j++) {
                aug[row][j] -= factor * aug[col][j];
                inv[row][j] -= factor * inv[col][j];
            }
        }
    }

    // Compute (JtJ)^-1 * Jt — row 0 gives the smoothing coefficients
    // (we only need the value at center, which is the 0th-order coefficient)
    // Coefficients = row 0 of (JtJ)^-1 * J^T
    std::vector<double> coeffs(n, 0.0);
    for (int i = 0; i < n; i++) {
        double sum = 0.0;
        for (int j = 0; j < m; j++) {
            sum += inv[0][j] * J[i][j];
        }
        coeffs[i] = sum;
    }

    return coeffs;
}

// Apply Savitzky-Golay smoothing to a 1D signal in-place.
// window_size must be odd and >= 3.
// poly_order must be < window_size.
inline void Smooth(std::vector<float>& signal, int window_size, int poly_order) {
    int n = static_cast<int>(signal.size());
    if (n < 3) return;

    // Clamp window_size to signal length
    if (window_size > n) window_size = n;
    // Ensure odd
    if (window_size % 2 == 0) window_size--;
    if (window_size < 3) return;
    // Clamp poly_order
    if (poly_order >= window_size) poly_order = window_size - 1;
    if (poly_order < 1) poly_order = 1;

    int half = window_size / 2;
    auto coeffs = ComputeCoefficients(window_size, poly_order);

    std::vector<float> smoothed(n);

    for (int i = 0; i < n; i++) {
        double val = 0.0;
        for (int j = 0; j < window_size; j++) {
            int idx = i - half + j;
            // Mirror boundary handling
            if (idx < 0) idx = -idx;
            if (idx >= n) idx = 2 * (n - 1) - idx;
            idx = std::max(0, std::min(idx, n - 1));
            val += coeffs[j] * signal[idx];
        }
        smoothed[i] = static_cast<float>(val);
    }

    signal = smoothed;
}

// Apply SavGol smoothing to keypoint tracks, respecting confidence.
// Only smooths positions where confidence is above a threshold.
// Gaps (low confidence) are interpolated before smoothing.
inline void SmoothKeypoints(
    std::vector<float>& x_track,    // x[num_frames]
    std::vector<float>& y_track,    // y[num_frames]
    const std::vector<float>& conf_track, // conf[num_frames] (not modified)
    const std::vector<bool>& valid_frames,
    int window_size, int poly_order,
    float conf_min = 0.1f)
{
    int n = static_cast<int>(x_track.size());
    if (n < 3) return;

    // Build mask of valid points
    std::vector<bool> ok(n, false);
    for (int i = 0; i < n; i++) {
        ok[i] = valid_frames[i] && conf_track[i] >= conf_min;
    }

    // Simple linear interpolation to fill gaps before smoothing
    // Find first and last valid
    int first = -1, last = -1;
    for (int i = 0; i < n; i++) {
        if (ok[i]) { if (first < 0) first = i; last = i; }
    }
    if (first < 0) return; // no valid frames

    // Fill gaps with linear interpolation
    int prev_valid = first;
    for (int i = first + 1; i <= last; i++) {
        if (ok[i]) {
            // Interpolate between prev_valid and i
            if (i - prev_valid > 1) {
                for (int j = prev_valid + 1; j < i; j++) {
                    float t = static_cast<float>(j - prev_valid) / (i - prev_valid);
                    x_track[j] = x_track[prev_valid] * (1 - t) + x_track[i] * t;
                    y_track[j] = y_track[prev_valid] * (1 - t) + y_track[i] * t;
                }
            }
            prev_valid = i;
        }
    }

    // Extend edges (hold first/last value)
    for (int i = 0; i < first; i++) {
        x_track[i] = x_track[first];
        y_track[i] = y_track[first];
    }
    for (int i = last + 1; i < n; i++) {
        x_track[i] = x_track[last];
        y_track[i] = y_track[last];
    }

    // Apply SavGol smoothing
    Smooth(x_track, window_size, poly_order);
    Smooth(y_track, window_size, poly_order);
}

} // namespace SavGol
