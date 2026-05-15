# -*- coding: utf-8 -*-
"""Rotation_detect.ipynb"""

import math
import json
import os
import time
from pathlib import Path
from io import BytesIO

import cv2
import numpy as np

try:
    import torch
except Exception:
    torch = None

try:
    import pandas as pd
except Exception:
    pd = None

try:
    import matplotlib.pyplot as plt
except Exception:
    plt = None

from ultralytics import YOLO

try:
    from google.colab import files
except Exception:
    files = None

from PIL import Image

try:
    import pillow_heif
    pillow_heif.register_heif_opener()
except Exception:
    pillow_heif = None

CAR_CLASSES = {2, 3, 5, 7}
ROT_KEEP_IDS = None

CLS_ID_TO_NAME = {
    2: "car",
    3: "motorcycle",
    5: "bus",
    7: "truck",
}


def _round_or_none(v, nd=3):
    if v is None:
        return None
    try:
        fv = float(v)
    except Exception:
        return None
    if not np.isfinite(fv):
        return None
    return round(fv, nd)


def _safe_float(v):
    try:
        fv = float(v)
    except Exception:
        return None
    if not np.isfinite(fv):
        return None
    return fv


def _normalize_rotation_deg(a):
    a = float(a)
    a = ((a + 180.0) % 360.0) - 180.0
    if abs(a + 180.0) < 1e-9:
        return 180.0
    return a


def _k90_to_angle_deg(k):
    k = int(k) % 4
    if k == 0:
        return 0.0
    if k == 1:
        return -90.0
    if k == 2:
        return 180.0
    return 90.0


def _angle_deg_to_k90(angle_deg):
    a = _normalize_rotation_deg(angle_deg)
    if abs(a - 0.0) < 1e-6:
        return 0
    if abs(a + 90.0) < 1e-6:
        return 1
    if abs(abs(a) - 180.0) < 1e-6:
        return 2
    if abs(a - 90.0) < 1e-6:
        return 3
    return None


def _k90_affine_matrix(W, H, k):
    k = int(k) % 4
    W = int(W)
    H = int(H)

    if k == 0:
        return np.array([[1.0, 0.0, 0.0],
                         [0.0, 1.0, 0.0]], dtype=np.float32)

    if k == 1:
        return np.array([[0.0, -1.0, float(H - 1)],
                         [1.0,  0.0, 0.0]], dtype=np.float32)

    if k == 2:
        return np.array([[-1.0, 0.0, float(W - 1)],
                         [0.0, -1.0, float(H - 1)]], dtype=np.float32)

    return np.array([[0.0, 1.0, 0.0],
                     [-1.0, 0.0, float(W - 1)]], dtype=np.float32)


def _affine_to_3x3(M2x3):
    M2x3 = np.asarray(M2x3, dtype=np.float32).reshape(2, 3)
    return np.vstack([M2x3, np.array([[0.0, 0.0, 1.0]], dtype=np.float32)])


def _affine_compose(M2, M1):
    A = _affine_to_3x3(M2)
    B = _affine_to_3x3(M1)
    C = A @ B
    return C[:2, :].astype(np.float32)


def _apply_rotation_with_matrix(img_bgr, angle_deg):
    if img_bgr is None or img_bgr.size == 0:
        return img_bgr, np.array([[1.0, 0.0, 0.0], [0.0, 1.0, 0.0]], dtype=np.float32), 0.0

    H, W = img_bgr.shape[:2]
    a = _normalize_rotation_deg(angle_deg)

    if abs(a) < 1e-9:
        return img_bgr, np.array([[1.0, 0.0, 0.0], [0.0, 1.0, 0.0]], dtype=np.float32), 0.0

    k = _angle_deg_to_k90(a)
    if k is not None:
        out = rotate_k90_bgr(img_bgr, k)
        M = _k90_affine_matrix(W, H, k)
        return out, M, a

    out, M, _ = _rotate_bound(img_bgr, a)
    return out, np.asarray(M, dtype=np.float32), a


def _camera_ray_from_pixel(u, v, cx, cy, f):
    fu = _safe_float(f)
    if fu is None or fu <= 1e-9:
        return None

    x = (float(u) - float(cx)) / fu
    y = (float(cy) - float(v)) / fu
    z = 1.0

    n = math.sqrt(x * x + y * y + z * z)
    if (not np.isfinite(n)) or n <= 1e-12:
        return None

    return (x / n, y / n, z / n)


def _bbox_center_xy(bbox_xyxy):
    if bbox_xyxy is None or len(bbox_xyxy) != 4:
        return None, None
    x1, y1, x2, y2 = map(float, bbox_xyxy)
    return 0.5 * (x1 + x2), 0.5 * (y1 + y2)


def _bbox_size_xy(bbox_xyxy):
    if bbox_xyxy is None or len(bbox_xyxy) != 4:
        return None, None
    x1, y1, x2, y2 = map(float, bbox_xyxy)
    return max(0.0, x2 - x1), max(0.0, y2 - y1)

def _extract_metric_depth_from_row(row):
    if not isinstance(row, dict):
        return None

    metric_keys = (
        "depth_m",
        "distance_m",
        "dist_m",
        "range_m",
        "z_m",
        "forward_m",
        "distance"
    )

    for key in metric_keys:
        if key in row:
            fv = _safe_float(row.get(key))
            if fv is not None and fv > 0.0:
                return fv

    return None


def _estimate_metric_focal_px(image_w, image_h, f_vp=None):
    fv = _safe_float(f_vp)
    if fv is not None and fv > 1e-6:
        return fv

    w = max(1.0, float(image_w))
    h = max(1.0, float(image_h))
    return 0.5 * math.sqrt(w * w + h * h)


def _metric_position_from_depth(bbox_xyxy, image_w, image_h, focal_px, depth_m):
    if bbox_xyxy is None or len(bbox_xyxy) != 4:
        return None, None, None

    f = _safe_float(focal_px)
    z = _safe_float(depth_m)

    if f is None or f <= 1e-9:
        return None, None, None
    if z is None or z <= 0.0:
        return None, None, None
    if image_w <= 1 or image_h <= 1:
        return None, None, None

    u, v = _bbox_center_xy(bbox_xyxy)
    if u is None or v is None:
        return None, None, None

    cx = 0.5 * float(image_w)
    cy = 0.5 * float(image_h)

    x_m = ((u - cx) * z) / f
    y_m = ((cy - v) * z) / f

    return float(x_m), float(y_m), float(z)


def _metric_size_from_depth(bbox_xyxy, focal_px, depth_m):
    if bbox_xyxy is None or len(bbox_xyxy) != 4:
        return None, None, None

    f = _safe_float(focal_px)
    z = _safe_float(depth_m)

    if f is None or f <= 1e-9:
        return None, None, None
    if z is None or z <= 0.0:
        return None, None, None

    bw, bh = _bbox_size_xy(bbox_xyxy)
    if bw is None or bh is None:
        return None, None, None

    width_m = z * bw / f
    height_m = z * bh / f

    return None, float(width_m), float(height_m)


def _projection_metrics(bbox_xyxy, image_w, image_h, focal_px):
    f = _safe_float(focal_px)
    if f is None or f <= 1e-9:
        return {
            "ray_x": None,
            "ray_y": None,
            "ray_z": None,
            "u_norm": None,
            "v_norm": None,
            "bbox_w_px": None,
            "bbox_h_px": None,
            "bbox_w_norm": None,
            "bbox_h_norm": None,
            "solid_angle_sr": None
        }

    u, v = _bbox_center_xy(bbox_xyxy)
    bw, bh = _bbox_size_xy(bbox_xyxy)

    if u is None or v is None or bw is None or bh is None:
        return {
            "ray_x": None,
            "ray_y": None,
            "ray_z": None,
            "u_norm": None,
            "v_norm": None,
            "bbox_w_px": None,
            "bbox_h_px": None,
            "bbox_w_norm": None,
            "bbox_h_norm": None,
            "solid_angle_sr": None
        }

    cx = 0.5 * float(image_w)
    cy = 0.5 * float(image_h)

    ray = _camera_ray_from_pixel(u, v, cx, cy, f)
    ray_x = None if ray is None else ray[0]
    ray_y = None if ray is None else ray[1]
    ray_z = None if ray is None else ray[2]

    u_norm = (u - cx) / f
    v_norm = (cy - v) / f
    bbox_w_norm = bw / f
    bbox_h_norm = bh / f

    half_w = 0.5 * bbox_w_norm
    half_h = 0.5 * bbox_h_norm
    solid_angle_sr = 4.0 * math.atan((half_w * half_h) / math.sqrt(1.0 + half_w * half_w + half_h * half_h))

    if not np.isfinite(solid_angle_sr):
        solid_angle_sr = None

    return {
        "ray_x": None if ray_x is None else float(ray_x),
        "ray_y": None if ray_y is None else float(ray_y),
        "ray_z": None if ray_z is None else float(ray_z),
        "u_norm": float(u_norm),
        "v_norm": float(v_norm),
        "bbox_w_px": float(bw),
        "bbox_h_px": float(bh),
        "bbox_w_norm": float(bbox_w_norm),
        "bbox_h_norm": float(bbox_h_norm),
        "solid_angle_sr": None if solid_angle_sr is None else float(solid_angle_sr)
    }


def _build_metric_pose_and_size(row, bbox_xyxy, image_w, image_h, focal_px):
    depth_m = _extract_metric_depth_from_row(row)

    pos_x_m, pos_y_m, pos_z_m = _metric_position_from_depth(
        bbox_xyxy=bbox_xyxy,
        image_w=image_w,
        image_h=image_h,
        focal_px=focal_px,
        depth_m=depth_m
    )

    size_x_m, size_y_m, size_z_m = _metric_size_from_depth(
        bbox_xyxy=bbox_xyxy,
        focal_px=focal_px,
        depth_m=depth_m
    )

    return {
        "depth_m": depth_m,
        "Pos-X": pos_x_m,
        "Pos-Y": pos_y_m,
        "Pos-Z": pos_z_m,
        "Rot-X": size_x_m,
        "Rot-Y": size_y_m,
        "Rot-Z": size_z_m
    }


def _normalized_vehicle_class(cls_name):
    cls = str(cls_name or "").strip().lower()
    if cls == "van":
        return "car"
    return cls


