import copy
import json
import os
import time
from dataclasses import dataclass
from typing import Any, Dict, List, Optional, Tuple

import cv2
import numpy as np
from PIL import Image, ImageOps
from ultralytics import YOLO

try:
    import torch
except Exception:
    torch = None

try:
    import pillow_heif  # type: ignore
    pillow_heif.register_heif_opener()
except Exception:
    pillow_heif = None


# ----------------------------
# Base utils
# ----------------------------

def now_ms() -> int:
    return int(time.time() * 1000)


def ensure_dir(p: str) -> None:
    os.makedirs(p, exist_ok=True)


def clamp(v: int, lo: int, hi: int) -> int:
    if v < lo:
        return lo
    if v > hi:
        return hi
    return v


def load_image_any(path: str) -> np.ndarray:
    """
    Loads image with EXIF transpose and returns BGR uint8.
    Supports most PIL formats; for JPEG/PNG also tries OpenCV fast path.
    """
    with open(path, "rb") as f:
        data = f.read()

    arr = np.frombuffer(data, np.uint8)
    img = cv2.imdecode(arr, cv2.IMREAD_COLOR)
    if img is not None:
        return img

    pil = Image.open(path)
    pil = ImageOps.exif_transpose(pil).convert("RGB")
    return cv2.cvtColor(np.array(pil), cv2.COLOR_RGB2BGR)


def safe_abs(path: str) -> str:
    return os.path.abspath(path) if path else ""


def jsonable(x: Any) -> Any:
    if isinstance(x, (np.integer,)):
        return int(x)
    if isinstance(x, (np.floating,)):
        return float(x)
    if isinstance(x, (np.ndarray,)):
        return x.tolist()
    if isinstance(x, dict):
        return {str(k): jsonable(v) for k, v in x.items()}
    if isinstance(x, list):
        return [jsonable(v) for v in x]
    if isinstance(x, tuple):
        return [jsonable(v) for v in x]
    return x


# ----------------------------
# Geometry
# ----------------------------

def iou_xyxy(a: Tuple[int, int, int, int], b: Tuple[int, int, int, int]) -> float:
    ax1, ay1, ax2, ay2 = a
    bx1, by1, bx2, by2 = b

    ix1 = max(ax1, bx1)
    iy1 = max(ay1, by1)
    ix2 = min(ax2, bx2)
    iy2 = min(ay2, by2)

    iw = max(0, ix2 - ix1)
    ih = max(0, iy2 - iy1)
    inter = iw * ih
    if inter <= 0:
        return 0.0

    area_a = max(0, ax2 - ax1) * max(0, ay2 - ay1)
    area_b = max(0, bx2 - bx1) * max(0, by2 - by1)
    denom = area_a + area_b - inter
    return float(inter) / float(denom) if denom > 0 else 0.0


def _containment_cover(big_xyxy, small_xyxy) -> float:
    bx1, by1, bx2, by2 = map(float, big_xyxy)
    sx1, sy1, sx2, sy2 = map(float, small_xyxy)

    ix1 = max(bx1, sx1)
    iy1 = max(by1, sy1)
    ix2 = min(bx2, sx2)
    iy2 = min(by2, sy2)

    iw = max(0.0, ix2 - ix1)
    ih = max(0.0, iy2 - iy1)
    inter = iw * ih
    area_s = max(0.0, sx2 - sx1) * max(0.0, sy2 - sy1)
    if area_s <= 1e-9:
        return 0.0
    return float(inter / area_s)


# ----------------------------
# Merge / NMS helpers
# ----------------------------

def union_merge_boxes(dets: List[Dict[str, Any]], iou_thr: float) -> List[Dict[str, Any]]:
    """
    Greedy union-merge by IoU within same cls_name.
    Keeps one merged det per connected component.
    """
    if not dets:
        return []

    used = [False] * len(dets)
    out: List[Dict[str, Any]] = []

    for i in range(len(dets)):
        if used[i]:
            continue

        base = dets[i]
        base_cls = base.get("cls_name")
        bx = tuple(map(int, base["bbox_xyxy"]))
        group = [i]
        used[i] = True

        changed = True
        while changed:
            changed = False
            for j in range(len(dets)):
                if used[j]:
                    continue
                if dets[j].get("cls_name") != base_cls:
                    continue
                jx = tuple(map(int, dets[j]["bbox_xyxy"]))
                if iou_xyxy(bx, jx) >= float(iou_thr):
                    group.append(j)
                    used[j] = True

                    xs: List[int] = []
                    ys: List[int] = []
                    for k in group:
                        x1, y1, x2, y2 = map(int, dets[k]["bbox_xyxy"])
                        xs += [x1, x2]
                        ys += [y1, y2]
                    bx = (min(xs), min(ys), max(xs), max(ys))
                    changed = True

        conf = max(float(dets[k].get("conf", 0.0)) for k in group)
        merged = dict(base)
        merged["bbox_xyxy"] = [int(bx[0]), int(bx[1]), int(bx[2]), int(bx[3])]
        merged["conf"] = float(conf)
        out.append(merged)

    return out


