from __future__ import annotations

import math
from typing import Any, Dict, List, Optional, Tuple

from PIL import ExifTags, Image

from .offline_map_data import load_offline_context
from .scene_features import extract_scene_features


def _safe_float(value: Any) -> Optional[float]:
    if value is None:
        return None
    try:
        if isinstance(value, tuple) and len(value) == 2:
            a = float(value[0])
            b = float(value[1])
            if b == 0.0:
                return None
            return a / b
        return float(value)
    except Exception:
        return None


def _norm_angle(deg: float) -> float:
    while deg < 0.0:
        deg += 360.0
    while deg >= 360.0:
        deg -= 360.0
    return deg


def _angle_delta(a: float, b: float) -> float:
    d = abs(_norm_angle(a) - _norm_angle(b))
    return min(d, 360.0 - d)


def _dms_to_deg(value: Any) -> Optional[float]:
    if not isinstance(value, (list, tuple)) or len(value) != 3:
        return None
    d = _safe_float(value[0])
    m = _safe_float(value[1])
    s = _safe_float(value[2])
    if d is None or m is None or s is None:
        return None
    return float(d) + float(m) / 60.0 + float(s) / 3600.0


def _read_exif_seed(image_path: str) -> Dict[str, Any]:
    try:
        img = Image.open(image_path)
        exif = img.getexif()
    except Exception:
        return {}

    gps_tag_map = getattr(ExifTags, "GPSTAGS", {}) or {}
    tag_map = getattr(ExifTags, "TAGS", {}) or {}

    gps_ifd = None
    try:
        gps_ifd = exif.get_ifd(34853)
    except Exception:
        try:
            gps_ifd = exif.get(34853, None)
        except Exception:
            gps_ifd = None

    gps_named: Dict[str, Any] = {}
    if isinstance(gps_ifd, dict):
        for k, v in gps_ifd.items():
            gps_named[str(gps_tag_map.get(int(k), str(k)))] = v

    lat = _dms_to_deg(gps_named.get("GPSLatitude"))
    lon = _dms_to_deg(gps_named.get("GPSLongitude"))

    lat_ref = gps_named.get("GPSLatitudeRef", "")
    lon_ref = gps_named.get("GPSLongitudeRef", "")
    if isinstance(lat_ref, bytes):
        lat_ref = lat_ref.decode("utf-8", errors="replace")
    if isinstance(lon_ref, bytes):
        lon_ref = lon_ref.decode("utf-8", errors="replace")

    if lat is not None and str(lat_ref).upper().startswith("S"):
        lat = -abs(lat)
    if lon is not None and str(lon_ref).upper().startswith("W"):
        lon = -abs(lon)

    heading = _safe_float(gps_named.get("GPSImgDirection"))
    if heading is None:
        heading = _safe_float(gps_named.get("GPSDestBearing"))

    dop = _safe_float(gps_named.get("GPSDOP"))
    hpe = _safe_float(gps_named.get("GPSHPositioningError"))

    datetime_original = None
    for tid, name in tag_map.items():
        if name == "DateTimeOriginal":
            raw = exif.get(tid, None)
            if raw is not None:
                datetime_original = str(raw)
            break

    out: Dict[str, Any] = {
        "raw_gps": bool(gps_named),
        "datetime_original": datetime_original,
    }

    if lat is not None and lon is not None and -90.0 <= lat <= 90.0 and -180.0 <= lon <= 180.0:
        out["lat"] = float(lat)
        out["lon"] = float(lon)

    if heading is not None and math.isfinite(heading):
        out["heading_deg"] = _norm_angle(float(heading))

    if dop is not None and dop >= 0.0:
        out["gps_dop"] = float(dop)

    if hpe is not None and hpe >= 0.0:
        out["gps_hposition_error_m"] = float(hpe)

    return out


def _read_coarse_seed(artifacts: Dict[str, Any]) -> Dict[str, Any]:
    if not isinstance(artifacts, dict):
        return {}

    coarse = artifacts.get("coarse_geo_search") or {}
    if not isinstance(coarse, dict):
        return {}

    status = str(coarse.get("status", "")).strip().lower()
    lat = _safe_float(coarse.get("best_lat"))
    lon = _safe_float(coarse.get("best_lon"))

    if status != "ready":
        return {}

    if lat is None or lon is None:
        return {}

    if not (-90.0 <= float(lat) <= 90.0 and -180.0 <= float(lon) <= 180.0):
        return {}

    out: Dict[str, Any] = {
        "lat": float(lat),
        "lon": float(lon),
    }

    radius = _safe_float(coarse.get("search_radius_m"))
    if radius is not None and radius > 0.0:
        out["search_radius_m"] = float(radius)

    confidence = _safe_float(coarse.get("confidence"))
    if confidence is not None:
        out["confidence"] = max(0.0, min(1.0, float(confidence)))

    method = str(coarse.get("method", "")).strip()
    if method:
        out["method"] = method

    seed_source = str(coarse.get("seed_source", "")).strip()
    if seed_source:
        out["seed_source"] = seed_source

    return out


