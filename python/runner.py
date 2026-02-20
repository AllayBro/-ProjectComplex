import argparse
import json
import os
import time
from dataclasses import dataclass
from typing import Any, Dict, List, Tuple

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


def read_json(path: str) -> Dict[str, Any]:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def write_json(path: str, obj: Dict[str, Any]) -> None:
    with open(path, "w", encoding="utf-8") as f:
        json.dump(obj, f, ensure_ascii=False, indent=2)


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
    inter_x1 = max(ax1, bx1)
    inter_y1 = max(ay1, by1)
    inter_x2 = min(ax2, bx2)
    inter_y2 = min(ay2, by2)
    iw = max(0, inter_x2 - inter_x1)
    ih = max(0, inter_y2 - inter_y1)
    inter = iw * ih
    if inter == 0:
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
                if dets[j]["cls_name"] != base["cls_name"]:
                    continue
                jx = tuple(dets[j]["bbox_xyxy"])
                if iou_xyxy(bx, jx) >= iou_thr:
                    group.append(j)
                    used[j] = True
                    xs = []
                    ys = []
                    for k in group:
                        x1, y1, x2, y2 = dets[k]["bbox_xyxy"]
                        xs += [x1, x2]
                        ys += [y1, y2]
                    bx = (min(xs), min(ys), max(xs), max(ys))
                    changed = True

        conf = max(dets[k]["conf"] for k in group)
        merged = dict(base)
        merged["bbox_xyxy"] = [int(bx[0]), int(bx[1]), int(bx[2]), int(bx[3])]
        merged["conf"] = float(conf)
        out.append(merged)
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
        self.model = YOLO(model_path)

    def predict(self, img_bgr: np.ndarray, cfg: Dict[str, Any], dev: DeviceChoice) -> Tuple[List[Dict[str, Any]], Dict[str, int]]:
        det_cfg = cfg["detector"]
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
        names = r0.names
        dets: List[Dict[str, Any]] = []

        keep = set([str(x).lower() for x in det_cfg.get("keep_classes", [])])
        min_box_px = int(det_cfg.get("min_box_px", 18))
        min_area_frac = float(det_cfg.get("min_area_frac", 0.0))
        ar_min = float(det_cfg.get("aspect_ratio_min", 0.0))
        ar_max = float(det_cfg.get("aspect_ratio_max", 1e9))

        h, w = img_bgr.shape[:2]
        min_area = float(w * h) * float(min_area_frac)

        if r0.boxes is not None and len(r0.boxes) > 0:
            xyxy = r0.boxes.xyxy.cpu().numpy().astype(np.int32)
            confs = r0.boxes.conf.cpu().numpy().astype(np.float32)
            clss = r0.boxes.cls.cpu().numpy().astype(np.int32)

            for bb, cf, ci in zip(xyxy, confs, clss):
                x1, y1, x2, y2 = map(int, bb.tolist())
                x1 = clamp(x1, 0, w - 1)
                y1 = clamp(y1, 0, h - 1)
                x2 = clamp(x2, 0, w - 1)
                y2 = clamp(y2, 0, h - 1)
                bw = max(0, x2 - x1)
                bh = max(0, y2 - y1)
                if bw < min_box_px or bh < min_box_px:
                    continue
                area = float(bw * bh)
                if area < min_area:
                    continue
                ar = (float(bw) / float(bh)) if bh > 0 else 0.0
                if ar < ar_min or ar > ar_max:
                    continue

                cls_name = str(names.get(int(ci), str(ci))).lower()
                if keep and (cls_name not in keep):
                    continue

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

        r_pos = float(np.median(angs)) if len(angs) > 0 else 0.0
        det["meta"]["r_pos_deg"] = r_pos
        det["meta"]["rot_x_deg"] = 0.0
        det["meta"]["rot_y_deg"] = 0.0
        det["meta"]["rot_z_deg"] = r_pos
        det["meta"]["pose_src"] = "hough_roi"
        return det


