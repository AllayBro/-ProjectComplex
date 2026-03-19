import json
import os
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional

import cv2
import numpy as np
from .camera_refine import refine_camera_pose, _read_exif_seed
from .coarse_geo_search import run as run_coarse_geo_search
from .vehicle_geo_projection import project_all_vehicles
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
        model_path = str(
            cfg.get("yolo_model_path")
            or (cfg.get("detector", {}) or {}).get("model_path")
            or ""
        ).strip().strip('"').strip("'")

        if not model_path:
            raise RuntimeError("yolo_model_path не задан. Модель должна выбираться из папки yolo в UI.")

        yolo_dir = str(cfg.get("yolo_dir", "") or "").strip().strip('"').strip("'")
        if yolo_dir and (not os.path.isabs(model_path)):
            model_path = os.path.join(yolo_dir, model_path)

        model_path = os.path.abspath(os.path.expandvars(os.path.expanduser(model_path)))
        if not os.path.exists(model_path):
            raise RuntimeError(f"Файл модели YOLO не найден: {model_path}")

        if yolo_dir:
            yolo_dir_abs = os.path.abspath(os.path.expandvars(os.path.expanduser(yolo_dir)))
            try:
                if os.path.commonpath([yolo_dir_abs, model_path]) != yolo_dir_abs:
                    raise RuntimeError(f"Модель должна быть внутри папки yolo: {yolo_dir_abs}. Получено: {model_path}")
            except Exception:
                pass
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
def _inject_seed_hints(image_path: str, artifacts: Dict[str, Any], cfg: Dict[str, Any]) -> None:
    exif_seed = _read_exif_seed(image_path) or {}
    exif_hint: Dict[str, Any] = {}

    lat = exif_seed.get("lat")
    lon = exif_seed.get("lon")
    heading = exif_seed.get("heading_deg")
    hpe = exif_seed.get("gps_hposition_error_m")
    dop = exif_seed.get("gps_dop")

    if isinstance(lat, (int, float)) and isinstance(lon, (int, float)):
        exif_hint["lat"] = float(lat)
        exif_hint["lon"] = float(lon)

    if isinstance(heading, (int, float)):
        exif_hint["heading_deg"] = float(heading)

    if isinstance(hpe, (int, float)) and hpe > 0.0:
        exif_hint["search_radius_m"] = float(max(100.0, min(5000.0, float(hpe) * 6.0)))
    elif isinstance(dop, (int, float)) and dop > 0.0:
        exif_hint["search_radius_m"] = float(max(150.0, min(5000.0, float(dop) * 80.0)))
    else:
        exif_hint["search_radius_m"] = float(
            (cfg.get("coarse_geo_search", {}) or {}).get("coarse_default_search_radius_m", 3000.0)
        )

    if exif_hint:
        artifacts["exif_gps"] = exif_hint

    gps_hint = cfg.get("gps_hint", {})
    if isinstance(gps_hint, dict) and gps_hint:
        artifacts["gps_hint"] = dict(gps_hint)

    manual_area = cfg.get("manual_search_area", {})
    if isinstance(manual_area, dict) and manual_area:
        artifacts["manual_search_area"] = dict(manual_area)

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

    _inject_seed_hints(image_path, ctx.artifacts, cfg)

    raw_coarse_geo_cfg = cfg.get("coarse_geo_search", {}) if isinstance(cfg, dict) else {}
    if not isinstance(raw_coarse_geo_cfg, dict):
        raw_coarse_geo_cfg = {}

    coarse_geo_cfg = dict(raw_coarse_geo_cfg)

    offline_map_cfg = cfg.get("offline_map", {}) if isinstance(cfg, dict) else {}
    if isinstance(offline_map_cfg, dict) and "offline_map" not in coarse_geo_cfg:
        coarse_geo_cfg["offline_map"] = dict(offline_map_cfg)

    scene_features_cfg = {}
    camera_refine_cfg_tmp = cfg.get("camera_refine", {}) if isinstance(cfg, dict) else {}
    if isinstance(camera_refine_cfg_tmp, dict):
        sf = camera_refine_cfg_tmp.get("scene_features", {})
        if isinstance(sf, dict):
            scene_features_cfg = dict(sf)

    if not scene_features_cfg:
        sf = cfg.get("scene_features", {}) if isinstance(cfg, dict) else {}
        if isinstance(sf, dict):
            scene_features_cfg = dict(sf)

    if scene_features_cfg and "scene_features" not in coarse_geo_cfg:
        coarse_geo_cfg["scene_features"] = scene_features_cfg

    coarse_geo = run_coarse_geo_search(
        image_path=image_path,
        detections=ctx.detections,
        image_w=int(w),
        image_h=int(h),
        artifacts=ctx.artifacts,
        cfg=coarse_geo_cfg,
    )

    coarse_geo_json_path = os.path.abspath(os.path.join(out_dir, "coarse_geo_search.json"))
    with open(coarse_geo_json_path, "w", encoding="utf-8") as f:
        json.dump(coarse_geo, f, ensure_ascii=False, indent=2)

    ctx.artifacts["coarse_geo_search"] = coarse_geo
    ctx.artifacts["coarse_geo_json_path"] = coarse_geo_json_path

    camera_refine_cfg = cfg.get("camera_refine", {}) if isinstance(cfg, dict) else {}
    if not isinstance(camera_refine_cfg, dict):
        camera_refine_cfg = {}

    camera_refine = refine_camera_pose(
        image_path=image_path,
        detections=ctx.detections,
        image_w=int(w),
        image_h=int(h),
        cfg=camera_refine_cfg,
        artifacts=ctx.artifacts,
    )

    camera_refine_json_path = os.path.abspath(os.path.join(out_dir, "camera_refine.json"))
    camera_refine["refine_debug_path"] = camera_refine_json_path
    with open(camera_refine_json_path, "w", encoding="utf-8") as f:
        json.dump(camera_refine, f, ensure_ascii=False, indent=2)

    ctx.artifacts["camera_refine"] = camera_refine
    ctx.artifacts["camera_refine_json_path"] = camera_refine_json_path
    ctx.artifacts["detections"] = list(ctx.detections)
    vehicle_geo_cfg = cfg.get("vehicle_geo", {}) if isinstance(cfg, dict) else {}
    if not isinstance(vehicle_geo_cfg, dict):
        vehicle_geo_cfg = {}

    vehicle_geo = project_all_vehicles(
        camera_refine=camera_refine,
        artifacts=ctx.artifacts,
        image_w=int(w),
        image_h=int(h),
        cfg=vehicle_geo_cfg,
    )

    vehicle_geo_json_path = os.path.abspath(os.path.join(out_dir, "vehicle_geo.json"))
    with open(vehicle_geo_json_path, "w", encoding="utf-8") as f:
        json.dump(vehicle_geo, f, ensure_ascii=False, indent=2)

    ctx.artifacts["vehicle_geo"] = vehicle_geo
    ctx.artifacts["vehicle_geo_json_path"] = vehicle_geo_json_path

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
        "artifacts": dict(ctx.artifacts),
        "timings_ms": dict(ctx.timings_ms),
        "detections": ctx.detections
    }