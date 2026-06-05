from __future__ import annotations

import math
from typing import Any, Dict, List, Optional, Tuple


def _safe_float(value: Any) -> Optional[float]:
    if value is None:
        return None
    try:
        return float(value)
    except Exception:
        return None


def _normalized_vehicle_class(cls_name: Any) -> str:
    cls = str(cls_name or "").strip().lower()
    if cls == "van":
        return "car"
    return cls


def _vehicle_dims_m(cls_id: Optional[int], cls_name: Any) -> Tuple[float, float]:
    cls = _normalized_vehicle_class(cls_name)
    if cls not in {"car", "truck", "bus", "motorcycle"}:
        mapping = {2: "car", 3: "truck", 5: "bus", 7: "motorcycle"}
        cls = mapping.get(int(cls_id)) if cls_id is not None else "car"
    dims = {
        "car": (4.5, 1.8),
        "truck": (9.0, 2.5),
        "bus": (12.0, 2.5),
        "motorcycle": (2.1, 0.8),
    }
    return dims.get(cls, dims["car"])


def _haversine_m(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    r = 6371000.0
    p1 = math.radians(lat1)
    p2 = math.radians(lat2)
    dp = math.radians(lat2 - lat1)
    dl = math.radians(lon2 - lon1)
    a = math.sin(dp / 2.0) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2.0) ** 2
    return 2.0 * r * math.atan2(math.sqrt(a), math.sqrt(max(0.0, 1.0 - a)))


def _projection_anchor(
        camera_refine: Dict[str, Any],
        cfg: Dict[str, Any],
) -> Tuple[float, float, str]:
    refined_lat = _safe_float(camera_refine.get("camera_refined_lat"))
    refined_lon = _safe_float(camera_refine.get("camera_refined_lon"))
    init_lat = _safe_float(camera_refine.get("camera_init_lat"))
    init_lon = _safe_float(camera_refine.get("camera_init_lon"))

    if refined_lat is None or refined_lon is None:
        return 0.0, 0.0, "missing"

    if init_lat is None or init_lon is None:
        return float(refined_lat), float(refined_lon), "refined"

    max_shift_m = float(cfg.get("max_camera_shift_for_vehicle_m", 12.0))
    shift_m = _haversine_m(float(init_lat), float(init_lon), float(refined_lat), float(refined_lon))

    if bool(cfg.get("prefer_init_anchor_for_vehicle", True)) and shift_m > max_shift_m:
        return float(init_lat), float(init_lon), "init"

    return float(refined_lat), float(refined_lon), "refined"


def _norm_angle(deg: float) -> float:
    while deg < 0.0:
        deg += 360.0
    while deg >= 360.0:
        deg -= 360.0
    return deg


def _destination_point(lat_deg: float, lon_deg: float, bearing_deg: float, distance_m: float) -> tuple[float, float]:
    r = 6378137.0

    lat1 = math.radians(lat_deg)
    lon1 = math.radians(lon_deg)
    brng = math.radians(bearing_deg)
    ang = float(distance_m) / r

    lat2 = math.asin(
        math.sin(lat1) * math.cos(ang) +
        math.cos(lat1) * math.sin(ang) * math.cos(brng)
    )

    lon2 = lon1 + math.atan2(
        math.sin(brng) * math.sin(ang) * math.cos(lat1),
        math.cos(ang) - math.sin(lat1) * math.sin(lat2)
    )

    return math.degrees(lat2), math.degrees(lon2)

def _bottom_center_px(vehicle: Dict[str, Any]) -> Optional[list[float]]:
    bottom_center = vehicle.get("bottom_center_px")
    if isinstance(bottom_center, (list, tuple)) and len(bottom_center) >= 2:
        x = _safe_float(bottom_center[0])
        y = _safe_float(bottom_center[1])
        if x is not None and y is not None:
            return [float(x), float(y)]

    bbox = vehicle.get("bbox_xyxy")
    if isinstance(bbox, (list, tuple)) and len(bbox) == 4:
        try:
            x1, y1, x2, y2 = map(float, bbox)
            return [0.5 * (x1 + x2), y2]
        except Exception:
            return None

    return None


def _resolve_focal_px(vehicle: Dict[str, Any], artifacts: Dict[str, Any], image_w: int) -> Optional[float]:
    focal_px = _safe_float(artifacts.get("focal_px_x"))
    if focal_px is None or focal_px <= 1.0:
        focal_px = _safe_float(artifacts.get("focal_px_y"))

    meta = vehicle.get("meta") if isinstance(vehicle, dict) else None
    if (focal_px is None or focal_px <= 1.0) and isinstance(meta, dict):
        focal_px = _safe_float(meta.get("focal_px"))
    if (focal_px is None or focal_px <= 1.0) and isinstance(meta, dict):
        focal_px = _safe_float(meta.get("focal_px_x"))
    if (focal_px is None or focal_px <= 1.0) and isinstance(meta, dict):
        focal_px = _safe_float(meta.get("focal_px_y"))

    if (focal_px is None or focal_px <= 1.0) and image_w > 0:
        focal_mm = 4.25
        sensor_width_mm = 5.6
        focal_px = (focal_mm / sensor_width_mm) * float(image_w)

    if focal_px is None or focal_px <= 1.0:
        return None
    return float(focal_px)


