from __future__ import annotations

import json
import math
import os
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple


def _safe_float(value: Any) -> Optional[float]:
    if value is None:
        return None
    try:
        return float(value)
    except Exception:
        return None


def _norm_lon(lon: float) -> float:
    while lon > 180.0:
        lon -= 360.0
    while lon < -180.0:
        lon += 360.0
    return lon


def _meters_per_deg_lat() -> float:
    return 111320.0


def _meters_per_deg_lon(lat_deg: float) -> float:
    return max(1e-6, 111320.0 * math.cos(math.radians(lat_deg)))


def _xy_from_latlon(lat: float, lon: float, lat0: float, lon0: float) -> Tuple[float, float]:
    x = (lon - lon0) * _meters_per_deg_lon(lat0)
    y = (lat - lat0) * _meters_per_deg_lat()
    return x, y


def _latlon_from_xy(x: float, y: float, lat0: float, lon0: float) -> Tuple[float, float]:
    lat = lat0 + y / _meters_per_deg_lat()
    lon = lon0 + x / _meters_per_deg_lon(lat0)
    return lat, _norm_lon(lon)


def _haversine_m(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    r = 6371000.0
    p1 = math.radians(lat1)
    p2 = math.radians(lat2)
    dp = math.radians(lat2 - lat1)
    dl = math.radians(lon2 - lon1)
    a = math.sin(dp / 2.0) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2.0) ** 2
    return 2.0 * r * math.atan2(math.sqrt(a), math.sqrt(max(0.0, 1.0 - a)))


