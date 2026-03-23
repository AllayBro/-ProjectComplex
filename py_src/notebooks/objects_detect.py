# -*- coding: utf-8 -*-
"""
objects_detect.py
Один вход: run(image_path, out_dir, cfg, device_mode) -> dict (ModuleResult)
"""

import os
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


def run(image_path: str, out_dir: str, cfg: dict, device_mode: str = "auto") -> dict:
    import json
    import csv

    t_all0 = perf_counter()
    timings_ms = {}
    warnings = []

    if cfg is None:
        cfg = {}

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
                if real_h is not None and h_px > 0:
                    dist_m = round((float(focal_px_y) * real_h) / float(h_px), 2)
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
        },
        "timings_ms": timings_ms,
        "detections": detections,
    }