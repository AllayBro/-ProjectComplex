import os
from typing import Any, Dict, Optional

import cv2

from .api import now_ms, ensure_dir, load_image_any, DeviceManager, DeviceChoice, VehicleDetector, union_merge_boxes, PostProcessor, DistanceEstimator, draw_annotated


def run(image_path: str, out_dir: str, cfg: Dict[str, Any], device_mode: str = "auto", state: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
    ensure_dir(out_dir)
    img = load_image_any(image_path)
    h, w = img.shape[:2]

    dev_cfg = cfg.get("device", {})
    gpu_id = int(dev_cfg.get("gpu_id", 0))
    fallback = bool(dev_cfg.get("fallback_to_cpu", True))

    warnings = []
    t0 = now_ms()

    dev = DeviceManager.pick(device_mode, gpu_id, fallback)

    model_path = str(cfg.get("detector", {}).get("model_path", "yolov8n.pt"))
    detector = None
    if state is not None:
        detector = state.get("detector", None)
        if detector is not None and getattr(detector, "model_path", None) != model_path:
            detector = None
    if detector is None:
        detector = VehicleDetector(model_path)
        if state is not None:
            state["detector"] = detector

    inference_ms = 0
    try:
        dets, tms = detector.predict(img, cfg, dev)
        inference_ms += int(tms.get("inference_ms", 0))
    except Exception:
        if fallback and dev.device_used == "gpu":
            warnings.append("gpu_failed_fallback_to_cpu")
            dev = DeviceChoice("cpu", "cpu")
            dets, tms = detector.predict(img, cfg, dev)
            inference_ms += int(tms.get("inference_ms", 0))
        else:
            raise

    if cfg.get("union_nms", {}).get("enabled", True):
        dets = union_merge_boxes(dets, float(cfg.get("union_nms", {}).get("iou", 0.55)))

    dets = PostProcessor.apply(w, h, dets, cfg)

    de = DistanceEstimator()
    dets = [de.estimate(img, d, cfg) for d in dets]

    annotated = draw_annotated(img, dets)
    annotated_path = os.path.abspath(os.path.join(out_dir, "annotated_cluster_4.jpg"))
    cv2.imwrite(annotated_path, annotated)

    t1 = now_ms()
    return {
        "module_id": "cluster_4",
        "image_w": int(w),
        "image_h": int(h),
        "device_used": dev.device_used,
        "warnings": warnings,
        "annotated_image_path": annotated_path,
        "cleaned_image_path": "",
        "artifacts": {},
        "timings_ms": {
            "total": int(t1 - t0),
            "preprocess": 0,
            "inference": int(inference_ms),
            "postprocess": int(max(0, (t1 - t0) - inference_ms))
        },
        "detections": dets
    }