"""CLI test for AE_YOLO inference pipeline.
Tests: ONNX model loading, letterbox preprocessing, inference, postprocessing.
Usage: python test_inference.py <model.onnx> <image.jpg|png>
"""
import sys
import numpy as np

def letterbox_preprocess(img, target_size=640):
    """Letterbox resize + HWC->CHW, matching Letterbox.h logic."""
    h, w = img.shape[:2]
    scale = min(target_size / w, target_size / h)
    new_w = round(w * scale)
    new_h = round(h * scale)
    pad_x = (target_size - new_w) / 2.0
    pad_y = (target_size - new_h) / 2.0

    # Resize with OpenCV
    import cv2
    resized = cv2.resize(img, (new_w, new_h), interpolation=cv2.INTER_LINEAR)

    # Pad to target_size
    pad_left = int(pad_x)
    pad_top = int(pad_y)
    pad_right = target_size - new_w - pad_left
    pad_bottom = target_size - new_h - pad_top
    padded = cv2.copyMakeBorder(resized, pad_top, pad_bottom, pad_left, pad_right,
                                 cv2.BORDER_CONSTANT, value=(114, 114, 114))

    # Normalize to [0,1] and HWC->CHW
    chw = padded.astype(np.float32) / 255.0
    chw = chw.transpose(2, 0, 1)  # HWC -> CHW (BGR order from OpenCV)
    # Convert BGR to RGB
    chw = chw[::-1].copy()

    info = {
        'scale': scale, 'pad_x': pad_x, 'pad_y': pad_y,
        'orig_w': w, 'orig_h': h, 'input_size': target_size
    }
    return chw, info


def remap_coords(x, y, info):
    """Remap from model space to original image space."""
    orig_x = (x - info['pad_x']) / info['scale']
    orig_y = (y - info['pad_y']) / info['scale']
    orig_x = max(0, min(orig_x, info['orig_w'] - 1))
    orig_y = max(0, min(orig_y, info['orig_h'] - 1))
    return orig_x, orig_y


def postprocess_yolo26(output, info, conf_threshold=0.25):
    """Post-NMS format: [1, N, 57] — x1,y1,x2,y2,conf,class_id,kp*51"""
    data = output[0]  # [N, 57]
    n_dets, n_cols = data.shape
    print(f"  Post-NMS format: {n_dets} detections, {n_cols} columns")

    # Filter by confidence
    mask = data[:, 4] >= conf_threshold
    filtered = data[mask]
    print(f"  {len(filtered)} detections above threshold {conf_threshold}")

    if len(filtered) == 0:
        return None

    # Pick highest confidence
    best_idx = np.argmax(filtered[:, 4])
    det = filtered[best_idx]
    print(f"  Best detection: conf={det[4]:.4f} bbox=[{det[0]:.1f},{det[1]:.1f},{det[2]:.1f},{det[3]:.1f}]")

    keypoints = []
    for k in range(17):
        kp_x = det[6 + k*3]
        kp_y = det[6 + k*3 + 1]
        kp_conf = det[6 + k*3 + 2]
        ox, oy = remap_coords(kp_x, kp_y, info)
        keypoints.append((ox, oy, kp_conf))

    return keypoints


def postprocess_v8(output, info, conf_threshold=0.25):
    """Raw anchor format: [1, 56, 8400] — features x anchors"""
    data = output[0]  # [56, 8400]
    n_feat, n_anchors = data.shape
    print(f"  Raw anchor format: {n_feat} features, {n_anchors} anchors")

    confs = data[4, :]
    mask = confs >= conf_threshold
    n_valid = mask.sum()
    print(f"  {n_valid} detections above threshold {conf_threshold}")

    if n_valid == 0:
        return None

    # Simple: just pick highest confidence (skip full NMS for CLI test)
    best_a = np.argmax(confs)
    print(f"  Best detection: conf={confs[best_a]:.4f}")

    keypoints = []
    for k in range(17):
        base = 5 + k * 3
        kp_x = data[base, best_a]
        kp_y = data[base + 1, best_a]
        kp_conf = data[base + 2, best_a]
        ox, oy = remap_coords(kp_x, kp_y, info)
        keypoints.append((ox, oy, kp_conf))

    return keypoints


KP_NAMES = [
    "Nose",   "LEye",   "REye",   "LEar",   "REar",
    "LShldr", "RShldr", "LElbow", "RElbow", "LWrist",
    "RWrist", "LHip",   "RHip",   "LKnee",  "RKnee",
    "LAnkle", "RAnkle"
]


def main():
    if len(sys.argv) < 3:
        print(f"Usage: python {sys.argv[0]} <model.onnx> <image.jpg|png>")
        sys.exit(1)

    model_path = sys.argv[1]
    image_path = sys.argv[2]
    conf_threshold = float(sys.argv[3]) if len(sys.argv) > 3 else 0.25

    import cv2
    import onnxruntime as ort

    # Load image
    img = cv2.imread(image_path)
    if img is None:
        print(f"Error: could not load image '{image_path}'")
        sys.exit(1)
    print(f"Image: {image_path} ({img.shape[1]}x{img.shape[0]})")

    # Load model
    print(f"Model: {model_path}")
    sess = ort.InferenceSession(model_path)
    inp = sess.get_inputs()[0]
    out = sess.get_outputs()[0]
    input_size = inp.shape[2]  # [1, 3, H, W]
    print(f"  Input: {inp.name} shape={inp.shape}")
    print(f"  Output: {out.name} shape={out.shape}")

    # Preprocess
    chw, info = letterbox_preprocess(img, input_size)
    input_tensor = chw[np.newaxis]  # [1, 3, H, W]
    print(f"  Preprocessed: {input_tensor.shape}, scale={info['scale']:.4f}, pad=({info['pad_x']:.1f},{info['pad_y']:.1f})")

    # Run inference
    results = sess.run(None, {inp.name: input_tensor})
    output = results[0]
    print(f"  Output shape: {output.shape}")

    # Auto-detect format and postprocess
    if output.ndim == 3:
        dim1, dim2 = output.shape[1], output.shape[2]
    else:
        dim1, dim2 = output.shape[0], output.shape[1]

    if dim2 == 57 or (dim2 >= 56 and dim2 <= 60 and dim1 <= 1000):
        keypoints = postprocess_yolo26(output, info, conf_threshold)
    elif dim1 == 56 or (dim1 >= 50 and dim1 <= 60 and dim2 >= 1000):
        keypoints = postprocess_v8(output, info, conf_threshold)
    else:
        print(f"Error: unrecognized output shape {output.shape}")
        sys.exit(1)

    if keypoints is None:
        print("\nNo person detected!")
        sys.exit(0)

    # Print results
    print(f"\n{'Keypoint':<10} {'X':>8} {'Y':>8} {'Conf':>6}")
    print("-" * 35)
    for i, (x, y, c) in enumerate(keypoints):
        print(f"{KP_NAMES[i]:<10} {x:8.1f} {y:8.1f} {c:6.3f}")

    # Draw on image
    for i, (x, y, c) in enumerate(keypoints):
        if c > 0.3:
            cv2.circle(img, (int(x), int(y)), 4, (0, 255, 0), -1)
            cv2.putText(img, KP_NAMES[i], (int(x)+5, int(y)-5),
                       cv2.FONT_HERSHEY_SIMPLEX, 0.4, (0, 255, 0), 1)

    out_path = image_path.rsplit('.', 1)[0] + '_yolo_pose.jpg'
    cv2.imwrite(out_path, img)
    print(f"\nVisualization saved to: {out_path}")


if __name__ == "__main__":
    main()