def _cls_name_from_id(cls_id):
    return _normalized_vehicle_class(CLS_ID_TO_NAME.get(int(cls_id), str(int(cls_id))))


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
    font = cv2.FONT_HERSHEY_SIMPLEX
    font_scale = 0.8
    box_thickness = 2
    text_thickness = 2
    outline_thickness = 5
    pad_x = 8
    pad_y = 6

    cv2.rectangle(img, (int(x1), int(y1)), (int(x2), int(y2)), color, box_thickness)

    (tw, th), baseline = cv2.getTextSize(label, font, font_scale, text_thickness)
    tx = max(0, int(x1))
    ty = max(th + pad_y, int(y1) - 10)

    bg_x1 = tx
    bg_y1 = max(0, ty - th - pad_y)
    bg_x2 = min(img.shape[1] - 1, tx + tw + pad_x * 2)
    bg_y2 = min(img.shape[0] - 1, ty + baseline + pad_y)

    cv2.rectangle(img, (bg_x1, bg_y1), (bg_x2, bg_y2), (0, 0, 0), -1)
    cv2.putText(img, label, (tx + pad_x, ty), font, font_scale, (0, 0, 0), outline_thickness, cv2.LINE_AA)
    cv2.putText(img, label, (tx + pad_x, ty), font, font_scale, (255, 255, 255), text_thickness, cv2.LINE_AA)


def _box_cls_id(box):
    if len(box) > 5:
        try:
            return int(box[5])
        except Exception:
            return None
    return None


def _read_exif_orientation_deg_from_bytes(file_bytes: bytes):
    try:
        pil = Image.open(BytesIO(file_bytes))
        exif = pil.getexif()
        if not exif:
            return None
        ori = int(exif.get(274, 1))
    except Exception:
        return None

    mapping = {
        1: 0.0,
        2: 0.0,
        3: 180.0,
        4: 180.0,
        5: 90.0,
        6: -90.0,
        7: -90.0,
        8: 90.0,
    }
    return mapping.get(ori, 0.0)


def read_image_auto_with_meta(file_bytes: bytes):
    exif_orientation_deg = _read_exif_orientation_deg_from_bytes(file_bytes)

    arr = np.frombuffer(file_bytes, np.uint8)
    img = cv2.imdecode(arr, cv2.IMREAD_COLOR)
    if img is not None:
        return img, {"exif_orientation_deg": exif_orientation_deg}

    try:
        pil = Image.open(BytesIO(file_bytes)).convert("RGB")
        img = cv2.cvtColor(np.array(pil), cv2.COLOR_RGB2BGR)
        return img, {"exif_orientation_deg": exif_orientation_deg}
    except Exception as e:
        print("HEIC read error:", repr(e))
        return None, {"exif_orientation_deg": exif_orientation_deg}


def read_image_auto(file_bytes: bytes):
    img, _ = read_image_auto_with_meta(file_bytes)
    return img


def read_image_path_with_meta(image_path: str):
    with open(image_path, "rb") as f:
        data = f.read()
    return read_image_auto_with_meta(data)


def read_image_path(image_path: str):
    img, _ = read_image_path_with_meta(image_path)
    return img


yolo = None
YOLO_DEVICE_ARG = None
def _onnx_tensor_inputs_info(model_path: str):
    try:
        import onnx
    except Exception as e:
        raise RuntimeError(
            "Для ONNX-моделей должны быть заранее установлены пакеты 'onnx' и 'onnxruntime'."
        ) from e

    try:
        model = onnx.load(model_path)
    except Exception as e:
        raise RuntimeError(f"Не удалось прочитать ONNX-модель: {model_path}") from e

    inputs = []
    for inp in model.graph.input:
        tt = inp.type.tensor_type
        dims = []
        for d in tt.shape.dim:
            if getattr(d, "dim_value", 0):
                dims.append(int(d.dim_value))
            elif getattr(d, "dim_param", ""):
                dims.append(str(d.dim_param))
            else:
                dims.append("?")
        inputs.append((str(inp.name), dims))

    return inputs


def _validate_model_path_for_cluster2(model_path: str):
    suffix = Path(model_path).suffix.lower()

    if suffix not in {".pt", ".onnx"}:
        raise RuntimeError(
            f"Неподдерживаемый формат модели для cluster_2: {model_path}. Ожидается .pt или .onnx"
        )

    if suffix != ".onnx":
        return

    inputs = _onnx_tensor_inputs_info(model_path)
    if not inputs:
        raise RuntimeError(f"У ONNX-модели нет входов: {model_path}")

    if len(inputs) != 1:
        desc = ", ".join(f"{name}:{shape}" for name, shape in inputs)
        raise RuntimeError(
            "Текущий cluster_2 поддерживает только YOLO-совместимые ONNX-модели с одним входным "
            f"тензором изображения. У выбранной модели входы: {desc}"
        )

    input_name, shape = inputs[0]
    rank = len(shape)

    if rank != 4:
        raise RuntimeError(
            "Выбрана несовместимая ONNX-модель для текущего YOLO-пайплайна: "
            f"вход '{input_name}' имеет rank={rank} и shape={shape}, "
            "а здесь ожидается YOLO-совместимый 4D вход вида [N, C, H, W]. "
            "Например, FasterRCNN-12.onnx для этого модуля не подходит."
        )


def _resolve_device(device_mode: str, cfg: dict):
    mode = str(device_mode or "auto").lower().strip()
    gpu_id = int((cfg.get("device", {}) or {}).get("gpu_id", 0))

    has_cuda = False
    if torch is not None:
        try:
            has_cuda = bool(torch.cuda.is_available())
        except Exception:
            has_cuda = False

    if mode == "cpu":
        return "cpu", "cpu"

    if mode in {"gpu", "cuda"}:
        if has_cuda:
            return "gpu", str(gpu_id)
        return "cpu", "cpu"

    if has_cuda:
        return "gpu", str(gpu_id)

    return "cpu", "cpu"


def iou_xyxy(a, b):
    ax1, ay1, ax2, ay2 = a
    bx1, by1, bx2, by2 = b
    ix1, iy1 = max(ax1, bx1), max(ay1, by1)
    ix2, iy2 = min(ax2, bx2), min(ay2, by2)
    iw, ih = max(0, ix2 - ix1), max(0, iy2 - iy1)
    inter = iw * ih
    au = max(0, ax2 - ax1) * max(0, ay2 - ay1)
    bu = max(0, bx2 - bx1) * max(0, by2 - by1)
    return inter / (au + bu - inter + 1e-6)


def nms_union(boxes, thr=0.55):
    boxes = sorted(boxes, key=lambda x: x[4], reverse=True)
    kept = []
    for b in boxes:
        drop = False
        for k in kept:
            cls_b = _box_cls_id(b)
            cls_k = _box_cls_id(k)
            if cls_b is not None and cls_k is not None and cls_b != cls_k:
                continue
            if iou_xyxy(b[:4], k[:4]) > thr:
                drop = True
                break
        if not drop:
            kept.append(b)
    return kept


def _vk_expand_xyxy(x1, y1, x2, y2, frac):
    w = max(1.0, float(x2) - float(x1))
    h = max(1.0, float(y2) - float(y1))
    dx = float(frac) * w
    dy = float(frac) * h
    return (float(x1) - dx, float(y1) - dy, float(x2) + dx, float(y2) + dy)


def _vk_interval_gap(a1, a2, b1, b2):
    a1 = float(a1)
    a2 = float(a2)
    b1 = float(b1)
    b2 = float(b2)
    if a2 < b1:
        return b1 - a2
    if b2 < a1:
        return a1 - b2
    return 0.0


def _vk_axis_overlap_ratio(a1, a2, b1, b2):
    a1 = float(a1)
    a2 = float(a2)
    b1 = float(b1)
    b2 = float(b2)
    ov = max(0.0, min(a2, b2) - max(a1, b1))
    denom = max(1e-6, min(a2 - a1, b2 - b1))
    return ov / denom


def _vk_merge_parts_union(
        boxes,
        iou_thr=0.25,
        exp_frac=0.25,
        exp_iou_thr=0.05,
        axis_ov_thr=0.60,
        gap_frac=0.35,
        center_frac=2.0
):
    if not boxes:
        return []

    n = len(boxes)
    parent = list(range(n))

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    def unite(a, b):
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[rb] = ra

    P = []
    for box in boxes:
        x1, y1, x2, y2, c = box[:5]
        tail = tuple(box[5:])
        x1f = float(x1)
        y1f = float(y1)
        x2f = float(x2)
        y2f = float(y2)
        w = max(1.0, x2f - x1f)
        h = max(1.0, y2f - y1f)
        cx = 0.5 * (x1f + x2f)
        cy = 0.5 * (y1f + y2f)
        P.append((x1f, y1f, x2f, y2f, float(c), w, h, cx, cy, tail))

    for i in range(n):
        x1i, y1i, x2i, y2i, ci, wi, hi, cxi, cyi, tail_i = P[i]
        cls_i = tail_i[0] if len(tail_i) > 0 else None

        for j in range(i + 1, n):
            x1j, y1j, x2j, y2j, cj, wj, hj, cxj, cyj, tail_j = P[j]
            cls_j = tail_j[0] if len(tail_j) > 0 else None

            if cls_i is not None and cls_j is not None and cls_i != cls_j:
                continue
            if abs(cxi - cxj) > float(center_frac) * max(wi, wj):
                continue
            if abs(cyi - cyj) > float(center_frac) * max(hi, hj):
                continue

            if iou_xyxy((x1i, y1i, x2i, y2i), (x1j, y1j, x2j, y2j)) >= float(iou_thr):
                unite(i, j)
                continue

            ei = _vk_expand_xyxy(x1i, y1i, x2i, y2i, exp_frac)
            ej = _vk_expand_xyxy(x1j, y1j, x2j, y2j, exp_frac)
            if iou_xyxy(ei, ej) >= float(exp_iou_thr):
                unite(i, j)
                continue

            yov = _vk_axis_overlap_ratio(y1i, y2i, y1j, y2j)
            xov = _vk_axis_overlap_ratio(x1i, x2i, x1j, x2j)
            xgap = _vk_interval_gap(x1i, x2i, x1j, x2j)
            ygap = _vk_interval_gap(y1i, y2i, y1j, y2j)

            if (yov >= float(axis_ov_thr)) and (xgap <= float(gap_frac) * max(wi, wj)):
                unite(i, j)
                continue
            if (xov >= float(axis_ov_thr)) and (ygap <= float(gap_frac) * max(hi, hj)):
                unite(i, j)
                continue

    clusters = {}
    for idx in range(n):
        r = find(idx)
        clusters.setdefault(r, []).append(idx)

    merged = []
    for comp in clusters.values():
        x1 = min(P[k][0] for k in comp)
        y1 = min(P[k][1] for k in comp)
        x2 = max(P[k][2] for k in comp)
        y2 = max(P[k][3] for k in comp)

        best_k = max(comp, key=lambda k: P[k][4])
        c = P[best_k][4]
        tail = P[best_k][9]

        if x2 > x1 and y2 > y1:
            merged.append((int(round(x1)), int(round(y1)), int(round(x2)), int(round(y2)), float(c), *tail))

    merged.sort(key=lambda t: t[4], reverse=True)
    return merged


