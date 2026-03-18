from __future__ import annotations

import math
from typing import Any, Dict, List, Optional, Tuple

import cv2
import numpy as np


def _safe_float(value: Any) -> Optional[float]:
    if value is None:
        return None
    try:
        return float(value)
    except Exception:
        return None


def _clip01(v: float) -> float:
    return max(0.0, min(1.0, float(v)))


def _bottom_center(det: Dict[str, Any]) -> Optional[Tuple[float, float]]:
    bb = det.get("bbox_xyxy")
    if not isinstance(bb, (list, tuple)) or len(bb) != 4:
        return None
    try:
        x1, y1, x2, y2 = map(float, bb)
    except Exception:
        return None
    if x2 <= x1 or y2 <= y1:
        return None
    return (0.5 * (x1 + x2), y2)


def _line_len(line: Tuple[int, int, int, int]) -> float:
    x1, y1, x2, y2 = line
    return float(math.hypot(x2 - x1, y2 - y1))


def _line_angle_deg(line: Tuple[int, int, int, int]) -> float:
    x1, y1, x2, y2 = line
    return math.degrees(math.atan2(y2 - y1, x2 - x1))


def _to_points(line: Tuple[int, int, int, int]) -> Tuple[float, float, float, float]:
    x1, y1, x2, y2 = line
    return float(x1), float(y1), float(x2), float(y2)


def _intersect(line_a: Tuple[int, int, int, int], line_b: Tuple[int, int, int, int]) -> Optional[Tuple[float, float]]:
    x1, y1, x2, y2 = _to_points(line_a)
    x3, y3, x4, y4 = _to_points(line_b)

    den = (x1 - x2) * (y3 - y4) - (y1 - y2) * (x3 - x4)
    if abs(den) < 1e-9:
        return None

    px = ((x1 * y2 - y1 * x2) * (x3 - x4) - (x1 - x2) * (x3 * y4 - y3 * x4)) / den
    py = ((x1 * y2 - y1 * x2) * (y3 - y4) - (y1 - y2) * (x3 * y4 - y3 * x4)) / den
    if not (math.isfinite(px) and math.isfinite(py)):
        return None
    return px, py


def _x_at_y(line: Tuple[int, int, int, int], y: float) -> Optional[float]:
    x1, y1, x2, y2 = _to_points(line)
    dy = y2 - y1
    if abs(dy) < 1e-9:
        return None
    t = (y - y1) / dy
    x = x1 + t * (x2 - x1)
    if not math.isfinite(x):
        return None
    return x


def _longest_line(lines: List[Tuple[int, int, int, int]]) -> Optional[Tuple[int, int, int, int]]:
    if not lines:
        return None
    return max(lines, key=_line_len)


def _line_to_dict(line: Optional[Tuple[int, int, int, int]]) -> Dict[str, Any]:
    if line is None:
        return {}
    x1, y1, x2, y2 = line
    return {
        "x1": int(x1),
        "y1": int(y1),
        "x2": int(x2),
        "y2": int(y2),
        "length_px": round(_line_len(line), 3),
        "angle_deg": round(_line_angle_deg(line), 3),
    }