def _nms(dets: List[Dict[str, Any]], iou_thr: float) -> List[Dict[str, Any]]:
    dets_sorted = sorted(dets, key=lambda d: float(d.get("conf", 0.0)), reverse=True)
    kept: List[Dict[str, Any]] = []
    thr = float(iou_thr)

    for d in dets_sorted:
        bb = tuple(map(int, d["bbox_xyxy"]))
        cls_name = d.get("cls_name")
        ok = True
        for k in kept:
            if k.get("cls_name") != cls_name:
                continue
            if iou_xyxy(bb, tuple(map(int, k["bbox_xyxy"]))) > thr:
                ok = False
                break
        if ok:
            kept.append(d)

    return kept


def postprocess_after_merge(dets: List[Dict[str, Any]], cfg: Dict[str, Any]) -> List[Dict[str, Any]]:
    """
    Postprocess step after union merge: containment suppression + optional NMS.
    """
    pp = cfg.get("postprocess", {}) or {}
    if not bool(pp.get("enabled", True)) or not dets:
        return dets

    cs = pp.get("containment_suppression", {}) or {}
    if bool(cs.get("enabled", True)):
        cover_thr = float(cs.get("cover_thr", 0.92))
        conf_margin = float(cs.get("conf_margin", 0.02))

        ordered = sorted(dets, key=lambda d: float(d.get("conf", 0.0)), reverse=True)
        keep: List[Dict[str, Any]] = []

        for d in ordered:
            drop = False
            for k in keep:
                if k.get("cls_name") != d.get("cls_name"):
                    continue
                if (
                        _containment_cover(k["bbox_xyxy"], d["bbox_xyxy"]) >= cover_thr
                        and float(k.get("conf", 0.0)) + conf_margin >= float(d.get("conf", 0.0))
                ):
                    drop = True
                    break
            if not drop:
                keep.append(d)

        dets = keep

    nms = pp.get("nms", {}) or {}
    if bool(nms.get("enabled", False)) and dets:
        dets = _nms(dets, float(nms.get("iou", 0.60)))

    return dets


# ----------------------------
# Post-processor (filters + containment + NMS)
# ----------------------------