def merge_close_small(boxes, max_dist=6, max_size=26):
    small = []
    big = []

    for b in boxes:
        x1, y1, x2, y2, c = b[:5]
        tail = tuple(b[5:])
        w = x2 - x1
        h = y2 - y1
        item = (x1, y1, x2, y2, c, *tail)

        if min(w, h) <= max_size:
            small.append(item)
        else:
            big.append(item)

    used = [False] * len(small)
    merged_small = []

    for i, b in enumerate(small):
        if used[i]:
            continue

        x1, y1, x2, y2, c = b[:5]
        cx = 0.5 * (x1 + x2)
        cy = 0.5 * (y1 + y2)

        group = [b]
        used[i] = True

        for j in range(i + 1, len(small)):
            if used[j]:
                continue

            x1b, y1b, x2b, y2b, cb = small[j][:5]
            cxb = 0.5 * (x1b + x2b)
            cyb = 0.5 * (y1b + y2b)

            if abs(cxb - cx) <= max_dist and abs(cyb - cy) <= max_dist:
                group.append(small[j])
                used[j] = True

        xs1 = [g[0] for g in group]
        ys1 = [g[1] for g in group]
        xs2 = [g[2] for g in group]
        ys2 = [g[3] for g in group]

        best = max(group, key=lambda g: float(g[4]))
        conf = float(best[4])
        tail = tuple(best[5:])

        merged_small.append((
            min(xs1),
            min(ys1),
            max(xs2),
            max(ys2),
            conf,
            *tail
        ))

    return big + merged_small


def filter_boxes_geom(boxes):
    filtered = []

    for b in boxes:
        x1, y1, x2, y2, c = b[:5]
        tail = tuple(b[5:])

        w = x2 - x1
        h = y2 - y1
        if w <= 0 or h <= 0:
            continue

        s = min(w, h)
        ar = w / float(h)

        if s < 5:
            continue

        if ar < 0.25 or ar > 5.0:
            continue

        if s < 10 and c < 0.25:
            continue
        if 10 <= s < 20 and c < 0.15:
            continue
        if s >= 20 and c < 0.08:
            continue

        filtered.append((x1, y1, x2, y2, c, *tail))

    filtered = merge_close_small(filtered)
    return filtered


def rotate_k90_bgr(img_bgr: np.ndarray, k: int) -> np.ndarray:
    k %= 4
    if k == 0:
        return img_bgr
    if k == 1:
        return cv2.rotate(img_bgr, cv2.ROTATE_90_CLOCKWISE)
    if k == 2:
        return cv2.rotate(img_bgr, cv2.ROTATE_180)
    return cv2.rotate(img_bgr, cv2.ROTATE_90_COUNTERCLOCKWISE)


def _rotate_point_k90(x: float, y: float, W: int, H: int, k: int):
    k %= 4
    if k == 0:
        return x, y, W, H
    if k == 1:
        return (H - 1 - y), x, H, W
    if k == 2:
        return (W - 1 - x), (H - 1 - y), W, H
    return y, (W - 1 - x), H, W


def bbox_rotate_k90(bbox_xyxy, W: int, H: int, k: int):
    x1, y1, x2, y2 = map(float, bbox_xyxy)
    pts = [(x1, y1), (x2, y1), (x2, y2), (x1, y2)]
    rpts = []
    nW, nH = W, H
    for (x, y) in pts:
        rx, ry, nW, nH = _rotate_point_k90(x, y, W, H, k)
        rpts.append((rx, ry))
    xs = [p[0] for p in rpts]
    ys = [p[1] for p in rpts]
    return [int(round(min(xs))), int(round(min(ys))), int(round(max(xs))), int(round(max(ys)))], nW, nH


def rotate_boxes_k90_back(boxes_rot, W_base, H_base, k_applied):
    k = int(k_applied) % 4
    if k == 0:
        return boxes_rot

    Wr, Hr = (W_base, H_base) if (k % 2 == 0) else (H_base, W_base)
    k_inv = (-k) % 4

    out = []
    for box in boxes_rot:
        x1, y1, x2, y2, conf = box[:5]
        tail = tuple(box[5:])

        bb0, _, _ = bbox_rotate_k90([x1, y1, x2, y2], Wr, Hr, k_inv)
        X1, Y1, X2, Y2 = bb0

        X1 = max(0, min(int(X1), W_base - 1))
        X2 = max(0, min(int(X2), W_base - 1))
        Y1 = max(0, min(int(Y1), H_base - 1))
        Y2 = max(0, min(int(Y2), H_base - 1))

        if (X2 - X1) < 2 or (Y2 - Y1) < 2:
            continue

        out.append((X1, Y1, X2, Y2, float(conf), *tail))
    return out


def _rotate_bound(img_bgr: np.ndarray, angle_deg: float):
    h, w = img_bgr.shape[:2]
    if h < 2 or w < 2:
        return img_bgr, np.array([[1, 0, 0], [0, 1, 0]], dtype=np.float32), (w, h)

    cx, cy = w / 2.0, h / 2.0
    M = cv2.getRotationMatrix2D((cx, cy), float(angle_deg), 1.0).astype(np.float32)

    cos = abs(M[0, 0])
    sin = abs(M[0, 1])

    newW = int(h * sin + w * cos)
    newH = int(h * cos + w * sin)

    M[0, 2] += (newW / 2.0) - cx
    M[1, 2] += (newH / 2.0) - cy

    rot = cv2.warpAffine(
        img_bgr,
        M,
        (newW, newH),
        flags=cv2.INTER_LINEAR,
        borderMode=cv2.BORDER_CONSTANT,
        borderValue=(114, 114, 114)
    )
    return rot, M, (newW, newH)


def _apply_affine_to_points(M2x3: np.ndarray, pts_xy: np.ndarray):
    pts = np.asarray(pts_xy, dtype=np.float32).reshape(-1, 2)
    ones = np.ones((pts.shape[0], 1), dtype=np.float32)
    pts_h = np.hstack([pts, ones])
    out = (M2x3 @ pts_h.T).T
    return out


def _map_boxes_from_rotated_to_base(boxes_rot, M_base_to_rot, W_base, H_base):
    if not boxes_rot:
        return []

    Minv = cv2.invertAffineTransform(np.asarray(M_base_to_rot, dtype=np.float32))
    out = []

    for box in boxes_rot:
        x1, y1, x2, y2, conf = box[:5]
        tail = tuple(box[5:])

        pts = np.array([[x1, y1], [x2, y1], [x2, y2], [x1, y2]], dtype=np.float32)
        pts0 = _apply_affine_to_points(Minv, pts)

        xs = pts0[:, 0]
        ys = pts0[:, 1]

        xx1 = int(np.floor(xs.min()))
        yy1 = int(np.floor(ys.min()))
        xx2 = int(np.ceil(xs.max()))
        yy2 = int(np.ceil(ys.max()))

        xx1 = max(0, min(xx1, W_base - 1))
        yy1 = max(0, min(yy1, H_base - 1))
        xx2 = max(1, min(xx2, W_base))
        yy2 = max(1, min(yy2, H_base))

        if xx2 - xx1 < 2 or yy2 - yy1 < 2:
            continue

        out.append((xx1, yy1, xx2, yy2, float(conf), *tail))

    return out


def _map_bbox_from_base_to_rotated(bbox_xyxy, M_base_to_rot, W_rot, H_rot):
    x1, y1, x2, y2 = map(float, bbox_xyxy)
    pts = np.array([[x1, y1], [x2, y1], [x2, y2], [x1, y2]], dtype=np.float32)
    pr = _apply_affine_to_points(np.asarray(M_base_to_rot, dtype=np.float32), pts)

    xs = pr[:, 0]
    ys = pr[:, 1]

    xx1 = int(np.floor(xs.min()))
    yy1 = int(np.floor(ys.min()))
    xx2 = int(np.ceil(xs.max()))
    yy2 = int(np.ceil(ys.max()))

    xx1 = max(0, min(xx1, W_rot - 1))
    yy1 = max(0, min(yy1, H_rot - 1))
    xx2 = max(1, min(xx2, W_rot))
    yy2 = max(1, min(yy2, H_rot))

    if xx2 - xx1 < 3 or yy2 - yy1 < 3:
        return None

    return (xx1, yy1, xx2, yy2)


def _run_yolo_cars(im_bgr, conf, iou, imgsz, max_det, tta=False):
    if "yolo" not in globals() or yolo is None:
        raise RuntimeError("YOLO model is not initialized (yolo is None)")

    kwargs = dict(conf=conf, iou=iou, imgsz=imgsz, max_det=max_det, verbose=False)
    if "YOLO_DEVICE_ARG" in globals() and YOLO_DEVICE_ARG is not None:
        kwargs["device"] = YOLO_DEVICE_ARG
    if tta:
        kwargs["augment"] = True

    r = yolo(im_bgr, **kwargs)[0]
    boxes = []
    names = getattr(r, "names", None)
    if not names:
        names = getattr(getattr(yolo, "model", None), "names", {}) or {}

    for b in r.boxes:
        cls_id = int(b.cls.item())

        if ROT_KEEP_IDS is not None:
            if cls_id not in ROT_KEEP_IDS:
                continue
        else:
            if cls_id not in CAR_CLASSES:
                continue

        cls_name = names.get(cls_id, str(cls_id)) if isinstance(names, dict) else names[cls_id]
        cls_name = _normalized_vehicle_class(cls_name)

        x1, y1, x2, y2 = b.xyxy[0].cpu().numpy().tolist()
        boxes.append((int(x1), int(y1), int(x2), int(y2), float(b.conf.item()), int(cls_id), str(cls_name)))

    return boxes