def _bearing_deg(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    p1 = math.radians(lat1)
    p2 = math.radians(lat2)
    dl = math.radians(lon2 - lon1)

    y = math.sin(dl) * math.cos(p2)
    x = math.cos(p1) * math.sin(p2) - math.sin(p1) * math.cos(p2) * math.cos(dl)
    b = math.degrees(math.atan2(y, x))
    while b < 0.0:
        b += 360.0
    while b >= 360.0:
        b -= 360.0
    return b


def _resolve_path(base_dir: Path, raw: str) -> Path:
    p = Path(raw)
    if p.is_absolute():
        return p
    return (base_dir / p).resolve()


def _load_geojson(path: Path) -> Dict[str, Any]:
    try:
        with open(path, "r", encoding="utf-8") as f:
            obj = json.load(f)
        if isinstance(obj, dict):
            return obj
    except Exception:
        pass
    return {}


def _iter_coords(geometry: Dict[str, Any]) -> List[Tuple[float, float]]:
    gtype = str(geometry.get("type", "") or "")
    coords = geometry.get("coordinates", None)

    out: List[Tuple[float, float]] = []

    def add_pair(pair: Any) -> None:
        if not isinstance(pair, (list, tuple)) or len(pair) < 2:
            return
        lon = _safe_float(pair[0])
        lat = _safe_float(pair[1])
        if lon is None or lat is None:
            return
        if -90.0 <= lat <= 90.0 and -180.0 <= lon <= 180.0:
            out.append((lat, lon))

    if gtype == "Point":
        add_pair(coords)
    elif gtype in ("MultiPoint", "LineString"):
        if isinstance(coords, list):
            for p in coords:
                add_pair(p)
    elif gtype in ("MultiLineString", "Polygon"):
        if isinstance(coords, list):
            for line in coords:
                if isinstance(line, list):
                    for p in line:
                        add_pair(p)
    elif gtype == "MultiPolygon":
        if isinstance(coords, list):
            for poly in coords:
                if isinstance(poly, list):
                    for line in poly:
                        if isinstance(line, list):
                            for p in line:
                                add_pair(p)

    return out


def _iter_lines(geometry: Dict[str, Any]) -> List[List[Tuple[float, float]]]:
    gtype = str(geometry.get("type", "") or "")
    coords = geometry.get("coordinates", None)

    def parse_line(raw: Any) -> List[Tuple[float, float]]:
        if not isinstance(raw, list):
            return []
        pts: List[Tuple[float, float]] = []
        for p in raw:
            if not isinstance(p, (list, tuple)) or len(p) < 2:
                continue
            lon = _safe_float(p[0])
            lat = _safe_float(p[1])
            if lon is None or lat is None:
                continue
            if -90.0 <= lat <= 90.0 and -180.0 <= lon <= 180.0:
                pts.append((lat, lon))
        return pts

    lines: List[List[Tuple[float, float]]] = []

    if gtype == "LineString":
        line = parse_line(coords)
        if len(line) >= 2:
            lines.append(line)
    elif gtype == "MultiLineString":
        if isinstance(coords, list):
            for raw in coords:
                line = parse_line(raw)
                if len(line) >= 2:
                    lines.append(line)
    elif gtype == "Polygon":
        if isinstance(coords, list):
            for raw in coords:
                line = parse_line(raw)
                if len(line) >= 2:
                    lines.append(line)
    elif gtype == "MultiPolygon":
        if isinstance(coords, list):
            for poly in coords:
                if isinstance(poly, list):
                    for raw in poly:
                        line = parse_line(raw)
                        if len(line) >= 2:
                            lines.append(line)

    return lines


def _feature_is_near(feature: Dict[str, Any], center_lat: float, center_lon: float, radius_m: float) -> bool:
    geom = feature.get("geometry", {}) or {}
    pts = _iter_coords(geom)
    if not pts:
        return False

    limit = max(radius_m, 1.0) * 1.5
    for lat, lon in pts:
        if _haversine_m(center_lat, center_lon, lat, lon) <= limit:
            return True
    return False


def _project_point_to_segment(
        center_lat: float,
        center_lon: float,
        a_lat: float,
        a_lon: float,
        b_lat: float,
        b_lon: float,
) -> Tuple[float, float, float]:
    ax, ay = _xy_from_latlon(a_lat, a_lon, center_lat, center_lon)
    bx, by = _xy_from_latlon(b_lat, b_lon, center_lat, center_lon)

    abx = bx - ax
    aby = by - ay
    ab2 = abx * abx + aby * aby

    if ab2 <= 1e-9:
        px, py = ax, ay
    else:
        t = -(ax * abx + ay * aby) / ab2
        t = max(0.0, min(1.0, t))
        px = ax + t * abx
        py = ay + t * aby

    dist = math.hypot(px, py)
    plat, plon = _latlon_from_xy(px, py, center_lat, center_lon)
    return dist, plat, plon


def _nearest_road(
        roads: List[Dict[str, Any]],
        center_lat: float,
        center_lon: float,
        radius_m: float,
) -> Dict[str, Any]:
    best: Dict[str, Any] = {}
    best_dist = float("inf")

    for feature_index, feature in enumerate(roads):
        geom = feature.get("geometry", {}) or {}
        lines = _iter_lines(geom)
        if not lines:
            continue

        for line_index, line in enumerate(lines):
            for i in range(len(line) - 1):
                a_lat, a_lon = line[i]
                b_lat, b_lon = line[i + 1]

                dist_m, proj_lat, proj_lon = _project_point_to_segment(
                    center_lat, center_lon, a_lat, a_lon, b_lat, b_lon
                )

                if dist_m < best_dist:
                    best_dist = dist_m
                    best = {
                        "distance_m": round(float(dist_m), 3),
                        "point_lat": round(float(proj_lat), 8),
                        "point_lon": round(float(proj_lon), 8),
                        "segment_bearing_deg": round(float(_bearing_deg(a_lat, a_lon, b_lat, b_lon)), 3),
                        "feature_index": int(feature_index),
                        "line_index": int(line_index),
                        "segment_index": int(i),
                        "properties": feature.get("properties", {}) or {},
                    }

    if not best:
        return {"status": "not_found"}

    best["status"] = "ok"
    best["within_search_radius"] = bool(float(best["distance_m"]) <= max(radius_m, 1.0))
    return best
def _segment_length_m(a_lat: float, a_lon: float, b_lat: float, b_lon: float) -> float:
    return _haversine_m(a_lat, a_lon, b_lat, b_lon)


def _segment_midpoint(a_lat: float, a_lon: float, b_lat: float, b_lon: float) -> Tuple[float, float]:
    return (0.5 * (float(a_lat) + float(b_lat)), 0.5 * (float(a_lon) + float(b_lon)))


def _road_candidates(
        roads: List[Dict[str, Any]],
        center_lat: float,
        center_lon: float,
        radius_m: float,
        top_k: int,
) -> List[Dict[str, Any]]:
    candidates: List[Dict[str, Any]] = []

    for feature_index, feature in enumerate(roads):
        geom = feature.get("geometry", {}) or {}
        props = feature.get("properties", {}) or {}
        lines = _iter_lines(geom)
        if not lines:
            continue

        for line_index, line in enumerate(lines):
            for segment_index in range(len(line) - 1):
                a_lat, a_lon = line[segment_index]
                b_lat, b_lon = line[segment_index + 1]

                dist_m, proj_lat, proj_lon = _project_point_to_segment(
                    center_lat, center_lon, a_lat, a_lon, b_lat, b_lon
                )

                seg_len_m = _segment_length_m(a_lat, a_lon, b_lat, b_lon)
                mid_lat, mid_lon = _segment_midpoint(a_lat, a_lon, b_lat, b_lon)

                candidates.append({
                    "distance_m": round(float(dist_m), 3),
                    "point_lat": round(float(proj_lat), 8),
                    "point_lon": round(float(proj_lon), 8),
                    "mid_lat": round(float(mid_lat), 8),
                    "mid_lon": round(float(mid_lon), 8),
                    "segment_length_m": round(float(seg_len_m), 3),
                    "segment_bearing_deg": round(float(_bearing_deg(a_lat, a_lon, b_lat, b_lon)), 3),
                    "feature_index": int(feature_index),
                    "line_index": int(line_index),
                    "segment_index": int(segment_index),
                    "within_search_radius": bool(float(dist_m) <= max(radius_m, 1.0)),
                    "properties": props,
                })

    candidates.sort(
        key=lambda c: (
            float(c.get("distance_m", 1e18)),
            -float(c.get("segment_length_m", 0.0)),
        )
    )

    if top_k > 0:
        candidates = candidates[:top_k]

    return candidates

def _extract_features(doc: Dict[str, Any]) -> List[Dict[str, Any]]:
    feats = doc.get("features", None)
    if isinstance(feats, list):
        return [f for f in feats if isinstance(f, dict)]
    if doc.get("type") == "Feature" and isinstance(doc.get("geometry"), dict):
        return [doc]
    return []


def load_offline_context(center_lat: Optional[float], center_lon: Optional[float], search_radius_m: float, cfg: Dict[str, Any]) -> Dict[str, Any]:
    out: Dict[str, Any] = {
        "status": "no_center",
        "search_radius_m": round(float(search_radius_m), 3),
        "base_dir": "",
        "local_tiles_dir": "",
        "layers": {
            "roads": {"path": "", "exists": False, "total": 0, "selected": 0},
            "intersections": {"path": "", "exists": False, "total": 0, "selected": 0},
            "buildings": {"path": "", "exists": False, "total": 0, "selected": 0},
        },
        "nearest_road": {"status": "not_found"},
        "road_candidates": [],
    }

    if center_lat is None or center_lon is None:
        return out

    base_dir = Path(str(cfg.get("base_dir") or "offline_map")).resolve()
    roads_path = _resolve_path(base_dir, str(cfg.get("roads_geojson") or "roads.geojson"))
    intersections_path = _resolve_path(base_dir, str(cfg.get("intersections_geojson") or "intersections.geojson"))
    buildings_path = _resolve_path(base_dir, str(cfg.get("buildings_geojson") or "buildings.geojson"))
    local_tiles_dir = _resolve_path(base_dir, str(cfg.get("local_tiles_dir") or "tiles"))

    out["status"] = "ok"
    out["base_dir"] = str(base_dir)
    out["local_tiles_dir"] = str(local_tiles_dir)

    max_features = int(cfg.get("max_features_per_layer", 5000) or 5000)
    selected_roads: List[Dict[str, Any]] = []

    for layer_name, path in (
            ("roads", roads_path),
            ("intersections", intersections_path),
            ("buildings", buildings_path),
    ):
        layer = out["layers"][layer_name]
        layer["path"] = str(path)
        layer["exists"] = path.exists()

        if not path.exists():
            continue

        doc = _load_geojson(path)
        feats = _extract_features(doc)
        layer["total"] = int(len(feats))

        selected: List[Dict[str, Any]] = []
        for feature in feats:
            if _feature_is_near(feature, float(center_lat), float(center_lon), float(search_radius_m)):
                selected.append(feature)
                if len(selected) >= max_features:
                    break

        layer["selected"] = int(len(selected))

        if layer_name == "roads":
            selected_roads = selected

    out["nearest_road"] = _nearest_road(
        selected_roads,
        float(center_lat),
        float(center_lon),
        float(search_radius_m),
    )

    top_k = int(cfg.get("candidate_roads_top_k", 32) or 32)
    out["road_candidates"] = _road_candidates(
        selected_roads,
        float(center_lat),
        float(center_lon),
        float(search_radius_m),
        top_k,
    )

    return out