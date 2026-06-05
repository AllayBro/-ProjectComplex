# -*- coding: utf-8 -*-
"""
objects_detect.py
Один вход: run(image_path, out_dir, cfg, device_mode) -> dict (ModuleResult)
"""

import math
import os
from importlib import import_module
from time import perf_counter
from pathlib import Path

import cv2
import numpy as np
from PIL import Image, ImageOps, ExifTags
from ultralytics import YOLO

try:
    import pyheif
except Exception:
    pyheif = None


FOCAL_MM_DEFAULT = 4.25
SENSOR_WIDTH_MM_DEFAULT = 5.6

YOLO_IMGSZ_DEFAULT = 1280
YOLO_CONF_GLOBAL_DEFAULT = 0.15

SCALE_TINY_MAX_DEFAULT = 0.02
SCALE_SMALL_MAX_DEFAULT = 0.05

MIN_BOX_TINY_DEFAULT = 10
MIN_BOX_SMALL_DEFAULT = 20
MIN_BOX_NORMAL_DEFAULT = 30

CONF_TINY_DEFAULT = 0.18
CONF_SMALL_DEFAULT = 0.22
CONF_NORMAL_DEFAULT = 0.28

VEHICLE_CLASSES_DEFAULT = {"car", "truck", "bus", "motorcycle"}

VEHICLE_DIMENSIONS_DEFAULT = {
    "car": (1.8, 1.5, 4.5),
    "truck": (2.5, 3.0, 9.0),
    "bus": (2.5, 3.2, 12.0),
    "motorcycle": (0.8, 1.2, 2.1),
    "default": (1.85, 1.5, 4.5),
}

def _vehicle_color_bgr(cls_name):
    cls = str(cls_name or "").strip().lower()
    if cls == "truck":
        return (0, 0, 255)
    if cls == "bus":
        return (255, 0, 0)
    if cls == "motorcycle":
        return (0, 0, 0)
    return (0, 255, 0)


def _draw_box_and_label(img, x1, y1, x2, y2, label, color):
    # Единый масштаб “рамка/шрифт” по размеру исходного изображения.
    # Это нужно, чтобы подписи читались одинаково при разных разрешениях.
    H, W = img.shape[:2]
    scale = max(1.0, float(max(H, W)) / 1600.0)

    font = cv2.FONT_HERSHEY_SIMPLEX
    font_scale = 0.8 * scale
    box_thickness = max(2, int(round(2.0 * scale)))
    text_thickness = max(2, int(round(2.0 * scale)))
    outline_thickness = max(4, int(round(5.0 * scale)))
    pad_x = max(8, int(round(8.0 * scale)))
    pad_y = max(6, int(round(6.0 * scale)))
    top_gap = max(10, int(round(10.0 * scale)))

    x1 = int(x1)
    y1 = int(y1)
    x2 = int(x2)
    y2 = int(y2)

    cv2.rectangle(img, (x1, y1), (x2, y2), color, box_thickness)

    (tw, th), baseline = cv2.getTextSize(label, font, font_scale, text_thickness)
    tx = max(0, x1)
    ty = max(th + pad_y, y1 - top_gap)

    bg_x1 = tx
    bg_y1 = max(0, ty - th - pad_y)
    bg_x2 = min(W - 1, tx + tw + pad_x * 2)
    bg_y2 = min(H - 1, ty + baseline + pad_y)

    cv2.rectangle(img, (bg_x1, bg_y1), (bg_x2, bg_y2), (0, 0, 0), -1)
    cv2.putText(img, label, (tx + pad_x, ty), font, font_scale, (0, 0, 0), outline_thickness, cv2.LINE_AA)
    cv2.putText(img, label, (tx + pad_x, ty), font, font_scale, (255, 255, 255), text_thickness, cv2.LINE_AA)

def _normalized_vehicle_class(cls_name):
    cls = str(cls_name or "").strip().lower()
    if cls == "van":
        return "car"
    return cls


def _vehicle_color_bgr(cls_name):
    cls = _normalized_vehicle_class(cls_name)
    if cls == "truck":
        return (0, 0, 255)
    if cls == "bus":
        return (255, 0, 0)
    if cls == "motorcycle":
        return (0, 0, 0)
    return (0, 255, 0)


def _draw_box_and_label(img, x1, y1, x2, y2, label, color):
    # Дублирующаяся функция в файле: сделаем ее такой же, как и первая.
    # (В Python более поздняя версия переопределяет предыдущую, но так меньше шансов
    # получить “разный стиль” при рефакторинге.)
    H, W = img.shape[:2]
    scale = max(1.0, float(max(H, W)) / 1600.0)

    font = cv2.FONT_HERSHEY_SIMPLEX
    font_scale = 0.8 * scale
    box_thickness = max(2, int(round(2.0 * scale)))
    text_thickness = max(2, int(round(2.0 * scale)))
    outline_thickness = max(4, int(round(5.0 * scale)))
    pad_x = max(8, int(round(8.0 * scale)))
    pad_y = max(6, int(round(6.0 * scale)))
    top_gap = max(10, int(round(10.0 * scale)))

    x1 = int(x1)
    y1 = int(y1)
    x2 = int(x2)
    y2 = int(y2)

    cv2.rectangle(img, (x1, y1), (x2, y2), color, box_thickness)

    (tw, th), baseline = cv2.getTextSize(label, font, font_scale, text_thickness)
    tx = max(0, x1)
    ty = max(th + pad_y, y1 - top_gap)

    bg_x1 = tx
    bg_y1 = max(0, ty - th - pad_y)
    bg_x2 = min(W - 1, tx + tw + pad_x * 2)
    bg_y2 = min(H - 1, ty + baseline + pad_y)

    cv2.rectangle(img, (bg_x1, bg_y1), (bg_x2, bg_y2), (0, 0, 0), -1)
    cv2.putText(img, label, (tx + pad_x, ty), font, font_scale, (0, 0, 0), outline_thickness, cv2.LINE_AA)
    cv2.putText(img, label, (tx + pad_x, ty), font, font_scale, (255, 255, 255), text_thickness, cv2.LINE_AA)