def _looks_upside_down(img_bgr: np.ndarray, frac: float = 0.22, ratio_thr: float = 1.18) -> bool:
    if img_bgr is None or img_bgr.size == 0:
        return False
    h, w = img_bgr.shape[:2]
    if h < 64 or w < 64:
        return False
    k = max(8, int(h * float(frac)))
    y = cv2.cvtColor(img_bgr, cv2.COLOR_BGR2YCrCb)[:, :, 0].astype(np.float32)
    top = float(np.mean(y[:k, :]))
    bot = float(np.mean(y[h - k:, :]))
    if not (np.isfinite(top) and np.isfinite(bot)):
        return False
    return bot > top * float(ratio_thr)


UPRIGHT_CFG = {
    "max_side": 960,
    "gauss_ksize": 5,
    "canny1": 50,
    "canny2": 150,
    "hough_threshold": 80,
    "min_len_frac": 0.10,
    "max_line_gap": 20,
    "hor_tol_deg": 12.0,
    "ver_tol_deg": 12.0,
    "min_lines": 10,
    "min_group_weight_frac": 0.35,
    "max_abs_rot_deg": 25.0
}


def _estimate_upright_rotation_info(img_bgr: np.ndarray) -> dict:
    if img_bgr is None or img_bgr.size == 0:
        return {
            "angle_deg": 0.0,
            "confident": False,
            "aligned_frac": 0.0,
            "lines_count": 0,
            "hor_weight": 0.0,
            "ver_weight": 0.0
        }

    H, W = img_bgr.shape[:2]
    if H < 64 or W < 64:
        return {
            "angle_deg": 0.0,
            "confident": False,
            "aligned_frac": 0.0,
            "lines_count": 0,
            "hor_weight": 0.0,
            "ver_weight": 0.0
        }

    cfg = UPRIGHT_CFG
    max_side = int(cfg["max_side"])

    work = img_bgr
    if max(H, W) > max_side:
        s = max_side / float(max(H, W))
        work = cv2.resize(work, (max(2, int(W * s)), max(2, int(H * s))), interpolation=cv2.INTER_AREA)

    g = cv2.cvtColor(work, cv2.COLOR_BGR2GRAY)
    k = int(cfg["gauss_ksize"])
    if k >= 3 and (k % 2 == 1):
        g = cv2.GaussianBlur(g, (k, k), 0)

    edges = cv2.Canny(g, int(cfg["canny1"]), int(cfg["canny2"]))

    m = max(work.shape[:2])
    min_len = max(8, int(float(cfg["min_len_frac"]) * m))
    lines = cv2.HoughLinesP(
        edges,
        1,
        np.pi / 180.0,
        threshold=int(cfg["hough_threshold"]),
        minLineLength=min_len,
        maxLineGap=int(cfg["max_line_gap"])
    )
    if lines is None:
        return {
            "angle_deg": 0.0,
            "confident": False,
            "aligned_frac": 0.0,
            "lines_count": 0,
            "hor_weight": 0.0,
            "ver_weight": 0.0
        }

    hor_tol = float(cfg["hor_tol_deg"])
    ver_tol = float(cfg["ver_tol_deg"])

    sum_all = 0.0
    hor_vals = []
    hor_w = []
    ver_vals = []
    ver_w = []

    L = lines.reshape(-1, 4)
    for x1, y1, x2, y2 in L:
        dx = float(x2 - x1)
        dy = float(y2 - y1)
        length = math.hypot(dx, dy)
        if length < 1.0:
            continue

        ang = math.degrees(math.atan2(dy, dx))
        while ang >= 90.0:
            ang -= 180.0
        while ang < -90.0:
            ang += 180.0

        sum_all += length

        if abs(ang) <= hor_tol:
            hor_vals.append(float(ang))
            hor_w.append(float(length))
            continue

        if abs(abs(ang) - 90.0) <= ver_tol:
            dev = (ang - 90.0) if ang > 0.0 else (ang + 90.0)
            ver_vals.append(float(dev))
            ver_w.append(float(length))
            continue

    if sum_all <= 1e-6:
        return {
            "angle_deg": 0.0,
            "confident": False,
            "aligned_frac": 0.0,
            "lines_count": int(len(L)),
            "hor_weight": 0.0,
            "ver_weight": 0.0
        }

    def _wmean(vals, ws):
        sw = float(np.sum(ws))
        if sw <= 1e-6:
            return None
        return float(np.sum(np.array(vals, dtype=np.float32) * np.array(ws, dtype=np.float32)) / sw)

    wh = float(np.sum(hor_w)) if len(hor_w) else 0.0
    wv = float(np.sum(ver_w)) if len(ver_w) else 0.0
    aligned_frac = float((wh + wv) / sum_all)

    min_lines = int(cfg["min_lines"])
    min_frac = float(cfg["min_group_weight_frac"])

    use_group = None
    if wh >= wv:
        if len(hor_vals) >= min_lines and (wh / sum_all) >= min_frac:
            use_group = "hor"
    else:
        if len(ver_vals) >= min_lines and (wv / sum_all) >= min_frac:
            use_group = "ver"

    if use_group is None:
        return {
            "angle_deg": 0.0,
            "confident": False,
            "aligned_frac": aligned_frac,
            "lines_count": int(len(L)),
            "hor_weight": wh,
            "ver_weight": wv
        }

    if use_group == "hor":
        mean_dev = _wmean(hor_vals, hor_w)
        if mean_dev is None:
            return {
                "angle_deg": 0.0,
                "confident": False,
                "aligned_frac": aligned_frac,
                "lines_count": int(len(L)),
                "hor_weight": wh,
                "ver_weight": wv
            }
        rot = -mean_dev
    else:
        mean_dev = _wmean(ver_vals, ver_w)
        if mean_dev is None:
            return {
                "angle_deg": 0.0,
                "confident": False,
                "aligned_frac": aligned_frac,
                "lines_count": int(len(L)),
                "hor_weight": wh,
                "ver_weight": wv
            }
        rot = -mean_dev

    rot = float(rot)
    if abs(rot) > float(cfg["max_abs_rot_deg"]):
        return {
            "angle_deg": 0.0,
            "confident": False,
            "aligned_frac": aligned_frac,
            "lines_count": int(len(L)),
            "hor_weight": wh,
            "ver_weight": wv
        }

    if abs(rot) < 0.2:
        rot = 0.0

    return {
        "angle_deg": rot,
        "confident": True,
        "aligned_frac": aligned_frac,
        "lines_count": int(len(L)),
        "hor_weight": wh,
        "ver_weight": wv
    }


def _estimate_quadrant_rotation_info(img_bgr: np.ndarray) -> dict:
    if img_bgr is None or img_bgr.size == 0:
        return {"k": 0, "angle_deg": 0.0, "confident": False, "score": -1e9}

    candidates = []
    base_score = None

    for k in (0, 1, 2, 3):
        cand = rotate_k90_bgr(img_bgr, k)
        info = _estimate_upright_rotation_info(cand)

        aligned_frac = float(info.get("aligned_frac", 0.0))
        lines_count = int(info.get("lines_count", 0))
        abs_small = abs(float(info.get("angle_deg", 0.0)))
        confident = bool(info.get("confident", False))
        upside = _looks_upside_down(cand)

        score = (
                (10000.0 if confident else 0.0) +
                aligned_frac * 1000.0 +
                min(lines_count, 500) * 0.1 -
                abs_small * 2.0 -
                (25.0 if upside else 0.0)
        )

        item = {
            "k": k,
            "angle_deg": _k90_to_angle_deg(k),
            "confident": confident,
            "score": score,
            "aligned_frac": aligned_frac,
            "lines_count": lines_count,
            "small_rot_deg": float(info.get("angle_deg", 0.0)),
            "upside_down": upside
        }
        candidates.append(item)
        if k == 0:
            base_score = score

    best = max(candidates, key=lambda x: x["score"])

    if best["k"] != 0 and base_score is not None and best["score"] < base_score + 5.0:
        return {
            "k": 0,
            "angle_deg": 0.0,
            "confident": candidates[0]["confident"],
            "score": candidates[0]["score"],
            "aligned_frac": candidates[0]["aligned_frac"],
            "lines_count": candidates[0]["lines_count"],
            "small_rot_deg": candidates[0]["small_rot_deg"],
            "upside_down": candidates[0]["upside_down"]
        }

    return best


def estimate_upright_rotation_deg(img_bgr: np.ndarray) -> float:
    return float(_estimate_upright_rotation_info(img_bgr)["angle_deg"])


def canonicalize_orientation(
        img_bgr: np.ndarray,
        min_abs_rot_deg: float = 0.2,
        enable_upside_down: bool = True,
        exif_orientation_deg=None
):
    identity = np.array([[1, 0, 0], [0, 1, 0]], dtype=np.float32)

    if img_bgr is None or img_bgr.size == 0:
        return img_bgr, {
            "angle_deg": 0.0,
            "M": identity,
            "orientation_confident": False,
            "orientation_source": "empty"
        }

    img1 = img_bgr
    M_total = identity.copy()
    rot_total = 0.0
    confident = False
    source = "none"

    exif_deg = _safe_float(exif_orientation_deg)
    if exif_deg is not None:
        exif_deg = _normalize_rotation_deg(exif_deg)
        if abs(exif_deg) >= 1e-6:
            img1, M0, a0 = _apply_rotation_with_matrix(img1, exif_deg)
            M_total = _affine_compose(M0, identity)
            rot_total = _normalize_rotation_deg(rot_total + a0)
            confident = True
            source = "exif"

    if source == "none":
        quad = _estimate_quadrant_rotation_info(img1)
        k = int(quad.get("k", 0))
        if k != 0:
            H0, W0 = img1.shape[:2]
            img1 = rotate_k90_bgr(img1, k)
            M0 = _k90_affine_matrix(W0, H0, k)
            M_total = _affine_compose(M0, M_total)
            rot_total = _normalize_rotation_deg(rot_total + _k90_to_angle_deg(k))
            source = "k90"
        confident = confident or bool(quad.get("confident", False))

    info = _estimate_upright_rotation_info(img1)
    rot1 = float(info.get("angle_deg", 0.0))
    info_confident = bool(info.get("confident", False))

    if info_confident and abs(rot1) >= float(min_abs_rot_deg):
        img2, M1, _ = _rotate_bound(img1, rot1)
        M_total = _affine_compose(M1, M_total)
        img1 = img2
        rot_total = _normalize_rotation_deg(rot_total + rot1)
        confident = True
        if source == "none":
            source = "small_angle"
    elif info_confident:
        confident = True
        if source == "none":
            source = "small_angle_zero"

    if enable_upside_down and _looks_upside_down(img1):
        img2, M2, _ = _apply_rotation_with_matrix(img1, 180.0)
        M_total = _affine_compose(M2, M_total)
        img1 = img2
        rot_total = _normalize_rotation_deg(rot_total + 180.0)
        confident = True
        if source == "none":
            source = "upside_down"
        else:
            source = source + "+180"

    return img1, {
        "angle_deg": float(_normalize_rotation_deg(rot_total)),
        "M": np.asarray(M_total, dtype=np.float32),
        "orientation_confident": bool(confident),
        "orientation_source": source
    }


