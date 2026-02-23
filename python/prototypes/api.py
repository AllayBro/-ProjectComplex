import json
import os
import time
from dataclasses import dataclass
from typing import Any, Dict, List, Tuple, Optional

import numpy as np
import cv2
from PIL import Image, ImageOps
from ultralytics import YOLO

try:
    import torch
except Exception:
    torch = None


def now_ms() -> int:
    return int(time.time() * 1000)


def ensure_dir(p: str) -> None:
    os.makedirs(p, exist_ok=True)


def load_image_any(path: str) -> np.ndarray:
    img = Image.open(path)
    img = ImageOps.exif_transpose(img)
    img = img.convert("RGB")
    return cv2.cvtColor(np.array(img), cv2.COLOR_RGB2BGR)


def clamp(v: int, lo: int, hi: int) -> int:
    return max(lo, min(hi, v))


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


def union_merge_boxes(dets: List[Dict[str, Any]], iou_thr: float) -> List[Dict[str, Any]]:
    used = [False] * len(dets)
    out: List[Dict[str, Any]] = []
    for i in range(len(dets)):
        if used[i]:
            continue
        base = dets[i]
        bx = tuple(base["bbox_xyxy"])
        group = [i]
        used[i] = True

        changed = True
        while changed:
            changed = False
            for j in range(len(dets)):
                if used[j]:
                    continue
                if dets[j].get("cls_name") != base.get("cls_name"):
                    continue
                jx = tuple(dets[j]["bbox_xyxy"])
                if iou_xyxy(bx, jx) >= float(iou_thr):
                    group.append(j)
                    used[j] = True
                    xs: List[int] = []
                    ys: List[int] = []
                    for k in group:
                        x1, y1, x2, y2 = dets[k]["bbox_xyxy"]
                        xs += [int(x1), int(x2)]
                        ys += [int(y1), int(y2)]
                    bx = (min(xs), min(ys), max(xs), max(ys))
                    changed = True

        conf = max(float(dets[k].get("conf", 0.0)) for k in group)
        merged = dict(base)
        merged["bbox_xyxy"] = [int(bx[0]), int(bx[1]), int(bx[2]), int(bx[3])]
        merged["conf"] = float(conf)
        out.append(merged)
    return out


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


def postprocess_after_merge(dets, cfg):
    pp = cfg.get("postprocess", {}) or {}
    if not bool(pp.get("enabled", True)) or not dets:
        return dets

    # 1) подавление "вложенных" боксов (частая причина мусора/дублей после merge)
    cs = pp.get("containment_suppression", {}) or {}
    if bool(cs.get("enabled", True)):
        cover_thr = float(cs.get("cover_thr", 0.92))
        conf_margin = float(cs.get("conf_margin", 0.02))

        ordered = sorted(dets, key=lambda d: float(d.get("conf", 0.0)), reverse=True)
        keep = []
        for d in ordered:
            drop = False
            for k in keep:
                if k.get("cls_name") != d.get("cls_name"):
                    continue
                if _containment_cover(k["bbox_xyxy"], d["bbox_xyxy"]) >= cover_thr and \
                        float(k.get("conf", 0.0)) + conf_margin >= float(d.get("conf", 0.0)):
                    drop = True
                    break
            if not drop:
                keep.append(d)
        dets = keep

    # 2) (опционально) обычный NMS после merge/containment
    nms = pp.get("nms", {}) or {}
    if bool(nms.get("enabled", False)) and dets:
        thr = float(nms.get("iou", 0.60))
        dets = sorted(dets, key=lambda d: float(d.get("conf", 0.0)), reverse=True)
        out = []
        for d in dets:
            ok = True
            for k in out:
                if k.get("cls_name") != d.get("cls_name"):
                    continue
                if iou_xyxy(tuple(k["bbox_xyxy"]), tuple(d["bbox_xyxy"])) >= thr:
                    ok = False
                    break
            if ok:
                out.append(d)
        dets = out

    return dets