def _rat_to_float(v):
    if v is None:
        return None
    try:
        if isinstance(v, tuple) and len(v) == 2 and float(v[1]) != 0.0:
            return float(v[0]) / float(v[1])
        return float(v)
    except Exception:
        return None


def _exif_get(exif_obj, tag_name: str):
    if exif_obj is None:
        return None
    tag_id = None
    for k, name in ExifTags.TAGS.items():
        if name == tag_name:
            tag_id = k
            break
    if tag_id is None:
        return None
    try:
        return exif_obj.get(tag_id, None)
    except Exception:
        return None


def _focal_plane_unit_mm(unit_val):
    u = _rat_to_float(unit_val)
    if u is None:
        return None
    ui = int(u)
    if ui == 2:
        return 25.4
    if ui == 3:
        return 10.0
    return None


def _estimate_brightness_bgr(img_bgr):
    gray = cv2.cvtColor(img_bgr, cv2.COLOR_BGR2GRAY)
    return round(float(np.mean(gray)), 2)


def _load_image_pil_any(image_path: str):
    p = Path(image_path)
    suf = p.suffix.lower()

    if suf == ".heic":
        if pyheif is not None:
            heif = pyheif.read(image_path)
            img_pil_raw = Image.frombytes(heif.mode, heif.size, heif.data, "raw", heif.mode)
            return img_pil_raw
        try:
            import pillow_heif
            pillow_heif.register_heif_opener()
            return Image.open(image_path)
        except Exception as e:
            raise RuntimeError("HEIC не прочитан: нет pyheif и не удалось использовать pillow-heif") from e

    return Image.open(image_path)


def _select_device(device_mode: str):
    mode = str(device_mode or "auto").lower().strip()
    try:
        import torch
        has_cuda = bool(torch.cuda.is_available())
    except Exception:
        has_cuda = False

    if mode in {"gpu", "cuda"}:
        if has_cuda:
            return "gpu", 0
        return "cpu", "cpu"

    if mode == "cpu":
        return "cpu", "cpu"

    if has_cuda:
        return "gpu", 0
    return "cpu", "cpu"


def _get_model_cached(model_path: str):
    cache = globals().setdefault("_YOLO_MODEL_CACHE", {})
    m = cache.get(model_path)
    if m is None:
        m = YOLO(model_path)
        cache[model_path] = m
    return m


_CLUSTER_MODULES = {
    1: "notebooks.vehicle_detect",
    2: "notebooks.rotation_detect",
    3: "notebooks.claster_detect",
}


def _use_full_distance(cfg: dict) -> bool:
    if not isinstance(cfg, dict):
        return False
    nb = (cfg.get("notebooks", {}) or {}).get("objects_detect", {}) or {}
    dist_cfg = cfg.get("distance", {}) or {}
    return bool(
        nb.get("use_full_distance")
        or dist_cfg.get("use_full_distance")
        or cfg.get("use_full_distance", False)
    )


def _pipeline_order(cfg: dict) -> list:
    order = cfg.get("full_pipeline_order", [1, 2, 3, 4]) if isinstance(cfg, dict) else [1, 2, 3, 4]
    out = []
    for cid in order:
        try:
            cid_i = int(cid)
        except Exception:
            continue
        if cid_i in _CLUSTER_MODULES and cid_i not in out:
            out.append(cid_i)
    return out or [1, 2, 3]


def _iou_xyxy(a, b) -> float:
    ax1, ay1, ax2, ay2 = map(float, a[:4])
    bx1, by1, bx2, by2 = map(float, b[:4])
    ix1, iy1 = max(ax1, bx1), max(ay1, by1)
    ix2, iy2 = min(ax2, bx2), min(ay2, by2)
    iw, ih = max(0.0, ix2 - ix1), max(0.0, iy2 - iy1)
    inter = iw * ih
    if inter <= 0.0:
        return 0.0
    area_a = max(0.0, ax2 - ax1) * max(0.0, ay2 - ay1)
    area_b = max(0.0, bx2 - bx1) * max(0.0, by2 - by1)
    denom = area_a + area_b - inter
    return float(inter / denom) if denom > 1e-9 else 0.0


def _best_match_meta(bbox_xyxy, candidates, min_iou=0.20):
    best = None
    best_iou = 0.0
    for item in candidates or []:
        bb = item.get("bbox_xyxy") or item.get("bbox")
        if not bb or len(bb) != 4:
            continue
        iou = _iou_xyxy(bbox_xyxy, bb)
        if iou > best_iou:
            best_iou = iou
            best = item
    if best is None or best_iou < float(min_iou):
        return None, best_iou
    return best.get("meta", {}) or {}, best_iou