class PostProcessor:
    @staticmethod
    def apply(img_w: int, img_h: int, dets: List[Dict[str, Any]], cfg: Dict[str, Any]) -> List[Dict[str, Any]]:
        pp = cfg.get("postprocess", {}) or {}
        if not bool(pp.get("enabled", True)) or not dets:
            return dets

        keep = pp.get("keep_classes", None)
        if keep is None:
            keep = (cfg.get("detector", {}) or {}).get("keep_classes", [])
        keep_set = set(str(x).lower() for x in (keep or []) if str(x).strip())

        min_box_px = int(pp.get("min_box_px", (cfg.get("detector", {}) or {}).get("min_box_px", 18)))
        min_area_frac = float(pp.get("min_area_frac", (cfg.get("detector", {}) or {}).get("min_area_frac", 0.0)))
        ar_min = float(pp.get("aspect_ratio_min", (cfg.get("detector", {}) or {}).get("aspect_ratio_min", 0.25)))
        ar_max = float(pp.get("aspect_ratio_max", (cfg.get("detector", {}) or {}).get("aspect_ratio_max", 5.0)))

        exclude_top_frac = float(pp.get("exclude_top_frac", 0.0))
        exclude_bottom_frac = float(pp.get("exclude_bottom_frac", 0.0))

        adaptive = pp.get("adaptive_conf", {}) or {}
        adaptive_enabled = bool(adaptive.get("enabled", True))
        tiny_max = float(adaptive.get("tiny_max_frac", 0.02))
        small_max = float(adaptive.get("small_max_frac", 0.05))
        conf_tiny = float(adaptive.get("conf_tiny", 0.28))
        conf_small = float(adaptive.get("conf_small", 0.22))
        conf_normal = float(adaptive.get("conf_normal", 0.18))

        min_area = float(img_w * img_h) * float(min_area_frac)

        out: List[Dict[str, Any]] = []

        for d in dets:
            x1, y1, x2, y2 = map(int, d["bbox_xyxy"])
            x1 = clamp(x1, 0, img_w - 1)
            y1 = clamp(y1, 0, img_h - 1)
            x2 = clamp(x2, 0, img_w - 1)
            y2 = clamp(y2, 0, img_h - 1)
            if x2 <= x1 or y2 <= y1:
                continue

            bw = x2 - x1
            bh = y2 - y1
            if bw < min_box_px or bh < min_box_px:
                continue

            area = float(bw * bh)
            if area < min_area:
                continue

            ar = float(bw) / float(bh) if bh > 0 else 0.0
            if ar < ar_min or ar > ar_max:
                continue

            cls_name = str(d.get("cls_name", "")).lower()
            if keep_set and cls_name not in keep_set:
                continue

            if exclude_top_frac > 0.0 and y2 <= int(exclude_top_frac * img_h):
                continue
            if exclude_bottom_frac > 0.0 and y1 >= int((1.0 - exclude_bottom_frac) * img_h):
                continue

            cf = float(d.get("conf", 0.0))
            if adaptive_enabled:
                rel_h = float(bh) / float(max(1, img_h))
                if rel_h <= tiny_max and cf < conf_tiny:
                    continue
                if tiny_max < rel_h <= small_max and cf < conf_small:
                    continue
                if rel_h > small_max and cf < conf_normal:
                    continue

            nd = dict(d)
            nd["bbox_xyxy"] = [x1, y1, x2, y2]
            nd["cls_name"] = cls_name
            nd["meta"] = dict(d.get("meta", {}) or {})
            out.append(nd)

        contain = pp.get("containment", {}) or {}
        if bool(contain.get("enabled", True)) and out:
            contain_thr = float(contain.get("thr", 0.92))
            conf_margin = float(contain.get("conf_margin", 0.02))

            out_sorted = sorted(
                out,
                key=lambda d: (
                    float(d.get("conf", 0.0)),
                    (d["bbox_xyxy"][2] - d["bbox_xyxy"][0]) * (d["bbox_xyxy"][3] - d["bbox_xyxy"][1]),
                ),
                reverse=True,
            )

            kept: List[Dict[str, Any]] = []
            for d in out_sorted:
                bb = tuple(map(int, d["bbox_xyxy"]))
                drop = False
                for k in kept:
                    kb = tuple(map(int, k["bbox_xyxy"]))
                    if (
                            _containment_cover(kb, bb) >= contain_thr
                            and float(k.get("conf", 0.0)) + conf_margin >= float(d.get("conf", 0.0))
                    ):
                        drop = True
                        break
                if not drop:
                    kept.append(d)
            out = kept

        nms_cfg = pp.get("nms", {}) or {}
        if bool(nms_cfg.get("enabled", True)) and out:
            out = _nms(out, float(nms_cfg.get("iou", 0.60)))

        return out


# ----------------------------
# Device selection
# ----------------------------

@dataclass(frozen=True)
class DeviceChoice:
    device_for_ultralytics: Any
    device_used: str


class DeviceManager:
    @staticmethod
    def pick(device_mode: str, gpu_id: int, fallback_to_cpu: bool) -> DeviceChoice:
        dm = (device_mode or "auto").lower().strip()
        if dm not in ("auto", "gpu", "cpu"):
            dm = "auto"

        has_cuda = False
        if torch is not None:
            try:
                has_cuda = bool(torch.cuda.is_available())
            except Exception:
                has_cuda = False

        if dm == "cpu":
            return DeviceChoice("cpu", "cpu")

        if dm == "gpu":
            if has_cuda:
                return DeviceChoice(int(gpu_id), "gpu")
            if fallback_to_cpu:
                return DeviceChoice("cpu", "cpu")
            raise RuntimeError("GPU requested but CUDA is not available")

        if has_cuda:
            return DeviceChoice(int(gpu_id), "gpu")
        return DeviceChoice("cpu", "cpu")


# ----------------------------
# Detector
# ----------------------------