def _search_radius_m(
        seed_source: str,
        seed: Dict[str, Any],
        exif_seed: Dict[str, Any],
        cfg: Dict[str, Any],
) -> float:
    default_radius = float(cfg.get("default_search_radius_m", 35.0))
    min_radius = float(cfg.get("min_search_radius_m", 8.0))
    max_radius = float(cfg.get("max_search_radius_m", 150.0))

    if str(seed_source).lower() == "coarse":
        coarse_default = float(cfg.get("coarse_default_refine_radius_m", 45.0))
        coarse_min = float(cfg.get("coarse_min_refine_radius_m", 20.0))
        coarse_max = float(cfg.get("coarse_max_refine_radius_m", min(max_radius, 90.0)))
        coarse_scale = float(cfg.get("coarse_radius_scale", 0.08))

        base = _safe_float(seed.get("local_refine_radius_m"))
        if base is None or base <= 0.0:
            coarse_radius = _safe_float(seed.get("search_radius_m"))
            if coarse_radius is not None and coarse_radius > 0.0:
                base = max(coarse_min, min(coarse_max, float(coarse_radius) * coarse_scale))
            else:
                base = coarse_default

            coarse_conf = _safe_float(seed.get("confidence"))
            if coarse_conf is not None:
                if coarse_conf >= 0.80:
                    base = min(base, float(cfg.get("coarse_high_conf_radius_m", 30.0)))
                elif coarse_conf < 0.45:
                    base = max(base, float(cfg.get("coarse_low_conf_radius_m", 60.0)))

        return max(min_radius, min(max_radius, float(base)))

    if "gps_hposition_error_m" in exif_seed:
        base = max(6.0, float(exif_seed["gps_hposition_error_m"]) * 1.5)
    elif "gps_dop" in exif_seed:
        base = max(8.0, float(exif_seed["gps_dop"]) * 8.0)
    else:
        base = default_radius

    return max(min_radius, min(max_radius, float(base)))


def _primary_vehicle(detections: List[Dict[str, Any]]) -> Dict[str, Any]:
    best = None
    best_key = None

    for idx, det in enumerate(detections):
        meta = det.get("meta") or {}
        dist_m = _safe_float(meta.get("dist_m"))

        bbox = det.get("bbox_xyxy")
        area = None
        if isinstance(bbox, (list, tuple)) and len(bbox) == 4:
            try:
                x1, y1, x2, y2 = map(float, bbox)
                area = max(0.0, x2 - x1) * max(0.0, y2 - y1)
            except Exception:
                area = None

        if dist_m is not None and dist_m > 0.0:
            key = (0, dist_m)
        elif area is not None and area > 0.0:
            key = (1, -area)
        else:
            key = (2, idx)

        if best is None or key < best_key:
            best = det
            best_key = key

    if not isinstance(best, dict):
        return {}

    meta = best.get("meta") or {}
    bbox = best.get("bbox_xyxy")
    out: Dict[str, Any] = {
        "bbox_xyxy": bbox,
        "cls_name": best.get("cls_name"),
        "conf": best.get("conf"),
        "dist_m": _safe_float(meta.get("dist_m")),
    }

    if isinstance(bbox, (list, tuple)) and len(bbox) == 4:
        try:
            x1, y1, x2, y2 = map(float, bbox)
            out["bottom_center_px"] = [round((x1 + x2) * 0.5, 3), round(y2, 3)]
        except Exception:
            pass

    return out


def _meters_per_deg_lat() -> float:
    return 111320.0

def _refined_heading(exif_heading: Optional[float], nearest_road: Dict[str, Any], cfg: Dict[str, Any]) -> Tuple[Optional[float], str]:
    road_status = str(nearest_road.get("status", ""))
    if road_status != "ok":
        if exif_heading is not None:
            return _norm_angle(float(exif_heading)), "exif"
        return None, "none"

    road_bearing = _safe_float(nearest_road.get("segment_bearing_deg"))
    if road_bearing is None:
        if exif_heading is not None:
            return _norm_angle(float(exif_heading)), "exif"
        return None, "none"

    cand1 = _norm_angle(float(road_bearing))
    cand2 = _norm_angle(float(road_bearing) + 180.0)

    if exif_heading is None:
        return cand1, "road_axis"

    heading = _norm_angle(float(exif_heading))
    chosen = cand1 if _angle_delta(heading, cand1) <= _angle_delta(heading, cand2) else cand2
    max_snap = float(cfg.get("max_heading_snap_deg", 35.0))

    if _angle_delta(heading, chosen) <= max_snap:
        return chosen, "road_axis_snapped"

    return heading, "exif"

