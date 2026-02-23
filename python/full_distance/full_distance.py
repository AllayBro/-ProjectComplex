import os
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional

import cv2
import numpy as np

from prototypes.api import (
    now_ms,
    ensure_dir,
    load_image_any,
    DeviceManager,
    DeviceChoice,
    VehicleDetector,
    union_merge_boxes,
    PostProcessor,
    DistanceEstimator,
    Cleanup,
    draw_annotated,
    write_csv_distance,
)


@dataclass
class DetectionContext:
    image_path: str
    out_dir: str
    img_bgr: np.ndarray
    image_w: int
    image_h: int
    device_used: str
    warnings: List[str] = field(default_factory=list)
    detections: List[Dict[str, Any]] = field(default_factory=list)
    artifacts: Dict[str, Any] = field(default_factory=dict)
    timings_ms: Dict[str, int] = field(default_factory=lambda: {"total": 0, "preprocess": 0, "inference": 0, "postprocess": 0})


class IProcessor:
    def run(self, ctx: DetectionContext, cfg: Dict[str, Any], dev: DeviceChoice, state: Optional[Dict[str, Any]]) -> DetectionContext:
        raise NotImplementedError()


class VehicleDetectorStep(IProcessor):
    def run(self, ctx: DetectionContext, cfg: Dict[str, Any], dev: DeviceChoice, state: Optional[Dict[str, Any]]) -> DetectionContext:
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

        dets, tms = detector.predict(ctx.img_bgr, cfg, dev)
        ctx.timings_ms["inference"] += int(tms.get("inference_ms", 0))

        if cfg.get("union_nms", {}).get("enabled", True):
            dets = union_merge_boxes(dets, float(cfg.get("union_nms", {}).get("iou", 0.55)))

        dets = PostProcessor.apply(ctx.image_w, ctx.image_h, dets, cfg)

        ctx.detections = dets
        return ctx


class DistanceEstimationStep(IProcessor):
    def run(self, ctx: DetectionContext, cfg: Dict[str, Any], dev: DeviceChoice, state: Optional[Dict[str, Any]]) -> DetectionContext:
        de = DistanceEstimator()
        ctx.detections = [de.estimate(ctx.img_bgr, d, cfg) for d in ctx.detections]

        csv_path = os.path.abspath(os.path.join(ctx.out_dir, "distance_results.csv"))
        ctx.artifacts["csv_path"] = csv_path
        return ctx


class CleanupStep(IProcessor):
    def run(self, ctx: DetectionContext, cfg: Dict[str, Any], dev: DeviceChoice, state: Optional[Dict[str, Any]]) -> DetectionContext:
        cleaned = Cleanup.cleanup_image(ctx.img_bgr, ctx.detections, cfg)
        cleaned_path = os.path.abspath(os.path.join(ctx.out_dir, "cleaned_only_vehicles.jpg"))
        cv2.imwrite(cleaned_path, cleaned)
        ctx.artifacts["cleaned_image_path"] = cleaned_path
        return ctx


class Pipeline:
    def __init__(self, steps: List[IProcessor]):
        self.steps = steps

    def run(self, ctx: DetectionContext, cfg: Dict[str, Any], dev: DeviceChoice, state: Optional[Dict[str, Any]]) -> DetectionContext:
        for s in self.steps:
            ctx = s.run(ctx, cfg, dev, state)
        return ctx


def run(image_path: str, out_dir: str, cfg: Dict[str, Any], device_mode: str = "auto", state: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
    ensure_dir(out_dir)

    img = load_image_any(image_path)
    h, w = img.shape[:2]

    dev_cfg = cfg.get("device", {})
    gpu_id = int(dev_cfg.get("gpu_id", 0))
    fallback = bool(dev_cfg.get("fallback_to_cpu", True))

    warnings: List[str] = []
    t0 = now_ms()

    dev = DeviceManager.pick(device_mode, gpu_id, fallback)

    ctx = DetectionContext(
        image_path=image_path,
        out_dir=out_dir,
        img_bgr=img,
        image_w=int(w),
        image_h=int(h),
        device_used=dev.device_used,
        warnings=warnings,
    )

    pipe = Pipeline([
        VehicleDetectorStep(),
        DistanceEstimationStep(),
        CleanupStep(),
    ])

    inference_ms_before = int(ctx.timings_ms.get("inference", 0))
    try:
        ctx = pipe.run(ctx, cfg, dev, state)
    except Exception:
        if fallback and dev.device_used == "gpu":
            warnings.append("gpu_failed_fallback_to_cpu")
            dev = DeviceChoice("cpu", "cpu")
            ctx.device_used = "cpu"
            ctx.timings_ms["inference"] = inference_ms_before
            ctx = pipe.run(ctx, cfg, dev, state)
        else:
            raise

    annotated = draw_annotated(img, ctx.detections)
    annotated_path = os.path.abspath(os.path.join(out_dir, "annotated_full.jpg"))
    cv2.imwrite(annotated_path, annotated)

    cleaned_path = str(ctx.artifacts.get("cleaned_image_path", ""))
    csv_path = str(ctx.artifacts.get("csv_path", ""))

    t1 = now_ms()
    total = int(t1 - t0)
    inf = int(ctx.timings_ms.get("inference", 0))
    ctx.timings_ms["total"] = total
    ctx.timings_ms["postprocess"] = int(max(0, total - inf))

    if csv_path:
        write_csv_distance(csv_path, image_path, ctx.detections, dev.device_used, ctx.timings_ms)

    return {
        "module_id": "distance_full",
        "image_w": int(w),
        "image_h": int(h),
        "device_used": dev.device_used,
        "warnings": warnings,
        "annotated_image_path": annotated_path,
        "cleaned_image_path": cleaned_path,
        "artifacts": {"csv_path": csv_path} if csv_path else {},
        "timings_ms": dict(ctx.timings_ms),
        "detections": ctx.detections
    }