def estimate_r_img_deg(img_bgr: np.ndarray, exif_orientation_deg=None):
    if img_bgr is None or img_bgr.size == 0:
        return None, "empty"

    img_vis = img_bgr
    source_prefix = "visual"

    exif_deg = _safe_float(exif_orientation_deg)
    if exif_deg is not None:
        exif_deg = _normalize_rotation_deg(exif_deg)
        if abs(exif_deg) >= 1e-6:
            img_vis, _, _ = _apply_rotation_with_matrix(img_vis, exif_deg)
            source_prefix = "exif+visual"

    _, tf_vis = canonicalize_orientation(
        img_vis,
        exif_orientation_deg=None
    )

    if tf_vis is None:
        return 0.0, source_prefix

    source_tail = str(tf_vis.get("orientation_source", "")).strip()
    source = source_prefix if not source_tail else (source_prefix + ":" + source_tail)
    return float(tf_vis.get("angle_deg", 0.0)), source


def _containment_suppression(boxes, contain_thr=0.92, conf_margin=0.02):
    if not boxes:
        return []

    bxs = sorted(boxes, key=lambda x: x[4], reverse=True)
    keep = []
    for b in bxs:
        x1, y1, x2, y2, cb = b[:5]
        area_b = max(0, x2 - x1) * max(0, y2 - y1)
        if area_b <= 0:
            continue

        cls_b = _box_cls_id(b)
        drop = False

        for a in keep:
            cls_a = _box_cls_id(a)
            if cls_a is not None and cls_b is not None and cls_a != cls_b:
                continue

            ax1, ay1, ax2, ay2, ca = a[:5]
            ix1, iy1 = max(x1, ax1), max(y1, ay1)
            ix2, iy2 = min(x2, ax2), min(y2, ay2)
            iw, ih = max(0, ix2 - ix1), max(0, iy2 - iy1)
            inter = iw * ih
            if inter <= 0:
                continue

            cover = inter / (area_b + 1e-9)
            if cover >= contain_thr and ca + conf_margin >= cb:
                drop = True
                break

        if not drop:
            keep.append(b)

    return keep


def _box_area(b):
    return max(0, int(b[2]) - int(b[0])) * max(0, int(b[3]) - int(b[1]))


def _box_center(b):
    return (0.5 * (float(b[0]) + float(b[2])), 0.5 * (float(b[1]) + float(b[3])))


def _box_min_side(b):
    return float(min(max(0, int(b[2]) - int(b[0])), max(0, int(b[3]) - int(b[1]))))


def _containment_cover(a, b):
    ax1, ay1, ax2, ay2 = map(float, a[:4])
    bx1, by1, bx2, by2 = map(float, b[:4])

    ix1, iy1 = max(ax1, bx1), max(ay1, by1)
    ix2, iy2 = min(ax2, bx2), min(ay2, by2)
    iw, ih = max(0.0, ix2 - ix1), max(0.0, iy2 - iy1)
    inter = iw * ih
    area_b = max(0.0, bx2 - bx1) * max(0.0, by2 - by1)
    if area_b <= 1e-9:
        return 0.0
    return float(inter / area_b)


def _pick_better_box(a, b, conf_margin=0.03):
    ca = float(a[4])
    cb = float(b[4])

    if ca > cb + conf_margin:
        return a
    if cb > ca + conf_margin:
        return b

    aa = _box_area(a)
    ab = _box_area(b)
    return a if aa >= ab else b


def _drop_big_if_contains_small(boxes, contain_thr=0.92, conf_margin=0.03, area_ratio=1.6):
    if not boxes:
        return []
    bxs = sorted(boxes, key=lambda b: _box_area(b))
    alive = [True] * len(bxs)

    for i in range(len(bxs)):
        if not alive[i]:
            continue
        small = bxs[i]
        a_small = _box_area(small)
        if a_small <= 0:
            continue

        for j in range(i + 1, len(bxs)):
            if not alive[j]:
                continue
            big = bxs[j]
            a_big = _box_area(big)
            if a_big <= 0:
                alive[j] = False
                continue

            if _containment_cover(big, small) >= contain_thr:
                if (float(small[4]) >= float(big[4]) - conf_margin) or (a_big >= area_ratio * a_small):
                    alive[j] = False

    return [bxs[i] for i in range(len(bxs)) if alive[i]]


def _same_object(a, b, iou_thr=0.20, center_thr=0.45, area_ratio_min=0.35, area_ratio_max=2.8, contain_thr=0.92):
    ia = float(iou_xyxy(a[:4], b[:4]))
    if ia >= iou_thr:
        return True

    if _containment_cover(a, b) >= contain_thr:
        return True
    if _containment_cover(b, a) >= contain_thr:
        return True

    aa = _box_area(a)
    ab = _box_area(b)
    if aa <= 0 or ab <= 0:
        return False

    r = aa / (ab + 1e-9)
    if r < area_ratio_min or r > area_ratio_max:
        return False

    (cx1, cy1) = _box_center(a)
    (cx2, cy2) = _box_center(b)
    dist = math.hypot(cx1 - cx2, cy1 - cy2)

    scale = max(6.0, 0.5 * (_box_min_side(a) + _box_min_side(b)))
    return dist <= center_thr * scale


def dedup_one_box_per_car(
        boxes,
        nms_thr=0.45,
        contain_thr=0.92,
        conf_margin=0.03,
        center_thr=0.45
):
    if not boxes:
        return []

    boxes = filter_boxes_geom(boxes)
    boxes = nms_union(boxes, thr=float(nms_thr))
    boxes = _drop_big_if_contains_small(boxes, contain_thr=contain_thr, conf_margin=conf_margin, area_ratio=1.6)
    boxes = _containment_suppression(boxes, contain_thr=contain_thr, conf_margin=0.02)

    boxes = sorted(boxes, key=lambda b: float(b[4]), reverse=True)
    kept = []
    for b in boxes:
        merged = False
        for k_idx in range(len(kept)):
            k = kept[k_idx]
            if _same_object(b, k, iou_thr=0.20, center_thr=center_thr, contain_thr=contain_thr):
                kept[k_idx] = _pick_better_box(b, k, conf_margin=conf_margin)
                merged = True
                break
        if not merged:
            kept.append(b)

    kept = nms_union(kept, thr=max(0.35, float(nms_thr) - 0.05))
    return kept


DET_CFG = {
    "conf": 0.18,
    "iou": 0.55,
    "imgsz": 1984,
    "max_det": 10000,
    "tiling": "auto",
    "tile": 960,
    "overlap": 0.20,
    "k90": (0, 1, 2, 3),
    "use_upright": True,
    "upright_thr_deg": 7.0,
    "merge_iou": 0.55,
    "do_containment": True,
    "tta": False,
    "merge_parts": True,
    "merge_parts_iou_thr": 0.25,
    "merge_parts_exp_frac": 0.25,
    "merge_parts_exp_iou_thr": 0.05,
    "merge_parts_axis_ov_thr": 0.60,
    "merge_parts_gap_frac": 0.35,
    "merge_parts_center_frac": 2.0
}


def _run_yolo_cars_tiled(im_bgr, conf, iou, imgsz, max_det, tta, tile, overlap):
    H, W = im_bgr.shape[:2]
    if H < 2 or W < 2:
        return []

    t = int(tile)
    ov = float(overlap)
    step = max(1, int(round(t * (1.0 - ov))))

    out = []
    y = 0
    while y < H:
        x = 0
        y2 = min(y + t, H)
        while x < W:
            x2 = min(x + t, W)
            crop = im_bgr[y:y2, x:x2]
            boxes = _run_yolo_cars(crop, conf=conf, iou=iou, imgsz=imgsz, max_det=max_det, tta=tta)
            for box in boxes:
                x1, y1, x3, y3, c = box[:5]
                tail = tuple(box[5:])
                out.append((int(x1 + x), int(y1 + y), int(x3 + x), int(y3 + y), float(c), *tail))
            x += step
        y += step
    return out


def _detect_on_image(im_bgr, conf, iou, imgsz, max_det, tta, tiling, tile, overlap):
    H, W = im_bgr.shape[:2]
    tile = int(tile)

    if tiling == "always":
        use_tiling = True
    elif tiling == "auto":
        use_tiling = (H > tile and W > tile)
    else:
        use_tiling = False

    if use_tiling:
        return _run_yolo_cars_tiled(im_bgr, conf, iou, imgsz, max_det, tta, tile, overlap)
    return _run_yolo_cars(im_bgr, conf, iou, imgsz, max_det, tta=tta)