def _meters_per_deg_lon(lat_deg: float) -> float:
    return max(1e-6, 111320.0 * math.cos(math.radians(lat_deg)))


def _xy_from_latlon(lat: float, lon: float, lat0: float, lon0: float) -> Tuple[float, float]:
    x = (lon - lon0) * _meters_per_deg_lon(lat0)
    y = (lat - lat0) * _meters_per_deg_lat()
    return x, y


def _latlon_from_xy(x: float, y: float, lat0: float, lon0: float) -> Tuple[float, float]:
    lat = lat0 + y / _meters_per_deg_lat()
    lon = lon0 + x / _meters_per_deg_lon(lat0)
    return lat, lon


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
    return max(-25.0, min(25.0, float(offset)))


def _candidate_offsets(search_radius_m: float) -> Tuple[List[float], List[float]]:
    r = max(6.0, float(search_radius_m))
    half = 0.5 * r
    along_values = [-r, -half, 0.0, half, r]

    cross_step = max(4.0, min(10.0, r * 0.25))
    cross_values = [0.0, cross_step, -cross_step, 2.0 * cross_step, -2.0 * cross_step]

    uniq_along: List[float] = []
    uniq_cross: List[float] = []

    for v in along_values:
        vv = round(float(v), 3)
        if vv not in uniq_along:
            uniq_along.append(vv)

    for v in cross_values:
        vv = round(float(v), 3)
        if vv not in uniq_cross:
            uniq_cross.append(vv)

    return uniq_along, uniq_cross


def _candidate_search(
        init_lat: float,
        init_lon: float,
        exif_heading: Optional[float],
        primary_vehicle: Dict[str, Any],
        nearest_road: Dict[str, Any],
        search_radius_m: float,
        cfg: Dict[str, Any],
        artifacts: Dict[str, Any],
        image_w: int,
) -> Dict[str, Any]:
    road_status = str(nearest_road.get("status", ""))
    road_lat = _safe_float(nearest_road.get("point_lat"))
    road_lon = _safe_float(nearest_road.get("point_lon"))
    road_bearing = _safe_float(nearest_road.get("segment_bearing_deg"))

    base_heading, heading_source = _refined_heading(exif_heading, nearest_road, cfg)
    heading_offset = _vehicle_heading_offset_deg(primary_vehicle, artifacts, image_w)

    if base_heading is not None:
        final_heading = _norm_angle(base_heading + heading_offset)
    else:
        final_heading = None

    if road_status != "ok" or road_lat is None or road_lon is None or road_bearing is None:
        return {
            "mode": "fallback_init",
            "evaluated_count": 1,
            "best_score": 0.0,
            "best_candidate": {
                "lat": round(float(init_lat), 8),
                "lon": round(float(init_lon), 8),
                "heading_deg": round(float(final_heading), 3) if final_heading is not None else None,
                "dist_from_init_m": 0.0,
                "cross_offset_m": None,
                "along_offset_m": None,
                "score": 0.0,
            },
            "heading_source": heading_source,
            "heading_offset_deg": round(float(heading_offset), 3),
            "top_candidates": [],
        }

    road_x, road_y = _xy_from_latlon(float(road_lat), float(road_lon), float(init_lat), float(init_lon))

    theta = math.radians(float(road_bearing))
    ux = math.sin(theta)
    uy = math.cos(theta)
    vx = math.sin(theta + math.pi / 2.0)
    vy = math.cos(theta + math.pi / 2.0)

    along_values, cross_values = _candidate_offsets(search_radius_m)
    max_snap = float(cfg.get("max_heading_snap_deg", 35.0))
    cross_limit = max(6.0, min(20.0, float(search_radius_m) * 0.5))

    candidates: List[Dict[str, Any]] = []

    init_cross = _safe_float(nearest_road.get("distance_m"))
    init_heading_score = 0.5
    if base_heading is not None and exif_heading is not None:
        init_heading_score = max(0.0, 1.0 - (_angle_delta(base_heading, exif_heading) / max(1.0, max_snap)))

    init_road_score = 0.0
    if init_cross is not None:
        init_road_score = max(0.0, 1.0 - abs(float(init_cross)) / cross_limit)

    init_score = 0.55 * 1.0 + 0.30 * init_road_score + 0.15 * init_heading_score
    candidates.append({
        "lat": round(float(init_lat), 8),
        "lon": round(float(init_lon), 8),
        "heading_deg": round(float(final_heading), 3) if final_heading is not None else None,
        "dist_from_init_m": 0.0,
        "cross_offset_m": init_cross,
        "along_offset_m": 0.0,
        "score": round(float(init_score), 6),
    })

    for along in along_values:
        for cross in cross_values:
            cx = road_x + along * ux + cross * vx
            cy = road_y + along * uy + cross * vy

            dist_from_init = math.hypot(cx, cy)
            if dist_from_init > float(search_radius_m) * 1.15:
                continue

            lat, lon = _latlon_from_xy(cx, cy, float(init_lat), float(init_lon))

            position_score = max(0.0, 1.0 - dist_from_init / max(1.0, float(search_radius_m)))
            road_score = max(0.0, 1.0 - abs(float(cross)) / cross_limit)

            heading_score = 0.5
            if base_heading is not None and exif_heading is not None:
                heading_score = max(0.0, 1.0 - (_angle_delta(base_heading, exif_heading) / max(1.0, max_snap)))

            score = 0.55 * position_score + 0.30 * road_score + 0.15 * heading_score

            candidates.append({
                "lat": round(float(lat), 8),
                "lon": round(float(lon), 8),
                "heading_deg": round(float(final_heading), 3) if final_heading is not None else None,
                "dist_from_init_m": round(float(dist_from_init), 3),
                "cross_offset_m": round(float(cross), 3),
                "along_offset_m": round(float(along), 3),
                "score": round(float(score), 6),
            })

    candidates.sort(key=lambda item: item["score"], reverse=True)
    best = candidates[0] if candidates else {
        "lat": round(float(init_lat), 8),
        "lon": round(float(init_lon), 8),
        "heading_deg": round(float(final_heading), 3) if final_heading is not None else None,
        "dist_from_init_m": 0.0,
        "cross_offset_m": None,
        "along_offset_m": None,
        "score": 0.0,
    }

    return {
        "mode": "road_candidate_search",
        "evaluated_count": int(len(candidates)),
        "best_score": round(float(best.get("score", 0.0)), 6),
        "best_candidate": best,
        "heading_source": heading_source,
        "heading_offset_deg": round(float(heading_offset), 3),
        "top_candidates": candidates[:10],
    }