def extract_scene_features(
        image_path: str,
        detections: List[Dict[str, Any]],
        image_w: int,
        image_h: int,
        cfg: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    cfg = cfg or {}

    out: Dict[str, Any] = {
        "status": "image_not_loaded",
        "image_w": int(image_w),
        "image_h": int(image_h),
        "vehicle_bottom_points_px": [],
        "vehicle_bottom_span_ratio": 0.0,
        "edge_density_bottom_half": 0.0,
        "road_edges_count": 0,
        "left_boundary": {},
        "right_boundary": {},
        "vanishing_point_px": None,
        "vanishing_point_strength": 0.0,
        "road_axis_dx_norm": 0.0,
        "road_axis_up_ratio": 0.0,
        "road_axis_angle_img_deg": None,
        "vertical_structure_count": 0,
        "road_corridor_bottom_width_ratio": 0.0,
        "quality": {
            "scene_score": 0.0,
            "has_two_sides": False,
            "has_vanishing_point": False,
            "has_lane_like_geometry": False,
        },
    }

    img = cv2.imread(image_path, cv2.IMREAD_COLOR)
    if img is None or img.size == 0:
        return out

    h, w = img.shape[:2]
    out["image_w"] = int(w)
    out["image_h"] = int(h)

    vehicle_points: List[Tuple[float, float]] = []
    for det in detections:
        bc = _bottom_center(det)
        if bc is not None:
            vehicle_points.append(bc)

    out["vehicle_bottom_points_px"] = [[round(x, 3), round(y, 3)] for x, y in vehicle_points]

    if vehicle_points and w > 0:
        xs = [p[0] for p in vehicle_points]
        out["vehicle_bottom_span_ratio"] = round((max(xs) - min(xs)) / float(w), 6)

    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)

    blur_ksize = int(cfg.get("blur_ksize", 5) or 5)
    if blur_ksize < 3:
        blur_ksize = 3
    if blur_ksize % 2 == 0:
        blur_ksize += 1

    gray = cv2.GaussianBlur(gray, (blur_ksize, blur_ksize), 0)

    roi_top_ratio = float(cfg.get("roi_top_ratio", 0.35) or 0.35)
    roi_top = int(max(0, min(h - 1, round(h * roi_top_ratio))))

    mask = np.zeros((h, w), dtype=np.uint8)
    mask[roi_top:, :] = 255

    vehicle_mask_margin = int(cfg.get("vehicle_mask_margin_px", 8) or 8)
    for det in detections:
        bb = det.get("bbox_xyxy")
        if not isinstance(bb, (list, tuple)) or len(bb) != 4:
            continue
        try:
            x1, y1, x2, y2 = map(int, bb)
        except Exception:
            continue
        x1 = max(0, x1 - vehicle_mask_margin)
        y1 = max(0, y1 - vehicle_mask_margin)
        x2 = min(w, x2 + vehicle_mask_margin)
        y2 = min(h, y2 + vehicle_mask_margin)
        if x2 > x1 and y2 > y1:
            mask[y1:y2, x1:x2] = 0

    canny_low = int(cfg.get("canny_low", 50) or 50)
    canny_high = int(cfg.get("canny_high", 150) or 150)
    edges = cv2.Canny(gray, canny_low, canny_high)
    edges = cv2.bitwise_and(edges, mask)

    bottom_half = edges[h // 2:, :] if h > 1 else edges
    nz = float(np.count_nonzero(bottom_half))
    total = float(bottom_half.size) if bottom_half.size > 0 else 1.0
    out["edge_density_bottom_half"] = round(nz / total, 6)

    hough_threshold = int(cfg.get("hough_threshold", 60) or 60)
    min_line_len = int(cfg.get("min_line_length_px", max(30, int(0.08 * min(w, h)))) or max(30, int(0.08 * min(w, h))))
    max_line_gap = int(cfg.get("max_line_gap_px", 20) or 20)

    raw_lines = cv2.HoughLinesP(
        edges,
        1,
        np.pi / 180.0,
        threshold=hough_threshold,
        minLineLength=min_line_len,
        maxLineGap=max_line_gap,
        )

    all_lines: List[Tuple[int, int, int, int]] = []
    if raw_lines is not None:
        for item in raw_lines:
            if item is None or len(item) == 0:
                continue
            x1, y1, x2, y2 = map(int, item[0])
            all_lines.append((x1, y1, x2, y2))

    out["road_edges_count"] = int(len(all_lines))

    left_lines: List[Tuple[int, int, int, int]] = []
    right_lines: List[Tuple[int, int, int, int]] = []
    vertical_count = 0

    min_abs_angle = float(cfg.get("min_abs_line_angle_deg", 20.0) or 20.0)
    max_abs_angle = float(cfg.get("max_abs_line_angle_deg", 85.0) or 85.0)
    vertical_angle = float(cfg.get("vertical_angle_deg", 78.0) or 78.0)

    for line in all_lines:
        ang = _line_angle_deg(line)
        abs_ang = abs(ang)

        if abs_ang >= vertical_angle:
            vertical_count += 1

        if abs_ang < min_abs_angle or abs_ang > max_abs_angle:
            continue

        if ang < 0.0:
            left_lines.append(line)
        else:
            right_lines.append(line)

    out["vertical_structure_count"] = int(vertical_count)

    left_best = _longest_line(left_lines)
    right_best = _longest_line(right_lines)

    out["left_boundary"] = _line_to_dict(left_best)
    out["right_boundary"] = _line_to_dict(right_best)

    vp: Optional[Tuple[float, float]] = None
    intersections: List[Tuple[float, float]] = []

    sample_left = sorted(left_lines, key=_line_len, reverse=True)[:8]
    sample_right = sorted(right_lines, key=_line_len, reverse=True)[:8]

    for la in sample_left:
        for rb in sample_right:
            p = _intersect(la, rb)
            if p is None:
                continue
            px, py = p
            if -0.25 * w <= px <= 1.25 * w and -0.35 * h <= py <= 0.90 * h:
                intersections.append((px, py))

    if intersections:
        xs = sorted(p[0] for p in intersections)
        ys = sorted(p[1] for p in intersections)
        vp = (xs[len(xs) // 2], ys[len(ys) // 2])

    if vp is not None:
        vx, vy = vp
        out["vanishing_point_px"] = [round(vx, 3), round(vy, 3)]
        out["vanishing_point_strength"] = round(_clip01(len(intersections) / 20.0), 6)

        dx_norm = (vx - (w * 0.5)) / max(1.0, (w * 0.5))
        up_ratio = 1.0 - (vy / max(1.0, float(h)))
        out["road_axis_dx_norm"] = round(float(dx_norm), 6)
        out["road_axis_up_ratio"] = round(_clip01(up_ratio), 6)

        dx = vx - (w * 0.5)
        dy = float(h) - vy
        if abs(dx) > 1e-9 or abs(dy) > 1e-9:
            out["road_axis_angle_img_deg"] = round(math.degrees(math.atan2(dx, dy)), 3)

    y_bottom = float(h - 1)
    xl = _x_at_y(left_best, y_bottom) if left_best is not None else None
    xr = _x_at_y(right_best, y_bottom) if right_best is not None else None
    if xl is not None and xr is not None and xr > xl and w > 0:
        out["road_corridor_bottom_width_ratio"] = round((xr - xl) / float(w), 6)

    has_two_sides = left_best is not None and right_best is not None
    has_vp = vp is not None
    has_lane_like = has_two_sides and has_vp

    scene_score = 0.0
    if has_two_sides:
        scene_score += 0.35
    if has_vp:
        scene_score += 0.30
    if vehicle_points:
        scene_score += 0.15
    scene_score += min(0.10, out["edge_density_bottom_half"] * 2.0)
    scene_score += min(0.10, out["vanishing_point_strength"])

    out["quality"] = {
        "scene_score": round(_clip01(scene_score), 6),
        "has_two_sides": bool(has_two_sides),
        "has_vanishing_point": bool(has_vp),
        "has_lane_like_geometry": bool(has_lane_like),
    }

    out["status"] = "ok"
    return out