def _rotated_frame_dims(W: float, H: float, r_img_deg: float):
    r = math.radians(float(r_img_deg))
    wp = abs(W * math.cos(r)) + abs(H * math.sin(r))
    hp = abs(W * math.sin(r)) + abs(H * math.cos(r))
    return float(wp), float(hp)


def _euler_zyx_deg_to_R(yaw_deg, pitch_deg, roll_deg):
    yaw = math.radians(float(yaw_deg))
    pitch = math.radians(float(pitch_deg))
    roll = math.radians(float(roll_deg))

    cz, sz = math.cos(yaw), math.sin(yaw)
    cy, sy = math.cos(pitch), math.sin(pitch)
    cx, sx = math.cos(roll), math.sin(roll)

    return np.array(
        [
            [cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx],
            [sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx],
            [-sy, cy * sx, cy * cx],
        ],
        dtype=np.float64,
    )


def _safe_float(v):
    if v is None:
        return None
    try:
        fv = float(v)
    except Exception:
        return None
    if not np.isfinite(fv):
        return None
    return fv


def _focal_px_from_vps(xi1, eta1, xi2, eta2, wp, hp):
    term = -((float(xi1) - wp / 2.0) * (float(xi2) - wp / 2.0) + (float(eta1) - hp / 2.0) * (float(eta2) - hp / 2.0))
    if term <= 1e-6:
        return None
    return math.sqrt(term)


def _monocular_distance_m(focal_px_y, real_h, h_px):
    if focal_px_y is None or real_h is None or h_px <= 0:
        return None
    try:
        return round((float(focal_px_y) * float(real_h)) / float(h_px), 2)
    except Exception:
        return None


def _full_distance_m(f_px, wp, hp, w_px, h_px, r12, r23):
    f = _safe_float(f_px)
    if f is None or f <= 1e-6:
        return None, None, None

    w_px = max(1.0, float(w_px))
    h_px = max(1.0, float(h_px))
    wp = max(1.0, float(wp))
    hp = max(1.0, float(hp))

    r12a = abs(float(r12)) if r12 is not None else 0.0
    r23a = abs(float(r23)) if r23 is not None else 0.0

    if r12a < 1e-9 and r23a < 1e-9:
        return None, None, None

    d_w = (f * hp * r12a) / h_px if r12a > 1e-9 else None
    d_h = (f * wp * r23a) / w_px if r23a > 1e-9 else None

    if d_w is None and d_h is None:
        return None, d_w, d_h

    if d_w is None:
        return round(float(d_h), 2), d_w, d_h
    if d_h is None:
        return round(float(d_w), 2), d_w, d_h

    denom = r12a + r23a
    if denom <= 1e-9:
        return None, d_w, d_h

    dist = (float(d_w) * r12a + float(d_h) * r23a) / denom
    return round(dist, 2), d_w, d_h


def _rotation_matrix_elements(rot_meta: dict):
    yaw = _safe_float(rot_meta.get("Rot-Z"))
    if yaw is None:
        yaw = _safe_float(rot_meta.get("X-pos"))
    pitch = _safe_float(rot_meta.get("Rot-X"))
    if pitch is None:
        pitch = _safe_float(rot_meta.get("Y-pos"))
    roll = _safe_float(rot_meta.get("Rot-Y"))
    if roll is None:
        roll = _safe_float(rot_meta.get("Z-pos"))

    if yaw is None and pitch is None and roll is None:
        return None, None

    yaw = 0.0 if yaw is None else yaw
    pitch = 0.0 if pitch is None else pitch
    roll = 0.0 if roll is None else roll

    R = _euler_zyx_deg_to_R(yaw, pitch, roll)
    return float(R[0, 1]), float(R[1, 2])


def _run_upstream_clusters(image_path: str, out_dir: str, cfg: dict, device_mode: str, order: list):
    results = {}
    for cid in order:
        mod_name = _CLUSTER_MODULES.get(int(cid))
        if not mod_name:
            continue
        sub_out = os.path.join(out_dir, f"upstream_cluster_{cid}")
        os.makedirs(sub_out, exist_ok=True)
        mod = import_module(mod_name)
        results[int(cid)] = mod.run(image_path, sub_out, cfg, device_mode)
    return results


def _primary_detections(upstream: dict):
    for cid in (3, 1):
        res = upstream.get(cid) or {}
        dets = res.get("detections") or []
        if dets:
            return dets, cid
    return [], None


def _rotation_candidates(upstream: dict):
    res = upstream.get(2) or {}
    return list(res.get("detections") or [])