def _nms(dets: List[Dict[str, Any]], iou_thr: float) -> List[Dict[str, Any]]:
    dets_sorted = sorted(dets, key=lambda d: float(d.get("conf", 0.0)), reverse=True)
    kept: List[Dict[str, Any]] = []
    for d in dets_sorted:
        bb = tuple(d["bbox_xyxy"])
        ok = True
        for k in kept:
            if iou_xyxy(bb, tuple(k["bbox_xyxy"])) > float(iou_thr):
                ok = False
                break
        if ok:
            kept.append(d)
    return kept


class PostProcessor:
    @staticmethod
    def apply(img_w: int, img_h: int, dets: List[Dict[str, Any]], cfg: Dict[str, Any]) -> List[Dict[str, Any]]:
        pp = cfg.get("postprocess", {})
        if not bool(pp.get("enabled", True)):
            return dets

        keep = pp.get("keep_classes", None)
        if keep is None:
            keep = cfg.get("detector", {}).get("keep_classes", [])
        keep_set = set([str(x).lower() for x in (keep or [])])

        min_box_px = int(pp.get("min_box_px", cfg.get("detector", {}).get("min_box_px", 18)))
        min_area_frac = float(pp.get("min_area_frac", cfg.get("detector", {}).get("min_area_frac", 0.0)))
        ar_min = float(pp.get("aspect_ratio_min", cfg.get("detector", {}).get("aspect_ratio_min", 0.25)))
        ar_max = float(pp.get("aspect_ratio_max", cfg.get("detector", {}).get("aspect_ratio_max", 5.0)))

        exclude_top_frac = float(pp.get("exclude_top_frac", 0.0))
        exclude_bottom_frac = float(pp.get("exclude_bottom_frac", 0.0))

        adaptive = pp.get("adaptive_conf", {})
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

            if exclude_top_frac > 0.0:
                if y2 <= int(exclude_top_frac * img_h):
                    continue
            if exclude_bottom_frac > 0.0:
                if y1 >= int((1.0 - exclude_bottom_frac) * img_h):
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

        contain = pp.get("containment", {})
        if bool(contain.get("enabled", True)) and out:
            contain_thr = float(contain.get("thr", 0.92))
            conf_margin = float(contain.get("conf_margin", 0.02))
            out_sorted = sorted(out, key=lambda d: (float(d.get("conf", 0.0)), (d["bbox_xyxy"][2]-d["bbox_xyxy"][0])*(d["bbox_xyxy"][3]-d["bbox_xyxy"][1])), reverse=True)
            kept: List[Dict[str, Any]] = []
            for d in out_sorted:
                bb = tuple(d["bbox_xyxy"])
                drop = False
                for k in kept:
                    kb = tuple(k["bbox_xyxy"])
                    if _containment_cover(kb, bb) >= contain_thr and float(k.get("conf", 0.0)) + conf_margin >= float(d.get("conf", 0.0)):
                        drop = True
                        break
                if not drop:
                    kept.append(d)
            out = kept

        nms_cfg = pp.get("nms", {})
        if bool(nms_cfg.get("enabled", True)) and out:
            out = _nms(out, float(nms_cfg.get("iou", 0.60)))

        return out


@dataclass
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