class ClusterRefiner:
    def __init__(self, detector: VehicleDetector):
        self.detector = detector

    def refine(self, img_bgr: np.ndarray, dets: List[Dict[str, Any]], cfg: Dict[str, Any], dev: DeviceChoice) -> Tuple[List[Dict[str, Any]], Dict[str, Any]]:
        cr = cfg.get("cluster_refine", {})
        if not bool(cr.get("enabled", True)):
            return dets, {}

        h, w = img_bgr.shape[:2]
        roi_scale = float(cr.get("roi_scale", 2.0))
        roi_min = int(cr.get("roi_min_size", 640))
        refine_imgsz = int(cr.get("refine_imgsz", cfg["detector"].get("imgsz", 960)))
        refine_conf = float(cr.get("refine_conf", cfg["detector"].get("conf", 0.20)))
        refine_iou = float(cr.get("refine_iou", cfg["detector"].get("iou", 0.50)))
        min_keep_iou = float(cr.get("min_keep_iou", 0.12))

        cfg2 = json.loads(json.dumps(cfg))
        cfg2["detector"]["imgsz"] = refine_imgsz
        cfg2["detector"]["conf"] = refine_conf
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
                d["meta"]["refined"] = False
                refined.append(d)
                continue

            t0 = now_ms()
            roi_dets, _ = self.detector.predict(roi, cfg2, dev)
            t1 = now_ms()
            t_total += int(t1 - t0)

            base_box = (x1, y1, x2, y2)
            kept = []
            for rd in roi_dets:
                rbx1, rby1, rbx2, rby2 = rd["bbox_xyxy"]
                ob = (rbx1 + rx1, rby1 + ry1, rbx2 + rx1, rby2 + ry1)
                if iou_xyxy(base_box, ob) >= min_keep_iou:
                    kept.append(ob)

            if not kept:
                d["meta"]["refined"] = False
                refined.append(d)
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
            nd["meta"] = dict(d.get("meta", {}))
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
            focal_px = (focal_mm / sensor_w_mm) * float(w)

        cls_name = str(det.get("cls_name", "car")).lower()
        class_height = dist_cfg.get("class_height_m", {})
        real_h = float(class_height.get(cls_name, dist_cfg.get("default_height_m", 1.5)))

        x1, y1, x2, y2 = det["bbox_xyxy"]
        bh = max(1, int(y2 - y1))
        dist_m = (real_h * focal_px) / float(bh)

        det["meta"]["distance_m"] = float(dist_m)
        det["meta"]["focal_px"] = float(focal_px)
        det["meta"]["real_height_m"] = float(real_h)
        return det


class Cleanup:
    @staticmethod
    def cleanup_image(img_bgr: np.ndarray, dets: List[Dict[str, Any]], cfg: Dict[str, Any]) -> np.ndarray:
        cc = cfg.get("cleanup", {})
        if not bool(cc.get("enabled", True)):
            return img_bgr.copy()

        h, w = img_bgr.shape[:2]
        out = np.zeros((h, w, 3), dtype=np.uint8)
        for d in dets:
            x1, y1, x2, y2 = d["bbox_xyxy"]
            out[y1:y2, x1:x2] = img_bgr[y1:y2, x1:x2]
        return out


def draw_annotated(img_bgr: np.ndarray, dets: List[Dict[str, Any]]) -> np.ndarray:
    out = img_bgr.copy()
    for i, d in enumerate(dets):
        x1, y1, x2, y2 = d["bbox_xyxy"]
        cls_name = d.get("cls_name", "")
        conf = float(d.get("conf", 0.0))

        label = f"{i}:{cls_name} {conf:.2f}"
        meta = d.get("meta", {}) or {}
        if "distance_m" in meta:
            label += f" {float(meta['distance_m']):.1f}m"
        if "r_pos_deg" in meta:
            label += f" r={float(meta['r_pos_deg']):.1f}"

        cv2.rectangle(out, (x1, y1), (x2, y2), (0, 220, 0), 2)
        cv2.putText(out, label, (x1, max(0, y1 - 7)), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 220, 0), 2, cv2.LINE_AA)
    return out


def write_csv(path: str, image_path: str, dets: List[Dict[str, Any]], device_used: str, timings_ms: Dict[str, Any]) -> None:
    header = "filename,det_idx,cls,x1,y1,x2,y2,w_px,h_px,conf,distance_m,device,inference_ms,total_ms\n"
    base = os.path.basename(image_path)

    inf_ms = int(timings_ms.get("inference", 0))
    tot_ms = int(timings_ms.get("total", 0))

    lines = [header]
    for i, d in enumerate(dets):
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