def _run_full_distance(image_path: str, out_dir: str, cfg: dict, device_mode: str = "auto") -> dict:
    import csv
    import json

    t_all0 = perf_counter()
    timings_ms = {}
    warnings = []

    module_id = str(cfg.get("module_id", "objects_detect"))
    nb = (cfg.get("notebooks", {}) or {}).get("objects_detect", {}) or {}
    dist_cfg = cfg.get("distance", {}) or {}

    vehicle_dimensions = cfg.get("vehicle_dimensions", None)
    if vehicle_dimensions is None:
        vehicle_dimensions = VEHICLE_DIMENSIONS_DEFAULT

    focal_mm_default = float(cfg.get("focal_mm_default", dist_cfg.get("focal_mm", FOCAL_MM_DEFAULT)))
    sensor_width_mm = float(cfg.get("sensor_width_mm", dist_cfg.get("sensor_width_mm", SENSOR_WIDTH_MM_DEFAULT)))

    save_annotated = bool(cfg.get("save_annotated", True))
    write_csv = bool(cfg.get("write_csv", True))
    write_exif_json = bool(cfg.get("write_exif_json", True))
    annotated_name = str(cfg.get("annotated_name", ""))
    csv_name = str(cfg.get("csv_name", "detections.csv"))
    jpeg_quality = int(cfg.get("jpeg_quality", 95))

    os.makedirs(out_dir, exist_ok=True)
    stem = Path(image_path).stem

    t0 = perf_counter()
    upstream = _run_upstream_clusters(image_path, out_dir, cfg, device_mode, _pipeline_order(cfg))
    timings_ms["upstream_clusters"] = int((perf_counter() - t0) * 1000)

    t0 = perf_counter()
    exif_dict = {}
    exif_raw = None
    focal_mm = focal_mm_default

    img_pil_raw = _load_image_pil_any(image_path)
    try:
        exif_raw = img_pil_raw.getexif()
    except Exception:
        exif_raw = None

    if exif_raw is not None:
        try:
            for tag_id, value in exif_raw.items():
                tag_name = ExifTags.TAGS.get(tag_id, str(tag_id))
                exif_dict[str(tag_name)] = str(value)
        except Exception:
            exif_dict = {}

    v_focal = _exif_get(exif_raw, "FocalLength")
    focal_mm_exif = _rat_to_float(v_focal)
    if focal_mm_exif is not None and focal_mm_exif > 0:
        focal_mm = focal_mm_exif

    img_pil = ImageOps.exif_transpose(img_pil_raw).convert("RGB")
    img_bgr = cv2.cvtColor(np.array(img_pil), cv2.COLOR_RGB2BGR)
    H, W = img_bgr.shape[:2]

    brightness = _estimate_brightness_bgr(img_bgr)
    focal_35mm = _rat_to_float(_exif_get(exif_raw, "FocalLengthIn35mmFilm"))
    fp_xres = _rat_to_float(_exif_get(exif_raw, "FocalPlaneXResolution"))
    fp_yres = _rat_to_float(_exif_get(exif_raw, "FocalPlaneYResolution"))
    fp_unit = _focal_plane_unit_mm(_exif_get(exif_raw, "FocalPlaneResolutionUnit"))

    focal_px_x = None
    focal_px_y = None
    focal_px_method = ""

    if fp_unit is not None and focal_mm is not None and focal_mm > 0 and (fp_xres is not None or fp_yres is not None):
        if fp_xres is not None and fp_xres > 0:
            focal_px_x = focal_mm * (fp_xres / fp_unit)
        if fp_yres is not None and fp_yres > 0:
            focal_px_y = focal_mm * (fp_yres / fp_unit)
        focal_px_method = "FocalPlaneResolution"
    elif focal_35mm is not None and focal_35mm > 0:
        focal_px_x = (focal_35mm / 36.0) * float(W)
        focal_px_y = (focal_35mm / 24.0) * float(H)
        focal_px_method = "FocalLengthIn35mmFilm"
    else:
        sw = float(sensor_width_mm)
        if sw > 0 and focal_mm > 0:
            focal_px_x = (float(focal_mm) / sw) * float(W)
            sh = sw * (float(H) / float(W) if W > 0 else 1.0)
            focal_px_y = (float(focal_mm) / sh) * float(H) if sh > 0 else None
            focal_px_method = "FallbackSensorWidth"

    timings_ms["load_image"] = int((perf_counter() - t0) * 1000)

    primary_dets, primary_cluster = _primary_detections(upstream)
    rot_candidates = _rotation_candidates(upstream)

    if not primary_dets:
        warnings.append("full_distance_no_upstream_detections")

    annotated = img_bgr.copy()
    detections = []
    match_iou_thr = float(nb.get("match_iou", dist_cfg.get("match_iou", 0.20)))

    for det_idx, det in enumerate(primary_dets, start=1):
        bb = det.get("bbox_xyxy")
        if not bb or len(bb) != 4:
            continue

        x1, y1, x2, y2 = map(int, bb)
        x1 = max(0, min(x1, W - 1))
        y1 = max(0, min(y1, H - 1))
        x2 = max(0, min(x2, W))
        y2 = max(0, min(y2, H))
        w_px = int(x2 - x1)
        h_px = int(y2 - y1)
        if w_px <= 0 or h_px <= 0:
            continue

        cls_name = _normalized_vehicle_class(det.get("cls_name", "car"))
        conf = float(det.get("conf", 0.0))

        rot_meta, match_iou = _best_match_meta(bb, rot_candidates, min_iou=match_iou_thr)
        if rot_meta is None:
            rot_meta = {}
            warnings.append(f"full_distance_no_rotation_match_det_{det_idx}")

        r_img_deg = _safe_float(rot_meta.get("R-img"))
        if r_img_deg is None:
            r_img_deg = 0.0

        wp, hp = _rotated_frame_dims(float(W), float(H), r_img_deg)
        r12, r23 = _rotation_matrix_elements(rot_meta)

        f_use = focal_px_y if focal_px_y is not None else focal_px_x
        if f_use is None:
            f_use = 0.9 * float(max(W, H))

        dist_m = None
        d_w = None
        d_h = None
        distance_method = "full_euler"

        if r12 is not None or r23 is not None:
            dist_m, d_w, d_h = _full_distance_m(f_use, wp, hp, w_px, h_px, r12 or 0.0, r23 or 0.0)

        if dist_m is None:
            dims = vehicle_dimensions.get(cls_name, vehicle_dimensions.get("default", (None, None, None)))
            real_h = float(dims[1]) if dims and dims[1] is not None else None
            dist_m = _monocular_distance_m(f_use, real_h, h_px)
            distance_method = "full_euler_fallback_monocular"
            if dist_m is None:
                distance_method = "failed"

        color = _vehicle_color_bgr(cls_name)
        label = f"{cls_name} {det_idx}"
        if dist_m is not None:
            label += f" ~{dist_m}m"
        _draw_box_and_label(annotated, x1, y1, x2, y2, label, color)

        meta = {
            "w_px": int(w_px),
            "h_px": int(h_px),
            "rel_h": float(h_px) / float(H if H > 0 else 1),
            "dist_m": dist_m,
            "distance_method": distance_method,
            "primary_cluster": primary_cluster,
            "rotation_match_iou": round(float(match_iou), 4),
            "R-img": round(float(r_img_deg), 3),
            "Rot-X": rot_meta.get("Rot-X"),
            "Rot-Y": rot_meta.get("Rot-Y"),
            "Rot-Z": rot_meta.get("Rot-Z"),
            "r12": None if r12 is None else round(float(r12), 6),
            "r23": None if r23 is None else round(float(r23), 6),
            "W_prime": round(float(wp), 3),
            "H_prime": round(float(hp), 3),
            "d_w": None if d_w is None else round(float(d_w), 3),
            "d_h": None if d_h is None else round(float(d_h), 3),
            "focal_px_used": round(float(f_use), 3) if f_use is not None else None,
        }

        detections.append(
            {
                "bbox_xyxy": [int(x1), int(y1), int(x2), int(y2)],
                "conf": float(round(conf, 6)),
                "cls_id": int(det.get("cls_id", -1)),
                "cls_name": str(cls_name),
                "meta": meta,
            }
        )

    if annotated_name.strip():
        annotated_path = os.path.join(out_dir, annotated_name)
    else:
        annotated_path = os.path.join(out_dir, f"annotated_{stem}.jpg")

    try:
        if save_annotated:
            cv2.imwrite(annotated_path, annotated, [int(cv2.IMWRITE_JPEG_QUALITY), int(jpeg_quality)])
        else:
            annotated_path = ""
    except Exception:
        warnings.append("Не удалось сохранить annotated_image")
        annotated_path = ""

    csv_path = os.path.join(out_dir, csv_name)
    if write_csv:
        try:
            file_exists = os.path.exists(csv_path)
            with open(csv_path, "a", newline="", encoding="utf-8") as f:
                w = csv.writer(f)
                if not file_exists:
                    w.writerow(
                        [
                            "Image",
                            "ID",
                            "Class",
                            "x1",
                            "y1",
                            "x2",
                            "y2",
                            "Width",
                            "Height",
                            "Confidence",
                            "Distance(m)",
                            "DistanceMethod",
                            "Rot-X",
                            "Rot-Y",
                            "Rot-Z",
                            "R-img",
                            "r12",
                            "r23",
                            "W_prime",
                            "H_prime",
                            "d_w",
                            "d_h",
                            "Brightness",
                            "FocalMM",
                            "FocalPxY",
                            "FocalPxMethod",
                        ]
                    )
                for i, d in enumerate(detections, start=1):
                    x1, y1, x2, y2 = d["bbox_xyxy"]
                    m = d.get("meta", {}) or {}
                    w.writerow(
                        [
                            os.path.basename(image_path),
                            i,
                            d["cls_name"],
                            x1,
                            y1,
                            x2,
                            y2,
                            (x2 - x1),
                            (y2 - y1),
                            round(float(d["conf"]), 6),
                            m.get("dist_m", None),
                            m.get("distance_method", ""),
                            m.get("Rot-X", ""),
                            m.get("Rot-Y", ""),
                            m.get("Rot-Z", ""),
                            m.get("R-img", ""),
                            m.get("r12", ""),
                            m.get("r23", ""),
                            m.get("W_prime", ""),
                            m.get("H_prime", ""),
                            m.get("d_w", ""),
                            m.get("d_h", ""),
                            brightness,
                            focal_mm,
                            focal_px_y,
                            focal_px_method,
                        ]
                    )
        except Exception:
            warnings.append("Не удалось записать CSV detections.csv")

    exif_json_path = os.path.join(out_dir, f"exif_{stem}.json")
    if write_exif_json:
        try:
            payload = {
                "image": os.path.basename(image_path),
                "focal_mm": focal_mm,
                "focal_35mm": focal_35mm,
                "sensor_width_mm": sensor_width_mm,
                "focal_px_x": focal_px_x,
                "focal_px_y": focal_px_y,
                "focal_px_method": focal_px_method,
                "brightness": brightness,
                "distance_mode": "full_euler",
                "upstream_clusters": sorted(list(upstream.keys())),
                "exif": exif_dict,
            }
            with open(exif_json_path, "w", encoding="utf-8") as f:
                json.dump(payload, f, ensure_ascii=False, indent=2)
        except Exception:
            warnings.append("Не удалось записать EXIF json")
            exif_json_path = ""

    timings_ms["total"] = int((perf_counter() - t_all0) * 1000)

    return {
        "module_id": module_id,
        "image_w": int(W),
        "image_h": int(H),
        "device_used": str((upstream.get(2) or {}).get("device_used", "cpu")),
        "warnings": warnings,
        "annotated_image_path": annotated_path,
        "cleaned_image_path": "",
        "artifacts": {
            "csv_path": csv_path if write_csv else "",
            "exif_json_path": exif_json_path,
            "brightness": brightness,
            "focal_mm": focal_mm,
            "focal_35mm": focal_35mm,
            "sensor_width_mm": sensor_width_mm,
            "focal_px_x": focal_px_x,
            "focal_px_y": focal_px_y,
            "focal_px_method": focal_px_method,
            "distance_mode": "full_euler",
            "upstream_clusters": {str(k): v.get("module_id", str(k)) for k, v in upstream.items()},
        },
        "timings_ms": timings_ms,
        "detections": detections,
    }