def _detect_all_cars_k90_fallback(
        img_bgr,
        conf,
        iou,
        imgsz,
        max_det,
        tiling,
        tile,
        overlap,
        merge_iou,
        tta,
        k90=None
):
    H0, W0 = img_bgr.shape[:2]
    rotations = (0, 1, 2, 3) if k90 is None else tuple(int(x) % 4 for x in k90)

    all_boxes = []
    for kk in rotations:
        im = rotate_k90_bgr(img_bgr, kk)
        boxes_rot = _detect_on_image(im, conf, iou, imgsz, max_det, tta, tiling, tile, overlap)
        boxes_base = rotate_boxes_k90_back(boxes_rot, W0, H0, kk)
        all_boxes.extend(boxes_base)

    if not all_boxes:
        return []

    return dedup_one_box_per_car(
        all_boxes,
        nms_thr=float(merge_iou),
        contain_thr=0.92,
        conf_margin=0.03,
        center_thr=0.45,
    )


def _detect_all_cars_like_clusters_raw(img_bgr, max_det):
    if img_bgr is None or img_bgr.size == 0:
        return []

    cluster_conf = 0.25
    cluster_iou = 0.55
    cluster_imgsz = 640
    cluster_tile = 640
    cluster_overlap = 0.20

    boxes = _run_yolo_cars(
        img_bgr,
        conf=cluster_conf,
        iou=cluster_iou,
        imgsz=cluster_imgsz,
        max_det=max_det,
        tta=False
    )

    if boxes:
        return dedup_one_box_per_car(
            boxes,
            nms_thr=0.45,
            contain_thr=0.92,
            conf_margin=0.03,
            center_thr=0.45,
        )

    boxes = _run_yolo_cars_tiled(
        img_bgr,
        conf=cluster_conf,
        iou=cluster_iou,
        imgsz=cluster_imgsz,
        max_det=max_det,
        tta=False,
        tile=cluster_tile,
        overlap=cluster_overlap
    )

    if not boxes:
        return []

    return dedup_one_box_per_car(
        boxes,
        nms_thr=0.45,
        contain_thr=0.92,
        conf_margin=0.03,
        center_thr=0.45,
    )


def detect_all_cars(
        img_bgr,
        conf=None,
        iou=None,
        imgsz=None,
        max_det=None,
        do_containment=None,
        tiling=None,
        tile=None,
        overlap=None,
        k90=None,
        use_upright=None,
        upright_thr_deg=None,
        merge_iou=None,
        tta=None,
        img_work=None,
        tf=None,
        exif_orientation_deg=None
):
    if img_bgr is None or img_bgr.size == 0:
        return []

    cfg = DET_CFG
    max_det = cfg["max_det"] if max_det is None else int(max_det)

    return _detect_all_cars_like_clusters_raw(
        img_bgr,
        max_det=max_det
    )

def wrap180(a: float) -> float:
    a = ((float(a) + 180.0) % 360.0) - 180.0
    return a


def line_angle(x1, y1, x2, y2):
    ang = math.atan2(y2 - y1, x2 - x1)
    if ang > math.pi / 2:
        ang -= math.pi
    if ang < -math.pi / 2:
        ang += math.pi
    return ang


def line_abcd(l):
    x1, y1, x2, y2 = l
    a = y1 - y2
    b = x2 - x1
    c = x1 * y2 - x2 * y1
    s = math.hypot(a, b) + 1e-9
    return a / s, b / s, c / s


def intersect_two(l1, l2):
    a1, b1, c1 = line_abcd(l1)
    a2, b2, c2 = line_abcd(l2)
    det = a1 * b2 - a2 * b1
    if abs(det) < 1e-8:
        return None
    x = (b1 * (-c2) - b2 * (-c1)) / det
    y = (a2 * (-c1) - a1 * (-c2)) / det
    return (float(x), float(y))


def vp_ransac(lines, iters=900):
    lines = np.asarray(lines, dtype=np.float32)
    n = len(lines)
    if n < 3:
        return None

    w = np.hypot(lines[:, 2] - lines[:, 0], lines[:, 3] - lines[:, 1]).astype(np.float64)
    w = w / (w.sum() + 1e-12)

    best_err = 1e30
    best = None

    for _ in range(iters):
        i = int(np.random.choice(n, p=w))
        j = int(np.random.choice(n, p=w))
        if i == j:
            continue

        p = intersect_two(lines[i], lines[j])
        if p is None:
            continue
        x, y = p
        if (not np.isfinite(x)) or (not np.isfinite(y)):
            continue

        err = 0.0
        for k in range(n):
            a, b, c = line_abcd(lines[k])
            err += w[k] * abs(a * x + b * y + c)

        if err < best_err:
            best_err = err
            best = (float(x), float(y))

    return best


def project_to_SO3(R):
    if R is None or not np.all(np.isfinite(R)):
        return None
    try:
        U, S, Vt = np.linalg.svd(R)
        Rso = U @ Vt
        if np.linalg.det(Rso) < 0:
            Vt[-1, :] *= -1
            Rso = U @ Vt
        return Rso
    except np.linalg.LinAlgError:
        return None


def yaw_guess_from_single_vp(vp, cx, cy, f):
    if vp is None or f is None or (not np.isfinite(f)) or f <= 1e-6:
        return None
    dx = (float(vp[0]) - cx) / f
    dz = 1.0
    yaw = math.degrees(math.atan2(dx, dz))
    yaw = ((yaw + 180) % 360) - 180
    return None if not np.isfinite(yaw) else yaw


def make_lsd_roi(img_bgr, bbox, pad_frac=0.18, min_side=96):
    if img_bgr is None or img_bgr.size == 0:
        return None, (0, 0)

    H, W = img_bgr.shape[:2]
    x1, y1, x2, y2 = map(int, bbox)

    x1 = max(0, min(x1, W - 1))
    y1 = max(0, min(y1, H - 1))
    x2 = max(1, min(x2, W))
    y2 = max(1, min(y2, H))

    if x2 <= x1 or y2 <= y1:
        return None, (0, 0)

    bw = x2 - x1
    bh = y2 - y1
    cx = 0.5 * (x1 + x2)
    cy = 0.5 * (y1 + y2)

    half_w = max(0.5 * float(min_side), 0.5 * bw * (1.0 + 2.0 * float(pad_frac)))
    half_h = max(0.5 * float(min_side), 0.5 * bh * (1.0 + 2.0 * float(pad_frac)))

    rx1 = int(math.floor(cx - half_w))
    ry1 = int(math.floor(cy - half_h))
    rx2 = int(math.ceil(cx + half_w))
    ry2 = int(math.ceil(cy + half_h))

    rx1 = max(0, min(rx1, W - 1))
    ry1 = max(0, min(ry1, H - 1))
    rx2 = max(1, min(rx2, W))
    ry2 = max(1, min(ry2, H))

    if rx2 - rx1 < 2 or ry2 - ry1 < 2:
        return None, (0, 0)

    return img_bgr[ry1:ry2, rx1:rx2], (rx1, ry1)


def inplane_angle(img, bbox):
    x1, y1, x2, y2 = map(int, bbox)
    roi = img[y1:y2, x1:x2]
    if roi is None or roi.size == 0:
        return None

    h, w = roi.shape[:2]
    if h < 10 or w < 10:
        return None

    binm = grabcut_fg_mask(roi, iters=2, margin=0.08)
    if binm is not None:
        ys, xs = np.where(binm > 0)
    else:
        ys = xs = None

    if xs is None or len(xs) < 80:
        g = cv2.cvtColor(roi, cv2.COLOR_BGR2GRAY)
        g = cv2.createCLAHE(clipLimit=2.0, tileGridSize=(8, 8)).apply(g)
        g = cv2.GaussianBlur(g, (5, 5), 0)
        e = cv2.Canny(g, 50, 150)

        b = 2
        e[:b, :] = 0
        e[-b:, :] = 0
        e[:, :b] = 0
        e[:, -b:] = 0

        ys, xs = np.where(e > 0)

    if xs is None or len(xs) < 80:
        return None

    pts = np.column_stack([xs.astype(np.float32), ys.astype(np.float32)])
    pts -= pts.mean(axis=0, keepdims=True)

    cov = (pts.T @ pts) / max(1.0, float(pts.shape[0]))
    evals, evecs = np.linalg.eigh(cov)

    idx = int(np.argmax(evals))
    vx, vy = float(evecs[0, idx]), float(evecs[1, idx])

    ang = math.degrees(math.atan2(vy, vx))
    ang = ((ang + 180.0) % 180.0) - 90.0
    if ang >= 90.0:
        ang -= 180.0
    if ang < -90.0:
        ang += 180.0

    return None if not np.isfinite(ang) else float(ang)


def lsd_lines(roi_bgr, max_side=320):
    if roi_bgr is None or roi_bgr.size == 0:
        return np.empty((0, 4), np.float32)

    H, W = roi_bgr.shape[:2]
    scale = 1.0
    work = roi_bgr

    if max(H, W) > max_side:
        scale = max_side / float(max(H, W))
        work = cv2.resize(roi_bgr, (int(W * scale), int(H * scale)), interpolation=cv2.INTER_AREA)

    gray = cv2.cvtColor(work, cv2.COLOR_BGR2GRAY)
    gray = cv2.createCLAHE(clipLimit=2.0, tileGridSize=(8, 8)).apply(gray)
    gray = cv2.GaussianBlur(gray, (3, 3), 0)

    lsd = cv2.createLineSegmentDetector()
    res = lsd.detect(gray)
    lines = res[0] if res is not None else None
    if lines is None:
        return np.empty((0, 4), np.float32)

    L = lines.reshape(-1, 4).astype(np.float32)

    lens = np.hypot(L[:, 2] - L[:, 0], L[:, 3] - L[:, 1])
    min_len = max(6, 0.02 * max(gray.shape[:2]))
    L = L[lens > min_len]

    if scale != 1.0 and len(L) > 0:
        L /= scale

    return L


def grabcut_fg_mask(bgr, iters=2, margin=0.05):
    h, w = bgr.shape[:2]
    if h < 16 or w < 16:
        return None

    mask = np.zeros((h, w), np.uint8)
    bg = np.zeros((1, 65), np.float64)
    fg = np.zeros((1, 65), np.float64)

    x = int(w * margin)
    y = int(h * margin)
    rw = max(1, int(w * (1 - 2 * margin)))
    rh = max(1, int(h * (1 - 2 * margin)))
    rect = (x, y, rw, rh)

    try:
        cv2.grabCut(bgr, mask, rect, bg, fg, iters, cv2.GC_INIT_WITH_RECT)
        binm = np.where((mask == 1) | (mask == 3), 255, 0).astype(np.uint8)
        binm = cv2.morphologyEx(binm, cv2.MORPH_OPEN, np.ones((3, 3), np.uint8), iterations=1)
        binm = cv2.morphologyEx(binm, cv2.MORPH_CLOSE, np.ones((5, 5), np.uint8), iterations=1)

        if binm.sum() < 0.02 * h * w * 255:
            return None

        return binm
    except Exception:
        return None