class VehicleDetector:
    def __init__(self, model_path: str):
        self.model_path = str(model_path)
        self.model = YOLO(self.model_path)

    def predict(self, img_bgr: np.ndarray, cfg: Dict[str, Any], dev: DeviceChoice) -> Tuple[List[Dict[str, Any]], Dict[str, int]]:
        det_cfg = cfg.get("detector", {})
        imgsz = det_cfg.get("imgsz", 960)
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
            verbose=False
        )
        t1 = now_ms()

        r0 = res[0]
        names = getattr(r0, "names", {}) or {}
        dets: List[Dict[str, Any]] = []

        h, w = img_bgr.shape[:2]
        if getattr(r0, "boxes", None) is not None and len(r0.boxes) > 0:
            xyxy = r0.boxes.xyxy.cpu().numpy().astype(np.int32)
            confs = r0.boxes.conf.cpu().numpy().astype(np.float32)
            clss = r0.boxes.cls.cpu().numpy().astype(np.int32)

            for bb, cf, ci in zip(xyxy, confs, clss):
                x1, y1, x2, y2 = map(int, bb.tolist())
                x1 = clamp(x1, 0, w - 1)
                y1 = clamp(y1, 0, h - 1)
                x2 = clamp(x2, 0, w - 1)
                y2 = clamp(y2, 0, h - 1)
                if x2 <= x1 or y2 <= y1:
                    continue
                cls_name = str(names.get(int(ci), str(ci))).lower()
                dets.append({
                    "bbox_xyxy": [x1, y1, x2, y2],
                    "conf": float(cf),
                    "cls_id": int(ci),
                    "cls_name": cls_name,
                    "meta": {}
                })

        timings = {"inference_ms": int(t1 - t0)}
        return dets, timings