def refine_camera_pose(
        image_path: str,
        detections: List[Dict[str, Any]],
        image_w: int,
        image_h: int,
        cfg: Optional[Dict[str, Any]] = None,
        artifacts: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    cfg = cfg or {}
    artifacts = artifacts or {}

    exif_seed = _read_exif_seed(image_path)
    coarse_seed = _read_coarse_seed(artifacts)

    seed_source = "none"
    init_lat = None
    init_lon = None

    if "lat" in coarse_seed and "lon" in coarse_seed:
        seed_source = "coarse"
        init_lat = coarse_seed.get("lat")
        init_lon = coarse_seed.get("lon")
    elif "lat" in exif_seed and "lon" in exif_seed:
        seed_source = "exif"
        init_lat = exif_seed.get("lat")
        init_lon = exif_seed.get("lon")

    exif_heading = exif_seed.get("heading_deg")

    active_seed = coarse_seed if seed_source == "coarse" else exif_seed
    search_radius_m = _search_radius_m(
        seed_source=seed_source,
        seed=active_seed,
        exif_seed=exif_seed,
        cfg=cfg,
    )

    primary_vehicle = _primary_vehicle(detections)
    scene_cfg = cfg.get("scene_features", {}) if isinstance(cfg.get("scene_features", {}), dict) else {}
    scene_features = extract_scene_features(
        image_path=image_path,
        detections=detections,
        image_w=int(image_w),
        image_h=int(image_h),
        cfg=scene_cfg,
    )

    if init_lat is None or init_lon is None:
        return {
            "status": "manual_required",
            "camera_init_lat": None,
            "camera_init_lon": None,
            "camera_search_radius_m": round(float(search_radius_m), 3),
            "camera_refined_lat": None,
            "camera_refined_lon": None,
            "camera_refined_azimuth_deg": None,
            "camera_confidence": 0.0,
            "camera_uncertainty_m": round(float(search_radius_m), 3),
            "refine_method": "coarse_or_exif_seed_candidate_search_v1",
            "refine_debug_path": "",
            "needs_manual_review": True,
            "seed_source": "none",
            "azimuth_source": "none",
            "offline_context": {
                "status": "no_center",
                "search_radius_m": round(float(search_radius_m), 3),
            },
            "primary_vehicle": primary_vehicle,
            "scene_features": scene_features,
            "inputs": {
                "image_w": int(image_w),
                "image_h": int(image_h),
                "detections_count": int(len(detections)),
                "has_cleaned_image": bool(artifacts.get("cleaned_image_path")),
                "has_csv": bool(artifacts.get("csv_path")),
                "has_coarse_seed": bool(coarse_seed),
                "has_exif_seed": bool(exif_seed.get("lat") is not None and exif_seed.get("lon") is not None),
            },
        }

    offline_cfg = cfg.get("offline_map", {}) if isinstance(cfg.get("offline_map", {}), dict) else {}
    offline_context = load_offline_context(
        center_lat=float(init_lat),
        center_lon=float(init_lon),
        search_radius_m=float(search_radius_m),
        cfg=offline_cfg,
    )
    nearest_road = offline_context.get("nearest_road", {}) or {}

    candidate_search = _candidate_search(
        init_lat=float(init_lat),
        init_lon=float(init_lon),
        exif_heading=exif_heading,
        primary_vehicle=primary_vehicle,
        nearest_road=nearest_road,
        search_radius_m=float(search_radius_m),
        cfg=cfg,
        artifacts=artifacts,
        image_w=int(image_w),
    )

    best_candidate = candidate_search.get("best_candidate", {}) or {}
    refined_lat = _safe_float(best_candidate.get("lat"))
    refined_lon = _safe_float(best_candidate.get("lon"))
    refined_heading = _safe_float(best_candidate.get("heading_deg"))
    best_score = _safe_float(candidate_search.get("best_score"))
    if best_score is None:
        best_score = 0.0

    confidence = 0.25

    if seed_source == "coarse":
        coarse_conf = _safe_float(coarse_seed.get("confidence"))
        if coarse_conf is not None:
            confidence += 0.15 * max(0.0, min(1.0, float(coarse_conf)))
        else:
            confidence += 0.08
    elif exif_seed.get("raw_gps"):
        confidence += 0.15

    if nearest_road.get("status") == "ok":
        confidence += 0.15
    if nearest_road.get("within_search_radius") is True:
        confidence += 0.10
    if primary_vehicle:
        confidence += 0.10
    if refined_heading is not None:
        confidence += 0.10

    confidence += 0.25 * max(0.0, min(1.0, float(best_score)))
    confidence = max(0.0, min(1.0, confidence))

    uncertainty = float(search_radius_m)
    best_cross = _safe_float(best_candidate.get("cross_offset_m"))
    if best_cross is not None:
        uncertainty = min(uncertainty, max(6.0, abs(float(best_cross)) + 8.0))
    elif nearest_road.get("status") == "ok" and nearest_road.get("within_search_radius") is True:
        uncertainty = min(uncertainty, max(6.0, float(nearest_road.get("distance_m", uncertainty)) + 8.0))

    needs_manual_review = bool(confidence < float(cfg.get("min_auto_confidence", 0.60)))

    return {
        "status": "ready",
        "camera_init_lat": round(float(init_lat), 8),
        "camera_init_lon": round(float(init_lon), 8),
        "camera_search_radius_m": round(float(search_radius_m), 3),
        "camera_refined_lat": round(float(refined_lat), 8) if refined_lat is not None else round(float(init_lat), 8),
        "camera_refined_lon": round(float(refined_lon), 8) if refined_lon is not None else round(float(init_lon), 8),
        "camera_refined_azimuth_deg": round(float(refined_heading), 3) if refined_heading is not None else None,
        "camera_confidence": round(float(confidence), 6),
        "camera_uncertainty_m": round(float(uncertainty), 3),
        "refine_method": "coarse_seed_candidate_search_v1" if seed_source == "coarse" else "seed_window_candidate_search_v1",
        "refine_debug_path": "",
        "needs_manual_review": needs_manual_review,
        "seed_source": seed_source,
        "azimuth_source": str(candidate_search.get("heading_source", "")),
        "offline_context": offline_context,
        "candidate_search": candidate_search,
        "primary_vehicle": primary_vehicle,
        "scene_features": scene_features,
        "inputs": {
            "image_w": int(image_w),
            "image_h": int(image_h),
            "detections_count": int(len(detections)),
            "has_cleaned_image": bool(artifacts.get("cleaned_image_path")),
            "has_csv": bool(artifacts.get("csv_path")),
            "has_coarse_seed": bool(coarse_seed),
            "has_exif_seed": bool(exif_seed.get("lat") is not None and exif_seed.get("lon") is not None),
        },
    }