def filter_lines_by_mask(L, binmask):
    if binmask is None:
        return L
    if L is None or len(L) == 0:
        return np.empty((0, 4), np.float32)

    h, w = binmask.shape[:2]
    kept = []
    for x1, y1, x2, y2 in np.asarray(L, dtype=np.float32):
        mx = int(0.5 * (x1 + x2))
        my = int(0.5 * (y1 + y2))
        if 0 <= mx < w and 0 <= my < h and binmask[my, mx] > 0:
            kept.append([x1, y1, x2, y2])

    if not kept:
        return np.empty((0, 4), np.float32)
    return np.asarray(kept, dtype=np.float32)


def cluster_angles(L, k=3):
    if len(L) < 6:
        return [[], [], []]
    angs = np.array([line_angle(*l) for l in L], dtype=np.float32).reshape(-1, 1)
    centers = np.linspace(-math.pi / 2, math.pi / 2, k, endpoint=False).reshape(k, 1).astype(np.float32)
    for _ in range(20):
        d = (angs - centers.T) ** 2
        lab = d.argmin(axis=1)
        for i in range(k):
            pts = angs[lab == i]
            if len(pts):
                centers[i] = np.median(pts)
    return [L[lab == i] for i in range(k)]


def vanishing_point(lines):
    lines = np.asarray(lines, dtype=np.float32)
    if len(lines) < 3:
        return None

    p = vp_ransac(lines, iters=900)
    if p is None:
        return None

    xs, ys = [], []
    n = len(lines)
    for i in range(n):
        for j in range(i + 1, n):
            q = intersect_two(lines[i], lines[j])
            if q is None:
                continue
            if np.isfinite(q[0]) and np.isfinite(q[1]):
                xs.append(q[0])
                ys.append(q[1])

    if len(xs) >= 10:
        return (float(np.median(xs)), float(np.median(ys)))

    return p


def global_vps(img_bgr, max_side=1024):
    H, W = img_bgr.shape[:2]
    if H < 64 or W < 64:
        return None, None, None

    scale = 1.0
    if max(H, W) > max_side:
        scale = max_side / float(max(H, W))
        small = cv2.resize(img_bgr, (int(W * scale), int(H * scale)), interpolation=cv2.INTER_AREA)
    else:
        small = img_bgr

    h, w = small.shape[:2]
    roi = small

    L_raw = lsd_lines(roi)

    binm_roi = None
    if min(roi.shape[:2]) >= 48:
        binm_roi = grabcut_fg_mask(roi, iters=2, margin=0.06)

    L = filter_lines_by_mask(L_raw, binm_roi)

    if len(L) < max(6, int(0.25 * len(L_raw))):
        L = L_raw

    groups = cluster_angles(L, k=3)

    vps = []
    counts = []
    for g in groups:
        g = np.asarray(g, dtype=np.float32)
        counts.append(int(len(g)))
        if len(g) >= 15:
            vps.append(vanishing_point(g))
        else:
            vps.append(None)

    cx, cy = w / 2.0, h / 2.0
    best = None
    best_score = -1

    for i in range(3):
        for j in range(i + 1, 3):
            vi = vps[i]
            vj = vps[j]
            if vi is None or vj is None:
                continue

            ux1, uy1 = vi[0] - cx, vi[1] - cy
            ux2, uy2 = vj[0] - cx, vj[1] - cy
            f2 = -(ux1 * ux2 + uy1 * uy2)

            if (not np.isfinite(f2)) or (f2 <= 1e-3):
                continue

            f = math.sqrt(float(f2))
            if not (0.2 * max(w, h) <= f <= 5.0 * max(w, h)):
                continue

            score = counts[i] + counts[j]
            if score > best_score:
                best_score = score
                best = (vi, vj, f)

    if best is None:
        return None, None, None

    (vp1, vp2, f_small) = best

    inv = 1.0 / scale
    vp1 = (vp1[0] * inv, vp1[1] * inv)
    vp2 = (vp2[0] * inv, vp2[1] * inv)
    f_global = float(f_small * inv)

    return vp1, vp2, f_global


def _shift_vp(vp, offset_xy):
    if vp is None:
        return None
    ox, oy = offset_xy
    return (float(vp[0]) + float(ox), float(vp[1]) + float(oy))


def _vp_pair_ok(v1, v2, cx, cy, W, H):
    if v1 is None or v2 is None:
        return False

    ux1, uy1 = v1[0] - cx, v1[1] - cy
    ux2, uy2 = v2[0] - cx, v2[1] - cy
    f2 = -(ux1 * ux2 + uy1 * uy2)

    if (not np.isfinite(f2)) or (f2 <= 1e-3):
        return False

    f = math.sqrt(float(f2))
    m = max(W, H)
    return 0.2 * m <= f <= 5.0 * m


def _extract_local_pose_vps(img_work, bbox_work):
    inpl = inplane_angle(img_work, bbox_work)

    roi, offset_xy = make_lsd_roi(img_work, bbox_work)
    if roi is None or roi.size == 0:
        return inpl, None, None

    L_raw = lsd_lines(roi)
    if len(L_raw) < 6:
        return inpl, None, None

    binm_roi = None
    if min(roi.shape[:2]) >= 48:
        binm_roi = grabcut_fg_mask(roi, iters=1, margin=0.06)

    L = filter_lines_by_mask(L_raw, binm_roi)
    if len(L) < max(6, int(0.25 * len(L_raw))):
        L = L_raw

    groups = cluster_angles(L, k=3)
    if len(groups) != 3:
        return inpl, None, None

    vps_local = [vanishing_point(g) if len(g) >= 3 else None for g in groups]

    ang_cent = []
    for g in groups:
        if len(g) == 0:
            ang_cent.append(None)
        else:
            ang_cent.append(float(np.median([line_angle(*l) for l in g])))

    def _adiff(a, b):
        if a is None or b is None:
            return 1e9
        d = a - b
        while d > math.pi / 2:
            d -= math.pi
        while d < -math.pi / 2:
            d += math.pi
        return abs(d)

    if inpl is not None:
        tgt = math.radians(float(inpl))
        idx_len = int(np.argmin([_adiff(a, tgt) for a in ang_cent]))
        idx_wid = int(np.argmin([_adiff(a, tgt + math.pi / 2) for a in ang_cent]))

        if idx_wid == idx_len:
            absdeg = [999.0 if a is None else abs(math.degrees(a)) for a in ang_cent]
            idx_height = int(np.argmax(absdeg))
            idx_len = int(np.argmin(absdeg))
            idx_wid = list({0, 1, 2} - {idx_height, idx_len})[0]
    else:
        absdeg = [999.0 if a is None else abs(math.degrees(a)) for a in ang_cent]
        idx_height = int(np.argmax(absdeg))
        idx_len = int(np.argmin(absdeg))
        idx_wid = list({0, 1, 2} - {idx_height, idx_len})[0]

    vp_len = _shift_vp(vps_local[idx_len], offset_xy)
    vp_wid = _shift_vp(vps_local[idx_wid], offset_xy)

    return inpl, vp_len, vp_wid


def pose_from_two_vp(vx, vy, cx, cy, f_default, f_override=None):
    if vx is None or vy is None:
        return None

    f = float(f_default)
    if (f_override is not None) and np.isfinite(f_override) and (f_override > 1e-6):
        f = float(f_override)

    Kinv = np.array([
        [1 / f, 0, -cx / f],
        [0, 1 / f, -cy / f],
        [0, 0, 1]
    ], dtype=np.float64)

    r1 = Kinv @ np.array([vx[0], vx[1], 1.0])
    r2 = Kinv @ np.array([vy[0], vy[1], 1.0])

    if (not np.all(np.isfinite(r1))) or (not np.all(np.isfinite(r2))):
        return None

    r1 /= (np.linalg.norm(r1) + 1e-9)
    r2 /= (np.linalg.norm(r2) + 1e-9)

    r3 = np.cross(r1, r2)
    n3 = np.linalg.norm(r3)
    if (not np.isfinite(n3)) or (n3 < 1e-6):
        return None
    r3 /= n3

    R = np.stack([r1, r2, r3], axis=1)
    return project_to_SO3(R)


def euler_zyx_from_R(R):
    if R is None or not np.all(np.isfinite(R)):
        return None
    yaw = math.degrees(math.atan2(R[1, 0], R[0, 0]))
    sy = math.sqrt(R[0, 0] ** 2 + R[1, 0] ** 2)
    pitch = math.degrees(math.atan2(-R[2, 0], sy))
    roll = math.degrees(math.atan2(R[2, 1], R[2, 2]))
    if any(map(lambda x: not np.isfinite(x), [yaw, pitch, roll])):
        return None
    return yaw, pitch, roll