def run(image_path: str, out_dir: str, cfg: dict, device_mode: str = "auto") -> dict:
    import json
    import csv

    if cfg is None:
        cfg = {}

    if _use_full_distance(cfg):
        return _run_full_distance(image_path, out_dir, cfg, device_mode)

    t_all0 = perf_counter()
    timings_ms = {}
    warnings = []

    module_id = str(cfg.get("module_id", "objects_detect"))

    model_path = str(cfg.get("yolo_model_path") or cfg.get("model_path") or "").strip().strip('"').strip("'")
    if not model_path:
        raise RuntimeError("yolo_model_path не задан. Модель должна выбираться из папки yolo в UI.")

    yolo_dir = str(cfg.get("yolo_dir", "") or "").strip().strip('"').strip("'")
    if yolo_dir and (not os.path.isabs(model_path)):
        model_path = os.path.join(yolo_dir, model_path)

    model_path = os.path.abspath(os.path.expandvars(os.path.expanduser(model_path)))
    if not os.path.exists(model_path):
        raise RuntimeError(f"Файл модели YOLO не найден: {model_path}")

    imgsz = int(cfg.get("yolo_imgsz", cfg.get("imgsz", YOLO_IMGSZ_DEFAULT)))
    conf_global = float(cfg.get("yolo_conf_global", cfg.get("conf_global", YOLO_CONF_GLOBAL_DEFAULT)))

    scale_tiny_max = float(cfg.get("scale_tiny_max", SCALE_TINY_MAX_DEFAULT))
    scale_small_max = float(cfg.get("scale_small_max", SCALE_SMALL_MAX_DEFAULT))

    min_box_tiny = int(cfg.get("min_box_tiny", MIN_BOX_TINY_DEFAULT))
    min_box_small = int(cfg.get("min_box_small", MIN_BOX_SMALL_DEFAULT))
    min_box_normal = int(cfg.get("min_box_normal", MIN_BOX_NORMAL_DEFAULT))

    conf_tiny = float(cfg.get("conf_tiny", CONF_TINY_DEFAULT))
    conf_small = float(cfg.get("conf_small", CONF_SMALL_DEFAULT))
    conf_normal = float(cfg.get("conf_normal", CONF_NORMAL_DEFAULT))

    vehicle_classes = cfg.get("vehicle_classes", None)
    if vehicle_classes is None:
        vehicle_classes = VEHICLE_CLASSES_DEFAULT
    vehicle_classes = set(map(str, vehicle_classes))

    vehicle_dimensions = cfg.get("vehicle_dimensions", None)
    if vehicle_dimensions is None:
        vehicle_dimensions = VEHICLE_DIMENSIONS_DEFAULT

    focal_mm_default = float(cfg.get("focal_mm_default", FOCAL_MM_DEFAULT))
    sensor_width_mm = float(cfg.get("sensor_width_mm", SENSOR_WIDTH_MM_DEFAULT))

    filter_vehicle_only = bool(cfg.get("filter_vehicle_only", True))

    save_annotated = bool(cfg.get("save_annotated", True))
    save_cleaned = bool(cfg.get("save_cleaned", True))
    write_csv = bool(cfg.get("write_csv", True))
    write_exif_json = bool(cfg.get("write_exif_json", True))

    annotated_name = str(cfg.get("annotated_name", ""))
    cleaned_name = str(cfg.get("cleaned_name", ""))
    csv_name = str(cfg.get("csv_name", "detections.csv"))

    jpeg_quality = int(cfg.get("jpeg_quality", 95))
    if jpeg_quality < 1:
        jpeg_quality = 1
    if jpeg_quality > 100:
        jpeg_quality = 100

    t0 = perf_counter()
    os.makedirs(out_dir, exist_ok=True)
    timings_ms["prepare_out_dir"] = int((perf_counter() - t0) * 1000)

    stem = Path(image_path).stem

    t0 = perf_counter()
    device_used, device_arg = _select_device(device_mode)
    if str(device_mode or "auto").lower().strip() in {"gpu", "cuda"} and device_used == "cpu":
        warnings.append("Запрошен gpu, но CUDA недоступна — использую cpu")
    timings_ms["select_device"] = int((perf_counter() - t0) * 1000)

    t0 = perf_counter()
    model = _get_model_cached(model_path)
    timings_ms["load_model"] = int((perf_counter() - t0) * 1000)

    t0 = perf_counter()
    exif_dict = {}
    exif_raw = None
    focal_mm = focal_mm_default

    img_pil_raw = _load_image_pil_any(image_path)

    try:
        exif_raw = img_pil_raw.getexif()
    except Exception:
        exif_raw = None

    if exif_raw is not None:
        try:
            for tag_id, value in exif_raw.items():
                tag_name = ExifTags.TAGS.get(tag_id, str(tag_id))
                exif_dict[str(tag_name)] = str(value)
        except Exception:
            exif_dict = {}

    v_focal = _exif_get(exif_raw, "FocalLength")
    focal_mm_exif = _rat_to_float(v_focal)
    if focal_mm_exif is not None and focal_mm_exif > 0:
        focal_mm = focal_mm_exif
    else:
        focal_mm = focal_mm_default

    img_pil = ImageOps.exif_transpose(img_pil_raw).convert("RGB")
    img_bgr = cv2.cvtColor(np.array(img_pil), cv2.COLOR_RGB2BGR)

    H, W = img_bgr.shape[:2]
    timings_ms["load_image"] = int((perf_counter() - t0) * 1000)

    t0 = perf_counter()
    brightness = None
    try:
        brightness = _estimate_brightness_bgr(img_bgr)
    except Exception:
        brightness = None
        warnings.append("Не удалось посчитать brightness")

    focal_35mm = _rat_to_float(_exif_get(exif_raw, "FocalLengthIn35mmFilm"))

    fp_xres = _rat_to_float(_exif_get(exif_raw, "FocalPlaneXResolution"))
    fp_yres = _rat_to_float(_exif_get(exif_raw, "FocalPlaneYResolution"))
    fp_unit = _focal_plane_unit_mm(_exif_get(exif_raw, "FocalPlaneResolutionUnit"))

    focal_px_x = None
    focal_px_y = None
    focal_px_method = ""

    if fp_unit is not None and focal_mm is not None and focal_mm > 0 and (fp_xres is not None or fp_yres is not None):
        if fp_xres is not None and fp_xres > 0:
            px_per_mm_x = fp_xres / fp_unit
            focal_px_x = focal_mm * px_per_mm_x
        if fp_yres is not None and fp_yres > 0:
            px_per_mm_y = fp_yres / fp_unit
            focal_px_y = focal_mm * px_per_mm_y
        focal_px_method = "FocalPlaneResolution"
    elif focal_35mm is not None and focal_35mm > 0:
        focal_px_x = (focal_35mm / 36.0) * float(W)
        focal_px_y = (focal_35mm / 24.0) * float(H)
        focal_px_method = "FocalLengthIn35mmFilm"
    else:
        try:
            sw = float(sensor_width_mm)
            if sw > 0 and focal_mm is not None and focal_mm > 0:
                focal_px_x = (float(focal_mm) / sw) * float(W)
                sh = sw * (float(H) / float(W) if W > 0 else 1.0)
                focal_px_y = (float(focal_mm) / sh) * float(H) if sh > 0 else None
                focal_px_method = "FallbackSensorWidth"
        except Exception:
            focal_px_x = None
            focal_px_y = None

    if focal_px_x is None and focal_px_y is None:
        warnings.append("EXIF не дал данных для focal_px (fx/fy) — проверьте EXIF или задайте sensor_width_mm")
    elif focal_px_method != "FocalPlaneResolution":
        warnings.append(f"focal_px рассчитан не из FocalPlaneResolution (метод: {focal_px_method})")

    timings_ms["frame_metrics"] = int((perf_counter() - t0) * 1000)

    t0 = perf_counter()
    yolo_res = model(
        img_bgr,
        imgsz=imgsz,
        conf=conf_global,
        verbose=False,
        device=device_arg,
    )[0]
    timings_ms["yolo_infer"] = int((perf_counter() - t0) * 1000)

    t0 = perf_counter()
    names = getattr(yolo_res, "names", None)
    if not names:
        names = getattr(getattr(model, "model", None), "names", {}) or {}

    annotated = img_bgr.copy()
    cleaned = np.zeros_like(img_bgr)

    detections = []
    det_idx = 0

    boxes = getattr(yolo_res, "boxes", None)
    if boxes is None:
        boxes = []

    for box in boxes:
        try:
            conf = box.conf[0]
            conf = float(conf.item()) if hasattr(conf, "item") else float(conf)
        except Exception:
            continue

        try:
            xyxy = box.xyxy[0]
            xyxy = xyxy.tolist() if hasattr(xyxy, "tolist") else list(xyxy)
            x1, y1, x2, y2 = map(int, xyxy)
        except Exception:
            continue

        x1 = max(0, min(x1, W - 1))
        y1 = max(0, min(y1, H - 1))
        x2 = max(0, min(x2, W))
        y2 = max(0, min(y2, H))
        w_px = int(x2 - x1)
        h_px = int(y2 - y1)
        if w_px <= 0 or h_px <= 0:
            continue

        rel_h = float(h_px) / float(H if H > 0 else 1)

        if rel_h <= scale_tiny_max:
            min_box_side = min_box_tiny
            conf_required = conf_tiny
        elif rel_h <= scale_small_max:
            min_box_side = min_box_small
            conf_required = conf_small
        else:
            min_box_side = min_box_normal
            conf_required = conf_normal

        if w_px < min_box_side or h_px < min_box_side:
            continue
        if conf < conf_required:
            continue

        try:
            cls_id = box.cls[0]
            cls_id = int(cls_id.item()) if hasattr(cls_id, "item") else int(cls_id)
        except Exception:
            cls_id = -1

        cls_name = _normalized_vehicle_class(names.get(cls_id, "default"))

        if filter_vehicle_only and (cls_name not in vehicle_classes):
            continue

        det_idx += 1

        dist_m = None
        if focal_px_y is not None and (cls_name in vehicle_classes):
            try:
                dims = vehicle_dimensions.get(cls_name, vehicle_dimensions.get("default", (None, None, None)))
                real_h = float(dims[1]) if dims and dims[1] is not None else None
                dist_m = _monocular_distance_m(focal_px_y, real_h, h_px)
            except Exception:
                dist_m = None

        color = _vehicle_color_bgr(cls_name)
        label = f"{cls_name} {det_idx}"
        if dist_m is not None:
            label += f" ~{dist_m}m"
        _draw_box_and_label(annotated, x1, y1, x2, y2, label, color)
        cleaned[y1:y2, x1:x2] = img_bgr[y1:y2, x1:x2]

        detections.append(
            {
                "bbox_xyxy": [int(x1), int(y1), int(x2), int(y2)],
                "conf": float(round(conf, 6)),
                "cls_id": int(cls_id),
                "cls_name": str(cls_name),
                "meta": {
                    "w_px": int(w_px),
                    "h_px": int(h_px),
                    "rel_h": float(rel_h),
                    "dist_m": dist_m,
                    "distance_method": "monocular" if dist_m is not None else "failed",
                },
            }
        )

    timings_ms["postprocess"] = int((perf_counter() - t0) * 1000)

    t0 = perf_counter()

    if annotated_name.strip():
        annotated_path = os.path.join(out_dir, annotated_name)
    else:
        annotated_path = os.path.join(out_dir, f"annotated_{stem}.jpg")

    if cleaned_name.strip():
        cleaned_path = os.path.join(out_dir, cleaned_name)
    else:
        cleaned_path = os.path.join(out_dir, f"cleaned_{stem}.jpg")

    try:
        if save_annotated:
            cv2.imwrite(annotated_path, annotated, [int(cv2.IMWRITE_JPEG_QUALITY), int(jpeg_quality)])
        else:
            annotated_path = ""
    except Exception:
        warnings.append("Не удалось сохранить annotated_image")
        annotated_path = ""

    try:
        if save_cleaned:
            cv2.imwrite(cleaned_path, cleaned, [int(cv2.IMWRITE_JPEG_QUALITY), int(jpeg_quality)])
        else:
            cleaned_path = ""
    except Exception:
        warnings.append("Не удалось сохранить cleaned_image")
        cleaned_path = ""

    csv_path = os.path.join(out_dir, csv_name)
    if write_csv:
        try:
            file_exists = os.path.exists(csv_path)
            with open(csv_path, "a", newline="", encoding="utf-8") as f:
                w = csv.writer(f)
                if not file_exists:
                    w.writerow(
                        [
                            "Image",
                            "ID",
                            "Class",
                            "x1",
                            "y1",
                            "x2",
                            "y2",
                            "Width",
                            "Height",
                            "Confidence",
                            "Distance(m)",
                            "DistanceMethod",
                            "Brightness",
                            "FocalMM",
                            "FocalPxY",
                            "FocalPxMethod",
                        ]
                    )
                for i, d in enumerate(detections, start=1):
                    x1, y1, x2, y2 = d["bbox_xyxy"]
                    w.writerow(
                        [
                            os.path.basename(image_path),
                            i,
                            d["cls_name"],
                            x1,
                            y1,
                            x2,
                            y2,
                            (x2 - x1),
                            (y2 - y1),
                            round(float(d["conf"]), 6),
                            d["meta"].get("dist_m", None),
                            d["meta"].get("distance_method", "monocular"),
                            brightness,
                            focal_mm,
                            focal_px_y,
                            focal_px_method,
                        ]
                    )
        except Exception:
            warnings.append("Не удалось записать CSV detections.csv")

    exif_json_path = os.path.join(out_dir, f"exif_{stem}.json")
    if write_exif_json:
        try:
            payload = {
                "image": os.path.basename(image_path),
                "focal_mm": focal_mm,
                "focal_35mm": focal_35mm,
                "sensor_width_mm": sensor_width_mm,
                "focal_px_x": focal_px_x,
                "focal_px_y": focal_px_y,
                "focal_px_method": focal_px_method,
                "brightness": brightness,
                "exif": exif_dict,
            }
            with open(exif_json_path, "w", encoding="utf-8") as f:
                json.dump(payload, f, ensure_ascii=False, indent=2)
        except Exception:
            warnings.append("Не удалось записать EXIF json")
            exif_json_path = ""

    timings_ms["save_artifacts"] = int((perf_counter() - t0) * 1000)
    timings_ms["total"] = int((perf_counter() - t_all0) * 1000)

    return {
        "module_id": module_id,
        "image_w": int(W),
        "image_h": int(H),
        "device_used": device_used,
        "warnings": warnings,
        "annotated_image_path": annotated_path,
        "cleaned_image_path": cleaned_path,
        "artifacts": {
            "csv_path": csv_path if write_csv else "",
            "exif_json_path": exif_json_path,
            "brightness": brightness,
            "focal_mm": focal_mm,
            "focal_35mm": focal_35mm,
            "sensor_width_mm": sensor_width_mm,
            "focal_px_x": focal_px_x,
            "focal_px_y": focal_px_y,
            "focal_px_method": focal_px_method,
            "distance_mode": "monocular",
        },
        "timings_ms": timings_ms,
        "detections": detections,
    }