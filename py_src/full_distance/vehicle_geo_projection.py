from __future__ import annotations

import math
from typing import Any, Dict, Optional


def _safe_float(value: Any) -> Optional[float]:
    if value is None:
        return None
    try:
        return float(value)
    except Exception:
        return None


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


def _vehicle_heading_offset_deg(primary_vehicle: Dict[str, Any], artifacts: Dict[str, Any], image_w: int) -> float:
    if not primary_vehicle or image_w <= 0:
        return 0.0

    bottom_center = primary_vehicle.get("bottom_center_px")
    if not isinstance(bottom_center, (list, tuple)) or len(bottom_center) < 1:
        return 0.0

    x = _safe_float(bottom_center[0])
    if x is None:
        return 0.0

    focal_px = _safe_float(artifacts.get("focal_px_x"))
    if focal_px is None or focal_px <= 1.0:
        focal_px = _safe_float(artifacts.get("focal_px_y"))
    if focal_px is None or focal_px <= 1.0:
        return 0.0

    dx = float(x) - float(image_w) * 0.5
    offset = math.degrees(math.atan2(dx, float(focal_px)))
    return max(-30.0, min(30.0, float(offset)))


def project_primary_vehicle(
        camera_refine: Dict[str, Any],
        artifacts: Dict[str, Any],
        image_w: int,
        image_h: int,
        cfg: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    cfg = cfg or {}

    refined_lat = _safe_float(camera_refine.get("camera_refined_lat"))
    refined_lon = _safe_float(camera_refine.get("camera_refined_lon"))
    refined_az = _safe_float(camera_refine.get("camera_refined_azimuth_deg"))
    camera_conf = _safe_float(camera_refine.get("camera_confidence"))
    if camera_conf is None:
        camera_conf = 0.0

    primary_vehicle = camera_refine.get("primary_vehicle", {}) or {}
    vehicle_dist = _safe_float(primary_vehicle.get("dist_m"))

    if refined_lat is None or refined_lon is None:
        return {
            "status": "camera_missing",
            "vehicle_lat": None,
            "vehicle_lon": None,
            "vehicle_distance_m": vehicle_dist,
            "vehicle_bearing_deg": None,
            "vehicle_confidence": 0.0,
            "source_detection_index": None,
            "needs_manual_review": True,
            "debug": {
                "reason": "camera_refined_missing",
            },
        }

    if refined_az is None:
        return {
            "status": "camera_heading_missing",
            "vehicle_lat": None,
            "vehicle_lon": None,
            "vehicle_distance_m": vehicle_dist,
            "vehicle_bearing_deg": None,
            "vehicle_confidence": 0.0,
            "source_detection_index": None,
            "needs_manual_review": True,
            "debug": {
                "reason": "camera_refined_azimuth_missing",
            },
        }

    if not primary_vehicle:
        return {
            "status": "vehicle_missing",
            "vehicle_lat": None,
            "vehicle_lon": None,
            "vehicle_distance_m": None,
            "vehicle_bearing_deg": None,
            "vehicle_confidence": 0.0,
            "source_detection_index": None,
            "needs_manual_review": True,
            "debug": {
                "reason": "primary_vehicle_missing",
            },
        }

    if vehicle_dist is None or vehicle_dist <= 0.0:
        return {
            "status": "vehicle_distance_missing",
            "vehicle_lat": None,
            "vehicle_lon": None,
            "vehicle_distance_m": vehicle_dist,
            "vehicle_bearing_deg": None,
            "vehicle_confidence": 0.0,
            "source_detection_index": None,
            "needs_manual_review": True,
            "debug": {
                "reason": "dist_m_missing",
                "primary_vehicle": primary_vehicle,
            },
        }

    heading_offset_deg = _vehicle_heading_offset_deg(primary_vehicle, artifacts, int(image_w))
    vehicle_bearing_deg = _norm_angle(float(refined_az) + float(heading_offset_deg))

    distance_scale = float(cfg.get("distance_scale", 1.0))
    max_distance_m = float(cfg.get("max_distance_m", 300.0))
    min_distance_m = float(cfg.get("min_distance_m", 1.0))

    projected_distance_m = float(vehicle_dist) * distance_scale
    projected_distance_m = max(min_distance_m, min(max_distance_m, projected_distance_m))

    vehicle_lat, vehicle_lon = _destination_point(
        lat_deg=float(refined_lat),
        lon_deg=float(refined_lon),
        bearing_deg=float(vehicle_bearing_deg),
        distance_m=float(projected_distance_m),
    )

    confidence = 0.0
    confidence += 0.55 * max(0.0, min(1.0, float(camera_conf)))
    if primary_vehicle.get("conf") is not None:
        det_conf = _safe_float(primary_vehicle.get("conf"))
        if det_conf is not None:
            confidence += 0.25 * max(0.0, min(1.0, float(det_conf)))
    confidence += 0.20
    confidence = max(0.0, min(1.0, confidence))

    manual_threshold = float(cfg.get("min_vehicle_confidence", 0.55))

    return {
        "status": "ready",
        "vehicle_lat": round(float(vehicle_lat), 8),
        "vehicle_lon": round(float(vehicle_lon), 8),
        "vehicle_distance_m": round(float(projected_distance_m), 3),
        "vehicle_bearing_deg": round(float(vehicle_bearing_deg), 3),
        "vehicle_confidence": round(float(confidence), 6),
        "source_detection_index": 0,
        "needs_manual_review": bool(confidence < manual_threshold),
        "debug": {
            "camera_lat": round(float(refined_lat), 8),
            "camera_lon": round(float(refined_lon), 8),
            "camera_azimuth_deg": round(float(refined_az), 3),
            "heading_offset_deg": round(float(heading_offset_deg), 3),
            "primary_vehicle": primary_vehicle,
            "image_w": int(image_w),
            "image_h": int(image_h),
        },
    }