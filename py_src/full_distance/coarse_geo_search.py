from __future__ import annotations

import math
from typing import Any, Dict, List, Optional

from .offline_map_data import load_offline_context
from .scene_features import extract_scene_features


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


def _angle_delta(a: float, b: float) -> float:
    d = abs(_norm_angle(float(a)) - _norm_angle(float(b)))
    return min(d, 360.0 - d)


def _pick_seed(artifacts: Dict[str, Any], cfg: Dict[str, Any]) -> Dict[str, Any]:
    seed: Dict[str, Any] = {
        "lat": None,
        "lon": None,
        "heading_deg": None,
        "search_radius_m": float(cfg.get("coarse_default_search_radius_m", 3000.0) or 3000.0),
        "seed_source": "none",
    }

    manual_area = artifacts.get("manual_search_area") or {}
    manual_lat = _safe_float(manual_area.get("center_lat"))
    manual_lon = _safe_float(manual_area.get("center_lon"))
    manual_radius = _safe_float(manual_area.get("radius_m"))

    if manual_lat is not None and manual_lon is not None:
        seed["lat"] = manual_lat
        seed["lon"] = manual_lon
        if manual_radius is not None and manual_radius > 0.0:
            seed["search_radius_m"] = float(manual_radius)
        seed["seed_source"] = "manual_area"
        return seed

    exif_gps = artifacts.get("exif_gps") or {}
    exif_lat = _safe_float(exif_gps.get("lat"))
    exif_lon = _safe_float(exif_gps.get("lon"))
    exif_heading = _safe_float(exif_gps.get("heading_deg"))
    exif_radius = _safe_float(exif_gps.get("search_radius_m"))

    if exif_lat is not None and exif_lon is not None:
        seed["lat"] = exif_lat
        seed["lon"] = exif_lon
        seed["heading_deg"] = exif_heading
        if exif_radius is not None and exif_radius > 0.0:
            seed["search_radius_m"] = float(exif_radius)
        seed["seed_source"] = "exif"
        return seed

    gps_hint = artifacts.get("gps_hint") or {}
    gps_lat = _safe_float(gps_hint.get("lat"))
    gps_lon = _safe_float(gps_hint.get("lon"))
    gps_heading = _safe_float(gps_hint.get("heading_deg"))
    gps_radius = _safe_float(gps_hint.get("radius_m"))

    if gps_lat is not None and gps_lon is not None:
        seed["lat"] = gps_lat
        seed["lon"] = gps_lon
        seed["heading_deg"] = gps_heading
        if gps_radius is not None and gps_radius > 0.0:
            seed["search_radius_m"] = float(gps_radius)
        seed["seed_source"] = "gps"
        return seed

    return seed


def _score_candidate(
        candidate: Dict[str, Any],
        scene: Dict[str, Any],
        seed_heading: Optional[float],
        cfg: Dict[str, Any],
) -> Dict[str, Any]:
    dist_m = float(candidate.get("distance_m", 1e18))
    bearing = _safe_float(candidate.get("segment_bearing_deg"))
    seg_len = float(candidate.get("segment_length_m", 0.0))

    quality = scene.get("quality", {}) if isinstance(scene.get("quality"), dict) else {}
    scene_score = float(quality.get("scene_score", 0.0) or 0.0)
    has_two_sides = bool(quality.get("has_two_sides", False))
    has_vp = bool(quality.get("has_vanishing_point", False))

    road_axis_angle = _safe_float(scene.get("road_axis_angle_img_deg"))
    corridor_width = _safe_float(scene.get("road_corridor_bottom_width_ratio"))
    vertical_count = float(scene.get("vertical_structure_count", 0.0) or 0.0)

    score = 0.0

    radius_norm = max(50.0, float(cfg.get("coarse_distance_norm_m", 800.0) or 800.0))
    score += 0.30 * max(0.0, 1.0 - dist_m / radius_norm)

    score += 0.25 * max(0.0, min(1.0, scene_score))

    if has_two_sides:
        score += 0.10
    if has_vp:
        score += 0.10

    if corridor_width is not None:
        width_pref = float(cfg.get("coarse_corridor_pref", 0.45) or 0.45)
        width_tol = float(cfg.get("coarse_corridor_tol", 0.30) or 0.30)
        score += 0.10 * max(0.0, 1.0 - abs(corridor_width - width_pref) / max(1e-6, width_tol))

    if vertical_count > 0.0:
        score += 0.05 * min(1.0, vertical_count / 8.0)

    if seed_heading is not None and bearing is not None:
        d1 = _angle_delta(seed_heading, bearing)
        d2 = _angle_delta(seed_heading, bearing + 180.0)
        score += 0.10 * max(0.0, 1.0 - min(d1, d2) / 45.0)

    if road_axis_angle is not None and bearing is not None:
        axis_strength = max(0.0, 1.0 - min(abs(road_axis_angle), 45.0) / 45.0)
        score += 0.05 * axis_strength

    if seg_len > 0.0:
        score += 0.05 * min(1.0, seg_len / 60.0)

    out = dict(candidate)
    out["score"] = round(float(max(0.0, min(1.0, score))), 6)
    out["scene_score"] = round(float(scene_score), 6)
    return out


def run(
        image_path: str,
        detections: List[Dict[str, Any]],
        image_w: int,
        image_h: int,
        artifacts: Dict[str, Any],
        cfg: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    cfg = cfg or {}

    seed = _pick_seed(artifacts, cfg)
    seed_lat = _safe_float(seed.get("lat"))
    seed_lon = _safe_float(seed.get("lon"))
    search_radius_m = float(seed.get("search_radius_m", 3000.0) or 3000.0)

    out: Dict[str, Any] = {
        "status": "no_seed",
        "method": "coarse_geo_search_v1",
        "seed_source": str(seed.get("seed_source", "none")),
        "search_radius_m": round(float(search_radius_m), 3),
        "best_lat": None,
        "best_lon": None,
        "confidence": 0.0,
        "top_candidates": [],
        "scene_features": {},
    }

    if seed_lat is None or seed_lon is None:
        return out

    scene = extract_scene_features(
        image_path=image_path,
        detections=detections,
        image_w=image_w,
        image_h=image_h,
        cfg=(cfg.get("scene_features") or {}),
    )
    out["scene_features"] = scene

    offline_context = load_offline_context(seed_lat, seed_lon, search_radius_m, (cfg.get("offline_map") or {}))
    road_candidates = offline_context.get("road_candidates", [])
    if not isinstance(road_candidates, list) or not road_candidates:
        out["status"] = "no_candidates"
        return out

    seed_heading = _safe_float(seed.get("heading_deg"))
    scored: List[Dict[str, Any]] = []

    for cand in road_candidates:
        if not isinstance(cand, dict):
            continue
        scored.append(_score_candidate(cand, scene, seed_heading, cfg))

    if not scored:
        out["status"] = "no_candidates"
        return out

    scored.sort(key=lambda c: float(c.get("score", 0.0)), reverse=True)
    top_k = int(cfg.get("coarse_keep_top_k", 8) or 8)
    top_candidates = scored[:top_k]
    best = top_candidates[0]

    out["status"] = "ready"
    out["best_lat"] = best.get("point_lat")
    out["best_lon"] = best.get("point_lon")
    out["confidence"] = float(best.get("score", 0.0))
    out["top_candidates"] = top_candidates
    return out