def _vehicle_heading_offset_deg(vehicle: Dict[str, Any], artifacts: Dict[str, Any], image_w: int) -> float:
    if not vehicle or image_w <= 0:
        return 0.0

    bottom_center = _bottom_center_px(vehicle)
    if not bottom_center:
        return 0.0

    x = _safe_float(bottom_center[0])
    if x is None:
        return 0.0

    focal_px = _resolve_focal_px(vehicle, artifacts, image_w)
    if focal_px is None:
        return 0.0

    dx = float(x) - float(image_w) * 0.5
    offset = math.degrees(math.atan2(dx, float(focal_px)))
    return max(-30.0, min(30.0, float(offset)))


def _collect_vehicles(camera_refine: Dict[str, Any], artifacts: Dict[str, Any]) -> List[Dict[str, Any]]:
    out: List[Dict[str, Any]] = []

    detections = artifacts.get("detections")
    if isinstance(detections, list):
        for idx, det in enumerate(detections):
            if not isinstance(det, dict):
                continue

            meta = det.get("meta") or {}
            dist_m = _safe_float(meta.get("dist_m"))
            if dist_m is None:
                dist_m = _safe_float(meta.get("distance_m"))
            if dist_m is None:
                dist_m = _safe_float(det.get("dist_m"))
            if dist_m is None:
                dist_m = _safe_float(det.get("distance_m"))
            if dist_m is None or dist_m <= 0.0:
                continue

            item = dict(det)
            item["dist_m"] = float(dist_m)
            item["_source_detection_index"] = int(idx)

            bc = _bottom_center_px(item)
            if bc is not None:
                item["bottom_center_px"] = bc

            out.append(item)

    if out:
        return out

    primary_vehicle = camera_refine.get("primary_vehicle", {}) or {}
    if isinstance(primary_vehicle, dict) and primary_vehicle:
        dist_m = _safe_float(primary_vehicle.get("dist_m"))
        if dist_m is None:
            dist_m = _safe_float(primary_vehicle.get("distance_m"))
        if dist_m is not None and dist_m > 0.0:
            item = dict(primary_vehicle)
            item["dist_m"] = float(dist_m)
            item["_source_detection_index"] = int(primary_vehicle.get("source_detection_index", 0) or 0)

            bc = _bottom_center_px(item)
            if bc is not None:
                item["bottom_center_px"] = bc

            out.append(item)

    return out


def _vehicle_confidence(camera_conf: float, det_conf: Optional[float]) -> float:
    confidence = 0.0
    confidence += 0.55 * max(0.0, min(1.0, float(camera_conf)))
    if det_conf is not None:
        confidence += 0.25 * max(0.0, min(1.0, float(det_conf)))
    confidence += 0.20
    return max(0.0, min(1.0, confidence))