class RotationEstimator:
    def estimate(self, img_bgr: np.ndarray, det: Dict[str, Any], cfg: Dict[str, Any]) -> Dict[str, Any]:
        rot_cfg = cfg.get("rotation", {})
        if not bool(rot_cfg.get("enabled", True)):
            return det

        x1, y1, x2, y2 = det["bbox_xyxy"]
        roi = img_bgr[y1:y2, x1:x2]
        if roi.size == 0:
            return det

        g = cv2.cvtColor(roi, cv2.COLOR_BGR2GRAY)
        e = cv2.Canny(g, int(rot_cfg.get("canny1", 60)), int(rot_cfg.get("canny2", 180)))

        lines = cv2.HoughLinesP(
            e, 1, np.pi / 180.0,
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

        r_pos = float(np.median(angs)) if len(angs) > 0 else 0.0

        meta = dict(det.get("meta", {}) or {})
        meta["r_pos"] = float(r_pos)
        meta["rot_x"] = float(meta.get("rot_x", 0.0))
        meta["rot_y"] = float(meta.get("rot_y", 0.0))
        meta["rot_z"] = float(r_pos)
        meta["pose_src"] = "hough_roi"

        out = dict(det)
        out["meta"] = meta
        return out


class ClusterRefiner:
    def __init__(self, detector: VehicleDetector):
        self.detector = detector

    def refine(self, img_bgr: np.ndarray, dets: List[Dict[str, Any]], cfg: Dict[str, Any], dev: DeviceChoice) -> Tuple[List[Dict[str, Any]], Dict[str, Any]]:
        cr = cfg.get("cluster_refine", {})
        if not bool(cr.get("enabled", True)):
            out = []
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

        refine_imgsz = cr.get("refine_imgsz", None)
        refine_conf = cr.get("refine_conf", None)
        refine_iou = cr.get("refine_iou", None)

        cfg2 = json.loads(json.dumps(cfg))
        cfg2.setdefault("detector", {})
        if refine_imgsz is not None:
            cfg2["detector"]["imgsz"] = refine_imgsz
        if refine_conf is not None:
            cfg2["detector"]["conf"] = refine_conf
        if refine_iou is not None:
            cfg2["detector"]["iou"] = refine_iou

        refined: List[Dict[str, Any]] = []
        t_total = 0

        for d in dets:
            x1, y1, x2, y2 = d["bbox_xyxy"]
            bw = max(1, x2 - x1)
            bh = max(1, y2 - y1)

            cx = (x1 + x2) // 2
            cy = (y1 + y2) // 2
            rw = int(max(roi_min, bw * roi_scale))
            rh = int(max(roi_min, bh * roi_scale))

            rx1 = clamp(cx - rw // 2, 0, w - 1)
            ry1 = clamp(cy - rh // 2, 0, h - 1)
            rx2 = clamp(cx + rw // 2, 0, w - 1)
            ry2 = clamp(cy + rh // 2, 0, h - 1)

            roi = img_bgr[ry1:ry2, rx1:rx2]
            if roi.size == 0:
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
                rbx1, rby1, rbx2, rby2 = rd["bbox_xyxy"]
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
                clamp(ny2, 0, h - 1)
            ]
            nd["meta"] = dict(d.get("meta", {}) or {})
            nd["meta"]["refined"] = True
            refined.append(nd)

        return refined, {"refine_total_ms": int(t_total)}


class DistanceEstimator:
    def estimate(self, img_bgr: np.ndarray, det: Dict[str, Any], cfg: Dict[str, Any]) -> Dict[str, Any]:
        dist_cfg = cfg.get("distance", {})
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
        class_height = dist_cfg.get("class_height_m", {})
        real_h = float(class_height.get(cls_name, dist_cfg.get("default_height_m", 1.5)))

        x1, y1, x2, y2 = det["bbox_xyxy"]
        bh = max(1, int(y2 - y1))
        dist_m = (real_h * focal_px) / float(bh)

        meta = dict(det.get("meta", {}) or {})
        meta["distance_m"] = float(dist_m)
        meta["focal_px"] = float(focal_px)
        meta["real_height_m"] = float(real_h)

        out = dict(det)
        out["meta"] = meta
        return out


class Cleanup:
    @staticmethod
    def cleanup_image(img_bgr: np.ndarray, dets: List[Dict[str, Any]], cfg: Dict[str, Any]) -> np.ndarray:
        cc = cfg.get("cleanup", {})
        if not bool(cc.get("enabled", True)):
            return img_bgr.copy()

        h, w = img_bgr.shape[:2]
        out = np.zeros((h, w, 3), dtype=np.uint8)

        pad_frac = float(cc.get("pad_frac", 0.0))
        for d in dets:
            x1, y1, x2, y2 = d["bbox_xyxy"]
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


def draw_annotated(img_bgr: np.ndarray, dets: List[Dict[str, Any]]) -> np.ndarray:
    out = img_bgr.copy()
    for i, d in enumerate(dets, start=1):
        x1, y1, x2, y2 = d["bbox_xyxy"]
        cls_name = d.get("cls_name", "")
        conf = float(d.get("conf", 0.0))

        label = f"{i}:{cls_name} {conf:.2f}"
        meta = d.get("meta", {}) or {}
        if "distance_m" in meta:
            label += f" {float(meta['distance_m']):.1f}m"
        if "r_pos" in meta:
            label += f" r={float(meta['r_pos']):.1f}"
        if "refined" in meta and bool(meta["refined"]):
            label += " refined"

        cv2.rectangle(out, (x1, y1), (x2, y2), (0, 220, 0), 2)
        cv2.putText(out, label, (x1, max(0, y1 - 7)), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 0, 0), 4, cv2.LINE_AA)
        cv2.putText(out, label, (x1, max(0, y1 - 7)), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 220, 0), 2, cv2.LINE_AA)
    return out


def write_csv_distance(path: str, image_path: str, dets: List[Dict[str, Any]], device_used: str, timings_ms: Dict[str, Any]) -> None:
    header = "filename,det_idx,cls,x1,y1,x2,y2,w_px,h_px,conf,distance_m,device,inference_ms,total_ms\n"
    base = os.path.basename(image_path)

    inf_ms = int(timings_ms.get("inference", 0))
    tot_ms = int(timings_ms.get("total", 0))

    lines = [header]
    for i, d in enumerate(dets, start=1):
        x1, y1, x2, y2 = d["bbox_xyxy"]
        bw = max(0, x2 - x1)
        bh = max(0, y2 - y1)
        cls_name = d.get("cls_name", "")
        conf = float(d.get("conf", 0.0))
        meta = d.get("meta", {}) or {}
        dist = ""
        if "distance_m" in meta:
            dist = f"{float(meta['distance_m']):.6f}"
        lines.append(f"{base},{i},{cls_name},{x1},{y1},{x2},{y2},{bw},{bh},{conf:.6f},{dist},{device_used},{inf_ms},{tot_ms}\n")

    with open(path, "w", encoding="utf-8") as f:
        f.writelines(lines)