def process_image(img_bgr, exif_orientation_deg=None):
    if img_bgr is None:
        return None, []

    r_img_deg, r_img_source = estimate_r_img_deg(
        img_bgr,
        exif_orientation_deg=exif_orientation_deg
    )

    img_work, tf = canonicalize_orientation(
        img_bgr,
        exif_orientation_deg=exif_orientation_deg
    )
    if img_work is None or img_work.size == 0:
        return None, []

    H, W = img_work.shape[:2]

    boxes = detect_all_cars(
        img_bgr,
        img_work=img_work,
        tf=tf,
        exif_orientation_deg=exif_orientation_deg
    )

    vis = img_bgr.copy()
    if not boxes:
        return vis, []

    f_default = 0.9 * max(W, H)
    vp_g1, vp_g2, f_g = global_vps(img_work, max_side=1024)

    cx, cy = W / 2.0, H / 2.0
    focal_for_metric = _estimate_metric_focal_px(
        image_w=W,
        image_h=H,
        f_vp=f_g
    )

    results = []

    for i, box in enumerate(boxes, 1):
        x1, y1, x2, y2, conf = box[:5]
        cls_id = int(box[5]) if len(box) > 5 else 2
        cls_name = _normalized_vehicle_class(box[6] if len(box) > 6 else _cls_name_from_id(cls_id))

        bb_w = _map_bbox_from_base_to_rotated((x1, y1, x2, y2), tf["M"], W, H) if tf is not None else (x1, y1, x2, y2)
        if bb_w is None:
            continue

        inpl, vp_len, vp_wid = _extract_local_pose_vps(img_work, bb_w)

        yaw = pitch = roll = None
        pose_src = "none"

        vp1, vp2 = vp_len, vp_wid
        f_use = f_default

        if _vp_pair_ok(vp1, vp2, cx, cy, W, H):
            pose_src = "local"
        elif _vp_pair_ok(vp_g1, vp_g2, cx, cy, W, H):
            vp1, vp2 = vp_g1, vp_g2
            f_use = f_g if (f_g is not None and np.isfinite(f_g) and f_g > 1e-6) else f_default
            pose_src = "global_flat"

        if vp1 is not None and vp2 is not None:
            Rm = pose_from_two_vp(vp1, vp2, cx, cy, f_use)
            angs = euler_zyx_from_R(Rm)

            if angs is not None:
                yaw, pitch, roll = angs
                yaw = ((yaw + 180) % 360) - 180
                pitch = ((pitch + 180) % 360) - 180
                roll = ((roll + 180) % 360) - 180

                if not (np.isfinite(yaw) and np.isfinite(pitch) and np.isfinite(roll)):
                    yaw = pitch = roll = None
                    pose_src = "none"

                if pose_src == "global_flat":
                    pitch = 0.0
                    roll = 0.0

        metric_pose = _build_metric_pose_and_size(
            row={},
            bbox_xyxy=(x1, y1, x2, y2),
            image_w=W,
            image_h=H,
            focal_px=focal_for_metric
        )

        color = _vehicle_color_bgr(cls_name)

        label = f"{cls_name} {i}"
        if inpl is not None:
            label += f" R-pos={inpl:.1f}"
        if r_img_deg is not None and abs(float(r_img_deg)) >= 0.05:
            label += f" R-img={float(r_img_deg):.1f}"

        _draw_box_and_label(vis, x1, y1, x2, y2, label, color)

        results.append({
            "id": i,
            "bbox": [int(x1), int(y1), int(x2), int(y2)],
            "conf": round(float(conf), 3),
            "cls_id": int(cls_id),
            "cls_name": str(cls_name),

            "Pos-X": _round_or_none(metric_pose.get("Pos-X"), 3),
            "Pos-Y": _round_or_none(metric_pose.get("Pos-Y"), 3),
            "Pos-Z": _round_or_none(metric_pose.get("Pos-Z"), 3),

            "R-img": _round_or_none(r_img_deg, 3),
            "R-pos": _round_or_none(inpl, 3),

            "Rot-X": _round_or_none(pitch, 3),
            "Rot-Y": _round_or_none(roll, 3),
            "Rot-Z": _round_or_none(yaw, 3),

            "X-pos": _round_or_none(yaw, 3),
            "Y-pos": _round_or_none(pitch, 3),
            "Z-pos": _round_or_none(roll, 3),
        })

    return vis, results

UPLOADED_IMAGES = {}


def run_pipeline():
    if files is None:
        raise RuntimeError("run_pipeline() доступен только в Google Colab (files.upload). Используйте run().")
    global UPLOADED_BYTES, UPLOADED_IMAGES
    UPLOADED_BYTES = files.upload()
    UPLOADED_IMAGES = {}

    for fn, data in UPLOADED_BYTES.items():
        img, meta = read_image_auto_with_meta(data)
        if img is None:
            print(f"{fn}: не удалось прочитать файл")
            continue

        UPLOADED_IMAGES[fn] = img

        vis, rows = process_image(
            img,
            exif_orientation_deg=(meta or {}).get("exif_orientation_deg")
        )
        if vis is None:
            print(f"{fn}: ошибка обработки")
            continue

        if plt is not None:
            plt.figure(figsize=(80, 32))
            plt.imshow(cv2.cvtColor(vis, cv2.COLOR_BGR2RGB))
            plt.axis("off")
            plt.show()

        if pd is not None:
            df = pd.DataFrame(rows)
            try:
                from google.colab import data_table
                data_table.DataTable(df, include_index=False)
            except Exception:
                pass

        print(json.dumps(rows, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    run_pipeline()


def run(image_path: str, out_dir: str, cfg: dict, device_mode: str = "auto") -> dict:
    t0 = time.time()
    os.makedirs(out_dir, exist_ok=True)

    warnings = []

    global yolo, YOLO_DEVICE_ARG, ROT_KEEP_IDS, DET_CFG

    nb = (cfg.get("notebooks", {}) or {}).get("rotation_detect", {}) or {}
    model_path = str(cfg.get("yolo_model_path", "") or "").strip().strip('"').strip("'")
    if not model_path:
        model_path = str(nb.get("model_path", "") or "").strip().strip('"').strip("'")
    if not model_path:
        raise FileNotFoundError("Не задан yolo_model_path (и rotation_detect.model_path) в cfg")

    model_path = os.path.expandvars(os.path.expanduser(model_path))
    if not Path(model_path).is_absolute():
        ydir = str(cfg.get("yolo_dir", "") or "").strip()
        ydir = os.path.expandvars(os.path.expanduser(ydir))
        if ydir:
            model_path = str((Path(ydir) / model_path).resolve())
    if not Path(model_path).exists():
        raise FileNotFoundError(f"Файл весов не найден: {model_path}")

    _validate_model_path_for_cluster2(model_path)

    try:
        if getattr(yolo, "_vk_model_path", None) != model_path:
            yolo = YOLO(model_path, task="detect")
            yolo._vk_model_path = model_path
    except Exception:
        yolo = YOLO(model_path, task="detect")
        yolo._vk_model_path = model_path
    DET_CFG["max_det"] = 10000
    ROT_KEEP_IDS = None
    device_used, YOLO_DEVICE_ARG = _resolve_device(device_mode, cfg)
    if str(device_mode or "auto").lower().strip() in {"gpu", "cuda"} and device_used != "gpu":
        warnings.append("gpu_requested_but_unavailable")

    img, img_meta = read_image_path_with_meta(image_path)
    if img is None:
        raise RuntimeError("Не удалось прочитать изображение")

    exif_orientation_deg = None if img_meta is None else img_meta.get("exif_orientation_deg")

    H, W = img.shape[:2]

    vis, rows = process_image(
        img,
        exif_orientation_deg=exif_orientation_deg
    )
    if vis is None:
        vis = img.copy()
        rows = []

    annotated_path = os.path.abspath(os.path.join(out_dir, "annotated_cluster_2.jpg"))
    cv2.imwrite(annotated_path, vis)

    dets = []
    brightness = round(float(np.mean(cv2.cvtColor(img, cv2.COLOR_BGR2GRAY))), 2)

    for r in (rows or []):
        bb = r.get("bbox", None)
        if not bb or len(bb) != 4:
            continue

        x1, y1, x2, y2 = map(int, bb)
        conf = r.get("conf", 0.0)
        cls_id = int(r.get("cls_id", 2))
        cls_name = _normalized_vehicle_class(r.get("cls_name", _cls_name_from_id(cls_id)))

        meta = dict(r)
        meta.pop("bbox", None)
        meta.pop("id", None)
        meta.pop("conf", None)
        meta.pop("cls_id", None)
        meta.pop("cls_name", None)

        for k, v in list(meta.items()):
            if isinstance(v, (np.integer,)):
                meta[k] = int(v)
            elif isinstance(v, (np.floating,)):
                meta[k] = float(v)

        meta["brightness"] = float(brightness)

        dets.append({
            "bbox_xyxy": [x1, y1, x2, y2],
            "conf": float(conf) if conf is not None else 0.0,
            "cls_id": int(cls_id),
            "cls_name": str(cls_name),
            "meta": meta
        })

    csv_path = os.path.abspath(os.path.join(out_dir, "cluster_2_results.csv"))
    with open(csv_path, "w", encoding="utf-8") as f:
        f.write(
            "filename,det_idx,cls,x1,y1,x2,y2,w_px,h_px,conf,"
            "Pos-X,Pos-Y,Pos-Z,R-img,R-pos,Rot-X,Rot-Y,Rot-Z,X-pos,Y-pos,Z-pos,brightness\n"
        )

        for i, d in enumerate(dets, start=1):
            x1, y1, x2, y2 = d["bbox_xyxy"]
            meta = d.get("meta", {}) or {}

            row = [
                os.path.basename(image_path),
                str(i),
                str(d["cls_name"]),
                str(x1),
                str(y1),
                str(x2),
                str(y2),
                str(int(x2 - x1)),
                str(int(y2 - y1)),
                f"{float(d['conf']):.6f}",

                "" if meta.get("Pos-X") is None else str(meta.get("Pos-X")),
                "" if meta.get("Pos-Y") is None else str(meta.get("Pos-Y")),
                "" if meta.get("Pos-Z") is None else str(meta.get("Pos-Z")),
                "" if meta.get("R-img") is None else str(meta.get("R-img")),
                "" if meta.get("R-pos") is None else str(meta.get("R-pos")),
                "" if meta.get("Rot-X") is None else str(meta.get("Rot-X")),
                "" if meta.get("Rot-Y") is None else str(meta.get("Rot-Y")),
                "" if meta.get("Rot-Z") is None else str(meta.get("Rot-Z")),
                "" if meta.get("X-pos") is None else str(meta.get("X-pos")),
                "" if meta.get("Y-pos") is None else str(meta.get("Y-pos")),
                "" if meta.get("Z-pos") is None else str(meta.get("Z-pos")),
                str(brightness)
            ]

            f.write(",".join(row) + "\n")

    total_ms = int(round((time.time() - t0) * 1000))

    return {
        "module_id": "cluster_2",
        "image_w": int(W),
        "image_h": int(H),
        "device_used": device_used,
        "warnings": warnings,
        "annotated_image_path": annotated_path,
        "cleaned_image_path": "",
        "artifacts": {
            "csv_path": csv_path,
            "brightness": brightness,
        },
        "timings_ms": {
            "total": total_ms,
            "preprocess": 0,
            "inference": 0,
            "postprocess": 0
        },
        "detections": dets
    }