class VehicleDetector:
    def __init__(self, model_path: str):
        self.model_path = str(model_path)
        self.model = YOLO(self.model_path)

    def predict(self, img_bgr: np.ndarray, cfg: Dict[str, Any], dev: DeviceChoice) -> Tuple[List[Dict[str, Any]], Dict[str, int]]:
        det_cfg = cfg.get("detector", {}) or {}
        imgsz = int(det_cfg.get("imgsz", 960))
        conf = float(det_cfg.get("conf", 0.20))
        iou = float(det_cfg.get("iou", 0.50))
        max_det = int(det_cfg.get("max_det", 300))

        t0 = now_ms()
        res = self.model.predict(
            source=img_bgr,
            imgsz=imgsz,
            conf=conf,
            iou=iou,
            max_det=max_det,
            device=dev.device_for_ultralytics,
            verbose=False,
        )
        t1 = now_ms()

        r0 = res[0]
        names = getattr(r0, "names", {}) or {}
        dets: List[Dict[str, Any]] = []

        h, w = img_bgr.shape[:2]
        boxes = getattr(r0, "boxes", None)
        if boxes is not None and len(boxes) > 0:
            xyxy = boxes.xyxy.cpu().numpy().astype(np.int32)
            confs = boxes.conf.cpu().numpy().astype(np.float32)
            clss = boxes.cls.cpu().numpy().astype(np.int32)

            for bb, cf, ci in zip(xyxy, confs, clss):
                x1, y1, x2, y2 = map(int, bb.tolist())
                x1 = clamp(x1, 0, w - 1)
                y1 = clamp(y1, 0, h - 1)
                x2 = clamp(x2, 0, w - 1)
                y2 = clamp(y2, 0, h - 1)
                if x2 <= x1 or y2 <= y1:
                    continue
                cls_name = str(names.get(int(ci), str(ci))).lower()
                dets.append(
                    {
                        "bbox_xyxy": [x1, y1, x2, y2],
                        "conf": float(cf),
                        "cls_id": int(ci),
                        "cls_name": cls_name,
                        "meta": {},
                    }
                )

        timings = {"inference_ms": int(t1 - t0)}
        return dets, timings


# ----------------------------
# Rotation (ROI Hough)
# ----------------------------

class RotationEstimator:
    def estimate(self, img_bgr: np.ndarray, det: Dict[str, Any], cfg: Dict[str, Any]) -> Dict[str, Any]:
        rot_cfg = cfg.get("rotation", {}) or {}
        if not bool(rot_cfg.get("enabled", True)):
            return det

        x1, y1, x2, y2 = map(int, det["bbox_xyxy"])
        roi = img_bgr[y1:y2, x1:x2]
        if roi is None or roi.size == 0:
            return det

        g = cv2.cvtColor(roi, cv2.COLOR_BGR2GRAY)
        e = cv2.Canny(
            g,
            int(rot_cfg.get("canny1", 60)),
            int(rot_cfg.get("canny2", 180)),
        )

        lines = cv2.HoughLinesP(
            e,
            1,
            np.pi / 180.0,
            threshold=int(rot_cfg.get("hough_threshold", 60)),
            minLineLength=int(rot_cfg.get("min_line_length", 40)),
            maxLineGap=int(rot_cfg.get("max_line_gap", 10)),
            )

        angs: List[float] = []
        if lines is not None:
            for l in lines[:, 0, :]:
                x1l, y1l, x2l, y2l = map(int, l.tolist())
                dx = x2l - x1l
                dy = y2l - y1l
                if dx == 0 and dy == 0:
                    continue
                a = float(np.degrees(np.arctan2(dy, dx)))
                if a < -90:
                    a += 180
                if a > 90:
                    a -= 180
                angs.append(a)

        r_pos = float(np.median(angs)) if angs else float(rot_cfg.get("fallback_deg", 0.0))

        meta = dict(det.get("meta", {}) or {})
        meta["r_pos"] = float(r_pos)
        meta["rot_x"] = float(meta.get("rot_x", 0.0))
        meta["rot_y"] = float(meta.get("rot_y", 0.0))
        meta["rot_z"] = float(r_pos)
        meta["pose_src"] = str(rot_cfg.get("pose_src", "hough_roi"))

        out = dict(det)
        out["meta"] = meta
        return out


# ----------------------------
# Cluster refine via ROI re-detect
# ----------------------------