def run_cluster(cluster_id: int, image_path: str, out_dir: str, device_mode: str, cfg: Dict[str, Any]) -> Dict[str, Any]:
    ensure_dir(out_dir)
    img = load_image_any(image_path)
    h, w = img.shape[:2]

    dev_cfg = cfg.get("device", {})
    gpu_id = int(dev_cfg.get("gpu_id", 0))
    fallback = bool(dev_cfg.get("fallback_to_cpu", True))

    warnings: List[str] = []
    t_total0 = now_ms()

    dev = DeviceManager.pick(device_mode, gpu_id, fallback)
    detector = VehicleDetector(cfg["detector"].get("model_path", "yolov8n.pt"))

    inference_ms = 0
    try:
        dets, tms = detector.predict(img, cfg, dev)
        inference_ms += int(tms.get("inference_ms", 0))
    except Exception as e:
        if fallback:
            warnings.append("gpu_failed_fallback_to_cpu")
            dev = DeviceChoice("cpu", "cpu")
            dets, tms = detector.predict(img, cfg, dev)
            inference_ms += int(tms.get("inference_ms", 0))
        else:
            raise

    if cfg.get("union_nms", {}).get("enabled", True):
        dets = union_merge_boxes(dets, float(cfg.get("union_nms", {}).get("iou", 0.55)))

    if cluster_id == 2:
        re = RotationEstimator()
        dets = [re.estimate(img, d, cfg) for d in dets]

    if cluster_id == 3:
        ref = ClusterRefiner(detector)
        dets, _ = ref.refine(img, dets, cfg, dev)

    if cluster_id == 4:
        de = DistanceEstimator()
        dets = [de.estimate(img, d, cfg) for d in dets]

    annotated = draw_annotated(img, dets)
    annotated_path = os.path.join(out_dir, f"annotated_cluster_{cluster_id}.jpg")
    cv2.imwrite(annotated_path, annotated)

    t_total1 = now_ms()

    return {
        "module_id": f"cluster_{cluster_id}",
        "image_w": int(w),
        "image_h": int(h),
        "device_used": dev.device_used,
        "warnings": warnings,
        "annotated_image_path": annotated_path,
        "cleaned_image_path": "",
        "artifacts": {},
        "timings_ms": {
            "total": int(t_total1 - t_total0),
            "preprocess": 0,
            "inference": int(inference_ms),
            "postprocess": int(max(0, (t_total1 - t_total0) - inference_ms))
        },
        "detections": dets
    }


def run_full_distance(image_path: str, out_dir: str, device_mode: str, cfg: Dict[str, Any]) -> Dict[str, Any]:
    ensure_dir(out_dir)
    img = load_image_any(image_path)
    h, w = img.shape[:2]

    dev_cfg = cfg.get("device", {})
    gpu_id = int(dev_cfg.get("gpu_id", 0))
    fallback = bool(dev_cfg.get("fallback_to_cpu", True))

    warnings: List[str] = []
    t_total0 = now_ms()

    dev = DeviceManager.pick(device_mode, gpu_id, fallback)
    detector = VehicleDetector(cfg["detector"].get("model_path", "yolov8n.pt"))

    inference_ms = 0
    try:
        dets, tms = detector.predict(img, cfg, dev)
        inference_ms += int(tms.get("inference_ms", 0))
    except Exception as e:
        if fallback:
            warnings.append("gpu_failed_fallback_to_cpu")
            dev = DeviceChoice("cpu", "cpu")
            dets, tms = detector.predict(img, cfg, dev)
            inference_ms += int(tms.get("inference_ms", 0))
        else:
            raise

    if cfg.get("union_nms", {}).get("enabled", True):
        dets = union_merge_boxes(dets, float(cfg.get("union_nms", {}).get("iou", 0.55)))

    de = DistanceEstimator()
    dets = [de.estimate(img, d, cfg) for d in dets]

    cleaned = Cleanup.cleanup_image(img, dets, cfg)
    cleaned_path = os.path.join(out_dir, "cleaned_only_vehicles.jpg")
    cv2.imwrite(cleaned_path, cleaned)

    annotated = draw_annotated(img, dets)
    annotated_path = os.path.join(out_dir, "annotated_full.jpg")
    cv2.imwrite(annotated_path, annotated)

    t_total1 = now_ms()

    timings_ms = {
        "total": int(t_total1 - t_total0),
        "preprocess": 0,
        "inference": int(inference_ms),
        "postprocess": int(max(0, (t_total1 - t_total0) - inference_ms))
    }

    csv_path = os.path.join(out_dir, "distance_results.csv")
    write_csv(csv_path, image_path, dets, dev.device_used, timings_ms)

    return {
        "module_id": "distance_full",
        "image_w": int(w),
        "image_h": int(h),
        "device_used": dev.device_used,
        "warnings": warnings,
        "annotated_image_path": annotated_path,
        "cleaned_image_path": cleaned_path,
        "artifacts": {
            "csv_path": csv_path
        },
        "timings_ms": timings_ms,
        "detections": dets
    }


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--task", required=True, choices=["cluster", "distance_full"])
    ap.add_argument("--cluster-id", type=int, default=0)
    ap.add_argument("--input", required=True)
    ap.add_argument("--output-dir", required=True)
    ap.add_argument("--device", default="auto", choices=["auto", "gpu", "cpu"])
    ap.add_argument("--config", required=True)
    ap.add_argument("--result-json", required=True)
    args = ap.parse_args()

    cfg = read_json(args.config)

    if args.task == "cluster":
        if args.cluster_id not in (1, 2, 3, 4):
            raise SystemExit("cluster-id must be 1..4")
        res = run_cluster(args.cluster_id, args.input, args.output_dir, args.device, cfg)
    else:
        res = run_full_distance(args.input, args.output_dir, args.device, cfg)

    write_json(args.result_json, res)
    print("OK")


if __name__ == "__main__":
    main()