def project_all_vehicles(
        camera_refine: Dict[str, Any],
        artifacts: Dict[str, Any],
        image_w: int,
        image_h: int,
        cfg: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    cfg = cfg or {}

    anchor_lat, anchor_lon, anchor_source = _projection_anchor(camera_refine, cfg)
    refined_az = _safe_float(camera_refine.get("camera_refined_azimuth_deg"))
    camera_conf = _safe_float(camera_refine.get("camera_confidence"))
    if camera_conf is None:
        camera_conf = 0.0

    vehicles = _collect_vehicles(camera_refine, artifacts)

    if anchor_source == "missing":
        return {
            "status": "camera_missing",
            "vehicle_lat": None,
            "vehicle_lon": None,
            "vehicle_distance_m": None,
            "vehicle_bearing_deg": None,
            "vehicle_confidence": 0.0,
            "source_detection_index": None,
            "needs_manual_review": True,
            "vehicles": [],
            "vehicles_count": 0,
            "debug": {
                "reason": "camera_refined_missing",
            },
        }

    heading_fallback = "refined"

    if refined_az is None:
        candidate_search = camera_refine.get("candidate_search", {}) or {}
        if isinstance(candidate_search, dict):
            best_candidate = candidate_search.get("best_candidate", {}) or {}
            if isinstance(best_candidate, dict):
                refined_az = _safe_float(best_candidate.get("heading_deg"))
                if refined_az is not None:
                    heading_fallback = "candidate_best"

    if refined_az is None:
        refined_az = 0.0
        heading_fallback = "zero_fallback"
    if not vehicles:
        return {
            "status": "vehicle_missing",
            "vehicle_lat": None,
            "vehicle_lon": None,
            "vehicle_distance_m": None,
            "vehicle_bearing_deg": None,
            "vehicle_confidence": 0.0,
            "source_detection_index": None,
            "needs_manual_review": True,
            "vehicles": [],
            "vehicles_count": 0,
            "debug": {
                "reason": "vehicles_missing",
            },
        }

    distance_scale = float(cfg.get("distance_scale", 1.0))
    max_distance_m = float(cfg.get("max_distance_m", 300.0))
    min_distance_m = float(cfg.get("min_distance_m", 1.0))
    manual_threshold = float(cfg.get("min_vehicle_confidence", 0.55))

    projected: List[Dict[str, Any]] = []

    for vehicle in vehicles:
        vehicle_dist = _safe_float(vehicle.get("dist_m"))
        if vehicle_dist is None or vehicle_dist <= 0.0:
            continue

        heading_offset_deg = _vehicle_heading_offset_deg(vehicle, artifacts, int(image_w))
        vehicle_bearing_deg = _norm_angle(float(refined_az) + float(heading_offset_deg))

        projected_distance_m = float(vehicle_dist) * distance_scale
        projected_distance_m = max(min_distance_m, min(max_distance_m, projected_distance_m))

        vehicle_lat, vehicle_lon = _destination_point(
            lat_deg=float(anchor_lat),
            lon_deg=float(anchor_lon),
            bearing_deg=float(vehicle_bearing_deg),
            distance_m=float(projected_distance_m),
        )

        det_conf = _safe_float(vehicle.get("conf"))
        confidence = _vehicle_confidence(float(camera_conf), det_conf)

        cls_id_raw = vehicle.get("cls_id")
        cls_id = int(cls_id_raw) if cls_id_raw is not None else None
        cls_name = _normalized_vehicle_class(vehicle.get("cls_name"))
        length_m, width_m = _vehicle_dims_m(cls_id, cls_name)

        projected.append({
            "vehicle_lat": round(float(vehicle_lat), 8),
            "vehicle_lon": round(float(vehicle_lon), 8),
            "vehicle_distance_m": round(float(projected_distance_m), 3),
            "vehicle_bearing_deg": round(float(vehicle_bearing_deg), 3),
            "vehicle_confidence": round(float(confidence), 6),
            "source_detection_index": int(vehicle.get("_source_detection_index", 0)),
            "needs_manual_review": bool(confidence < manual_threshold),
            "cls_id": cls_id,
            "cls_name": cls_name,
            "length_m": round(float(length_m), 3),
            "width_m": round(float(width_m), 3),
        })

    if not projected:
        return {
            "status": "vehicle_distance_missing",
            "vehicle_lat": None,
            "vehicle_lon": None,
            "vehicle_distance_m": None,
            "vehicle_bearing_deg": None,
            "vehicle_confidence": 0.0,
            "source_detection_index": None,
            "needs_manual_review": True,
            "vehicles": [],
            "vehicles_count": 0,
            "debug": {
                "reason": "all_vehicle_distances_missing",
            },
        }

    projected.sort(key=lambda item: float(item.get("vehicle_distance_m", 1e18)))
    primary = projected[0]

    return {
        "status": "ready",
        "vehicle_lat": primary["vehicle_lat"],
        "vehicle_lon": primary["vehicle_lon"],
        "vehicle_distance_m": primary["vehicle_distance_m"],
        "vehicle_bearing_deg": primary["vehicle_bearing_deg"],
        "vehicle_confidence": primary["vehicle_confidence"],
        "source_detection_index": primary["source_detection_index"],
        "needs_manual_review": bool(any(v.get("needs_manual_review", False) for v in projected)),
        "vehicles": projected,
        "vehicles_count": int(len(projected)),
        "debug": {
            "camera_lat": round(float(anchor_lat), 8),
            "camera_lon": round(float(anchor_lon), 8),
            "projection_anchor": anchor_source,
            "camera_refined_lat": round(float(_safe_float(camera_refine.get("camera_refined_lat")) or anchor_lat), 8),
            "camera_refined_lon": round(float(_safe_float(camera_refine.get("camera_refined_lon")) or anchor_lon), 8),
            "camera_azimuth_deg": round(float(refined_az), 3),
            "heading_fallback": heading_fallback,
            "image_w": int(image_w),
            "image_h": int(image_h),
        },
    }

def project_primary_vehicle(
        camera_refine: Dict[str, Any],
        artifacts: Dict[str, Any],
        image_w: int,
        image_h: int,
        cfg: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    return project_all_vehicles(
        camera_refine=camera_refine,
        artifacts=artifacts,
        image_w=image_w,
        image_h=image_h,
        cfg=cfg,
    )