class ClusterRefiner:
    def __init__(self, detector: VehicleDetector):
        self.detector = detector

    @staticmethod
    def _override_detector_cfg(cfg: Dict[str, Any], cr: Dict[str, Any]) -> Dict[str, Any]:
        cfg2 = copy.deepcopy(cfg)
        cfg2.setdefault("detector", {})

        if cr.get("refine_imgsz", None) is not None:
            cfg2["detector"]["imgsz"] = int(cr["refine_imgsz"])
        if cr.get("refine_conf", None) is not None:
            cfg2["detector"]["conf"] = float(cr["refine_conf"])
        if cr.get("refine_iou", None) is not None:
            cfg2["detector"]["iou"] = float(cr["refine_iou"])
        if cr.get("refine_max_det", None) is not None:
            cfg2["detector"]["max_det"] = int(cr["refine_max_det"])

        return cfg2

    def refine(
            self,
            img_bgr: np.ndarray,
            dets: List[Dict[str, Any]],
            cfg: Dict[str, Any],
            dev: DeviceChoice,
    ) -> Tuple[List[Dict[str, Any]], Dict[str, Any]]:
        cr = cfg.get("cluster_refine", {}) or {}
        if not bool(cr.get("enabled", True)) or not dets:
            out: List[Dict[str, Any]] = []
            for d in dets:
                nd = dict(d)
                nd["meta"] = dict(d.get("meta", {}) or {})
                nd["meta"]["refined"] = False
                out.append(nd)
            return out, {"refine_total_ms": 0}

        h, w = img_bgr.shape[:2]

        roi_scale = float(cr.get("roi_scale", 2.0))
        roi_min = int(cr.get("roi_min_size", 640))
        min_keep_iou = float(cr.get("min_keep_iou", 0.12))

        cfg2 = self._override_detector_cfg(cfg, cr)

        refined: List[Dict[str, Any]] = []
        t_total = 0

        for d in dets:
            x1, y1, x2, y2 = map(int, d["bbox_xyxy"])
            bw = max(1, x2 - x1)
            bh = max(1, y2 - y1)

            cx = (x1 + x2) // 2
            cy = (y1 + y2) // 2

            rw = int(max(roi_min, float(bw) * roi_scale))
            rh = int(max(roi_min, float(bh) * roi_scale))

            rx1 = clamp(cx - rw // 2, 0, w - 1)
            ry1 = clamp(cy - rh // 2, 0, h - 1)
            rx2 = clamp(cx + rw // 2, 0, w - 1)
            ry2 = clamp(cy + rh // 2, 0, h - 1)

            roi = img_bgr[ry1:ry2, rx1:rx2]
            if roi is None or roi.size == 0:
                nd = dict(d)
                nd["meta"] = dict(d.get("meta", {}) or {})
                nd["meta"]["refined"] = False
                refined.append(nd)
                continue

            t0 = now_ms()
            roi_dets, _ = self.detector.predict(roi, cfg2, dev)
            t1 = now_ms()
            t_total += int(t1 - t0)

            base_box = (x1, y1, x2, y2)

            kept: List[Tuple[int, int, int, int]] = []
            for rd in roi_dets:
                rbx1, rby1, rbx2, rby2 = map(int, rd["bbox_xyxy"])
                ob = (rbx1 + rx1, rby1 + ry1, rbx2 + rx1, rby2 + ry1)
                if iou_xyxy(base_box, ob) >= min_keep_iou:
                    kept.append(ob)

            if not kept:
                nd = dict(d)
                nd["meta"] = dict(d.get("meta", {}) or {})
                nd["meta"]["refined"] = False
                refined.append(nd)
                continue

            xs = [b[0] for b in kept] + [b[2] for b in kept]
            ys = [b[1] for b in kept] + [b[3] for b in kept]
            nx1, ny1, nx2, ny2 = int(min(xs)), int(min(ys)), int(max(xs)), int(max(ys))

            nd = dict(d)
            nd["bbox_xyxy"] = [
                clamp(nx1, 0, w - 1),
                clamp(ny1, 0, h - 1),
                clamp(nx2, 0, w - 1),
                clamp(ny2, 0, h - 1),
            ]
            nd["meta"] = dict(d.get("meta", {}) or {})
            nd["meta"]["refined"] = True
            refined.append(nd)

        return refined, {"refine_total_ms": int(t_total)}


# ----------------------------
# Distance
# ----------------------------

class DistanceEstimator:
    def estimate(self, img_bgr: np.ndarray, det: Dict[str, Any], cfg: Dict[str, Any]) -> Dict[str, Any]:
        dist_cfg = cfg.get("distance", {}) or {}
        if not bool(dist_cfg.get("enabled", True)):
            return det

        h, w = img_bgr.shape[:2]

        focal_px_override = dist_cfg.get("focal_px_override", None)
        if focal_px_override is not None:
            focal_px = float(focal_px_override)
        else:
            focal_mm = float(dist_cfg.get("focal_mm", 4.25))
            sensor_w_mm = float(dist_cfg.get("sensor_width_mm", 5.6))
            focal_px = (focal_mm / max(sensor_w_mm, 1e-9)) * float(w)

        cls_name = str(det.get("cls_name", "car")).lower()
        class_height = dist_cfg.get("class_height_m", {}) or {}
        real_h = float(class_height.get(cls_name, dist_cfg.get("default_height_m", 1.5)))

        x1, y1, x2, y2 = map(int, det["bbox_xyxy"])
        bh = max(1, int(y2 - y1))
        dist_m = (real_h * focal_px) / float(bh)

        meta = dict(det.get("meta", {}) or {})
        meta["distance_m"] = float(dist_m)
        meta["focal_px"] = float(focal_px)
        meta["real_height_m"] = float(real_h)

        out = dict(det)
        out["meta"] = meta
        return out


# ----------------------------
# Cleanup (keep only detections)
# ----------------------------

class Cleanup:
    @staticmethod
    def cleanup_image(img_bgr: np.ndarray, dets: List[Dict[str, Any]], cfg: Dict[str, Any]) -> np.ndarray:
        cc = cfg.get("cleanup", {}) or {}
        if not bool(cc.get("enabled", True)) or not dets:
            return img_bgr.copy()

        h, w = img_bgr.shape[:2]
        out = np.zeros((h, w, 3), dtype=np.uint8)

        pad_frac = float(cc.get("pad_frac", 0.0))
        for d in dets:
            x1, y1, x2, y2 = map(int, d["bbox_xyxy"])
            bw = max(1, x2 - x1)
            bh = max(1, y2 - y1)
            px = int(round(pad_frac * bw))
            py = int(round(pad_frac * bh))

            xx1 = clamp(x1 - px, 0, w - 1)
            yy1 = clamp(y1 - py, 0, h - 1)
            xx2 = clamp(x2 + px, 0, w - 1)
            yy2 = clamp(y2 + py, 0, h - 1)

            if xx2 <= xx1 or yy2 <= yy1:
                continue
            out[yy1:yy2, xx1:xx2] = img_bgr[yy1:yy2, xx1:xx2]

        return out


# ----------------------------
# Visualization + CSV
# ----------------------------

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


def _estimate_brightness_from_path(image_path: str):
    try:
        img = load_image_any(image_path)
        gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
        return round(float(np.mean(gray)), 2)
    except Exception:
        return ""

def _vehicle_color_bgr(cls_name: str):
    cls = str(cls_name or "").strip().lower()
    if cls == "truck":
        return (0, 0, 255)
    if cls == "bus":
        return (255, 0, 0)
    if cls == "motorcycle":
        return (0, 0, 0)
    return (0, 255, 0)

def draw_annotated(img_bgr: np.ndarray, dets: List[Dict[str, Any]]) -> np.ndarray:
    out = img_bgr.copy()

    for i, d in enumerate(dets, start=1):
        x1, y1, x2, y2 = map(int, d["bbox_xyxy"])
        cls_name = str(d.get("cls_name", "")).strip().lower()
        meta = d.get("meta", {}) or {}

        label = f"{cls_name} {i}"
        if meta.get("distance_m") is not None:
            label += f" ~{float(meta['distance_m']):.2f}m"
        elif meta.get("dist_m") is not None:
            label += f" ~{float(meta['dist_m']):.2f}m"
        elif meta.get("r_pos") is not None:
            label += f" R-pos={float(meta['r_pos']):.1f}"

        color = _vehicle_color_bgr(cls_name)
        _draw_box_and_label(out, x1, y1, x2, y2, label, color)

    return out

def write_csv_distance(path: str, image_path: str, dets: List[Dict[str, Any]], device_used: str, timings_ms: Dict[str, Any]) -> None:
    header = "filename,det_idx,cls,x1,y1,x2,y2,w_px,h_px,conf,distance_m,brightness,device,inference_ms,total_ms\n"
    base = os.path.basename(image_path)

    inf_ms = int((timings_ms or {}).get("inference", 0))
    tot_ms = int((timings_ms or {}).get("total", 0))
    brightness = _estimate_brightness_from_path(image_path)

    lines = [header]
    for i, d in enumerate(dets, start=1):
        x1, y1, x2, y2 = map(int, d["bbox_xyxy"])
        bw = max(0, x2 - x1)
        bh = max(0, y2 - y1)

        cls_name = _normalized_vehicle_class(d.get("cls_name", "vehicle"))
        conf = float(d.get("conf", 0.0))

        meta = d.get("meta", {}) or {}
        dist = ""
        if meta.get("distance_m") is not None:
            dist = f"{float(meta['distance_m']):.6f}"
        elif meta.get("dist_m") is not None:
            dist = f"{float(meta['dist_m']):.6f}"

        lines.append(
            f"{base},{i},{cls_name},{x1},{y1},{x2},{y2},{bw},{bh},{conf:.6f},{dist},{brightness},{device_used},{inf_ms},{tot_ms}\n"
        )

    with open(path, "w", encoding="utf-8") as f:
        f.writelines(lines)

# ----------------------------
# Full pipeline run
# ----------------------------

def default_cfg() -> Dict[str, Any]:
    return {
        "module_id": "cluster_pipeline",
        "device": {"gpu_id": 0, "fallback_to_cpu": True},
        "detector": {
            "model_path": "yolo11n.pt",
            "imgsz": 960,
            "conf": 0.20,
            "iou": 0.50,
            "max_det": 300,
            "keep_classes": [],
            "min_box_px": 18,
            "min_area_frac": 0.0,
            "aspect_ratio_min": 0.25,
            "aspect_ratio_max": 5.0,
        },
        "union_merge": {"enabled": True, "iou": 0.55},
        "postprocess": {
            "enabled": True,
            "keep_classes": None,
            "min_box_px": None,
            "min_area_frac": None,
            "aspect_ratio_min": None,
            "aspect_ratio_max": None,
            "exclude_top_frac": 0.0,
            "exclude_bottom_frac": 0.0,
            "adaptive_conf": {
                "enabled": True,
                "tiny_max_frac": 0.02,
                "small_max_frac": 0.05,
                "conf_tiny": 0.28,
                "conf_small": 0.22,
                "conf_normal": 0.18,
            },
            "containment_suppression": {"enabled": True, "cover_thr": 0.92, "conf_margin": 0.02},
            "containment": {"enabled": True, "thr": 0.92, "conf_margin": 0.02},
            "nms": {"enabled": True, "iou": 0.60},
        },
        "rotation": {
            "enabled": True,
            "canny1": 60,
            "canny2": 180,
            "hough_threshold": 60,
            "min_line_length": 40,
            "max_line_gap": 10,
            "fallback_deg": 0.0,
            "pose_src": "hough_roi",
        },
        "cluster_refine": {
            "enabled": True,
            "roi_scale": 2.0,
            "roi_min_size": 640,
            "min_keep_iou": 0.12,
            "refine_imgsz": None,
            "refine_conf": None,
            "refine_iou": None,
            "refine_max_det": None,
        },
        "distance": {
            "enabled": True,
            "focal_px_override": None,
            "focal_mm": 4.25,
            "sensor_width_mm": 5.6,
            "default_height_m": 1.5,
            "class_height_m": {},
            "csv": {"enabled": True, "filename": "distance.csv"},
        },
        "cleanup": {"enabled": False, "pad_frac": 0.0, "filename": "cleaned.jpg"},
        "artifacts": {"annotated_filename": "annotated.jpg", "result_json": "result.json"},
    }


def merge_cfg(base: Dict[str, Any], override: Optional[Dict[str, Any]]) -> Dict[str, Any]:
    if not override:
        return base

    def rec(dst: Dict[str, Any], src: Dict[str, Any]) -> Dict[str, Any]:
        for k, v in src.items():
            if isinstance(v, dict) and isinstance(dst.get(k), dict):
                dst[k] = rec(dict(dst[k]), v)
            else:
                dst[k] = v
        return dst

    return rec(copy.deepcopy(base), override)


def run(
        image_path: str,
        out_dir: str,
        cfg: Dict[str, Any],
        device_mode: str = "auto",
        state: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    ensure_dir(out_dir)

    t_total0 = now_ms()

    cfg_eff = merge_cfg(default_cfg(), cfg or {})
    module_id = str(cfg_eff.get("module_id", "cluster_pipeline"))

    dev_cfg = cfg_eff.get("device", {}) or {}
    dev = DeviceManager.pick(
        device_mode=device_mode,
        gpu_id=int(dev_cfg.get("gpu_id", 0)),
        fallback_to_cpu=bool(dev_cfg.get("fallback_to_cpu", True)),
    )

    warnings: List[str] = []
    if (device_mode or "auto") == "gpu" and dev.device_used != "gpu":
        warnings.append("gpu_requested_but_unavailable")

    img = load_image_any(image_path)
    h, w = img.shape[:2]

    det_cfg = cfg_eff.get("detector", {}) or {}
    model_path = str(det_cfg.get("model_path", "yolo11n.pt"))
    detector = VehicleDetector(model_path)

    t_inf0 = now_ms()
    dets, tms = detector.predict(img, cfg_eff, dev)
    t_inf1 = now_ms()

    if bool((cfg_eff.get("union_merge", {}) or {}).get("enabled", True)) and dets:
        dets = union_merge_boxes(dets, float((cfg_eff.get("union_merge", {}) or {}).get("iou", 0.55)))
        dets = postprocess_after_merge(dets, cfg_eff)

    dets = PostProcessor.apply(int(w), int(h), dets, cfg_eff)

    rot = RotationEstimator()
    if bool((cfg_eff.get("rotation", {}) or {}).get("enabled", True)) and dets:
        dets = [rot.estimate(img, d, cfg_eff) for d in dets]

    refiner = ClusterRefiner(detector)
    dets, refine_info = refiner.refine(img, dets, cfg_eff, dev)

    dist = DistanceEstimator()
    if bool((cfg_eff.get("distance", {}) or {}).get("enabled", True)) and dets:
        dets = [dist.estimate(img, d, cfg_eff) for d in dets]

    cleaned_path = ""
    if bool((cfg_eff.get("cleanup", {}) or {}).get("enabled", False)):
        cleaned = Cleanup.cleanup_image(img, dets, cfg_eff)
        cleaned_name = str((cfg_eff.get("cleanup", {}) or {}).get("filename", "cleaned.jpg"))
        cleaned_path = safe_abs(os.path.join(out_dir, cleaned_name))
        cv2.imwrite(cleaned_path, cleaned)

    annotated = draw_annotated(img, dets)
    ann_name = str((cfg_eff.get("artifacts", {}) or {}).get("annotated_filename", "annotated.jpg"))
    annotated_path = safe_abs(os.path.join(out_dir, ann_name))
    cv2.imwrite(annotated_path, annotated)

    artifacts: Dict[str, Any] = {}
    if bool(((cfg_eff.get("distance", {}) or {}).get("csv", {}) or {}).get("enabled", True)):
        csv_name = str((((cfg_eff.get("distance", {}) or {}).get("csv", {}) or {}).get("filename", "distance.csv")))
        csv_path = safe_abs(os.path.join(out_dir, csv_name))
        t_total1 = now_ms()
        timings_ms = {
            "total": int(t_total1 - t_total0),
            "preprocess": 0,
            "inference": int(tms.get("inference_ms", int(t_inf1 - t_inf0))),
            "postprocess": 0,
        }
        write_csv_distance(csv_path, image_path, dets, dev.device_used, timings_ms)
        artifacts["csv_path"] = csv_path

    if refine_info:
        artifacts["refine"] = jsonable(refine_info)

    result = {
        "module_id": module_id,
        "image_w": int(w),
        "image_h": int(h),
        "device_used": dev.device_used,
        "warnings": warnings,
        "annotated_image_path": annotated_path,
        "cleaned_image_path": cleaned_path,
        "artifacts": jsonable(artifacts),
        "timings_ms": {
            "total": int(now_ms() - t_total0),
            "preprocess": 0,
            "inference": int(tms.get("inference_ms", int(t_inf1 - t_inf0))),
            "postprocess": 0,
        },
        "detections": jsonable(dets),
    }

    res_json_name = str((cfg_eff.get("artifacts", {}) or {}).get("result_json", "result.json"))
    res_json_path = safe_abs(os.path.join(out_dir, res_json_name))
    with open(res_json_path, "w", encoding="utf-8") as f:
        json.dump(result, f, ensure_ascii=False, indent=2)

    result["artifacts"]["result_json_path"] = res_json_path
    return result