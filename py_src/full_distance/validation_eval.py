from __future__ import annotations

import argparse
import csv
import json
import math
import os
import statistics
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

from full_distance import run as run_full_distance


def _safe_float(value: Any) -> Optional[float]:
    if value is None:
        return None
    try:
        return float(value)
    except Exception:
        return None


def _read_json(path: str) -> Dict[str, Any]:
    try:
        with open(path, "r", encoding="utf-8") as f:
            obj = json.load(f)
        if isinstance(obj, dict):
            return obj
    except Exception:
        pass
    return {}


def _write_json(path: str, obj: Dict[str, Any]) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(obj, f, ensure_ascii=False, indent=2)


def _haversine_m(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    r = 6371000.0
    p1 = math.radians(lat1)
    p2 = math.radians(lat2)
    dp = math.radians(lat2 - lat1)
    dl = math.radians(lon2 - lon1)
    a = math.sin(dp / 2.0) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2.0) ** 2
    return 2.0 * r * math.atan2(math.sqrt(a), math.sqrt(max(0.0, 1.0 - a)))


def _angle_delta_deg(a: float, b: float) -> float:
    d = abs(float(a) - float(b)) % 360.0
    return min(d, 360.0 - d)


def _metric_summary(values: List[float]) -> Dict[str, Any]:
    clean = [float(v) for v in values if v is not None and math.isfinite(v)]
    if not clean:
        return {
            "count": 0,
            "mean": None,
            "median": None,
            "min": None,
            "max": None,
            "p90": None,
            "stdev": None,
        }

    clean_sorted = sorted(clean)
    p90_index = min(len(clean_sorted) - 1, max(0, int(math.ceil(len(clean_sorted) * 0.90)) - 1))

    return {
        "count": int(len(clean_sorted)),
        "mean": round(float(statistics.fmean(clean_sorted)), 6),
        "median": round(float(statistics.median(clean_sorted)), 6),
        "min": round(float(clean_sorted[0]), 6),
        "max": round(float(clean_sorted[-1]), 6),
        "p90": round(float(clean_sorted[p90_index]), 6),
        "stdev": round(float(statistics.pstdev(clean_sorted)), 6) if len(clean_sorted) > 1 else 0.0,
    }


def _pearson(xs: List[Optional[float]], ys: List[Optional[float]]) -> Optional[float]:
    pairs: List[Tuple[float, float]] = []
    for x, y in zip(xs, ys):
        if x is None or y is None:
            continue
        if not (math.isfinite(float(x)) and math.isfinite(float(y))):
            continue
        pairs.append((float(x), float(y)))

    if len(pairs) < 2:
        return None

    xvals = [p[0] for p in pairs]
    yvals = [p[1] for p in pairs]

    mx = statistics.fmean(xvals)
    my = statistics.fmean(yvals)

    num = sum((x - mx) * (y - my) for x, y in pairs)
    den_x = math.sqrt(sum((x - mx) ** 2 for x in xvals))
    den_y = math.sqrt(sum((y - my) ** 2 for y in yvals))

    den = den_x * den_y
    if den <= 1e-12:
        return None
    return round(float(num / den), 6)


def _extract_top_items(manifest_obj: Any) -> Tuple[Dict[str, Any], List[Dict[str, Any]]]:
    if isinstance(manifest_obj, dict):
        items = manifest_obj.get("items", [])
        if isinstance(items, list):
            return manifest_obj, [x for x in items if isinstance(x, dict)]
        return manifest_obj, []
    if isinstance(manifest_obj, list):
        return {}, [x for x in manifest_obj if isinstance(x, dict)]
    return {}, []


def _default_output_dir(report_dir: str, image_path: str, index: int) -> str:
    stem = Path(image_path).stem.strip() or f"item_{index:03d}"
    return os.path.join(report_dir, "work", stem)


def _load_existing_outputs(output_dir: str) -> Tuple[Dict[str, Any], Dict[str, Any]]:
    camera_refine = _read_json(os.path.join(output_dir, "camera_refine.json"))
    vehicle_geo = _read_json(os.path.join(output_dir, "vehicle_geo.json"))
    return camera_refine, vehicle_geo


def _run_or_load_item(
        item: Dict[str, Any],
        top_cfg: Dict[str, Any],
        report_dir: str,
        index: int,
) -> Tuple[Dict[str, Any], Dict[str, Any], str, str]:
    image_path = str(item.get("image_path") or "").strip()
    if not image_path:
        raise RuntimeError("item.image_path не задан")

    config_path = str(item.get("config_path") or top_cfg.get("default_config_path") or "").strip()
    if not config_path:
        raise RuntimeError(f"Для {image_path} не задан config_path")

    output_dir = str(item.get("output_dir") or "").strip()
    if not output_dir:
        output_dir = _default_output_dir(report_dir, image_path, index)

    device_mode = str(item.get("device_mode") or top_cfg.get("default_device_mode") or "auto").strip() or "auto"
    reuse_existing = bool(item.get("reuse_existing", top_cfg.get("reuse_existing", True)))

    camera_refine, vehicle_geo = ({}, {})
    if reuse_existing:
        camera_refine, vehicle_geo = _load_existing_outputs(output_dir)

    if camera_refine and vehicle_geo:
        return camera_refine, vehicle_geo, output_dir, "loaded"

    cfg = _read_json(config_path)
    if not cfg:
        raise RuntimeError(f"Не удалось прочитать config_path: {config_path}")

    os.makedirs(output_dir, exist_ok=True)
    result = run_full_distance(
        image_path=image_path,
        out_dir=output_dir,
        cfg=cfg,
        device_mode=device_mode,
        state=None,
    )

    artifacts = result.get("artifacts", {}) if isinstance(result, dict) else {}
    if not isinstance(artifacts, dict):
        artifacts = {}

    camera_refine = artifacts.get("camera_refine", {}) or {}
    vehicle_geo = artifacts.get("vehicle_geo", {}) or {}

    return camera_refine, vehicle_geo, output_dir, "executed"


def _build_row(
        item: Dict[str, Any],
        camera_refine: Dict[str, Any],
        vehicle_geo: Dict[str, Any],
        output_dir: str,
        run_mode: str,
) -> Dict[str, Any]:
    gt = item.get("ground_truth", {}) if isinstance(item.get("ground_truth"), dict) else {}
    gt_camera = gt.get("camera", {}) if isinstance(gt.get("camera"), dict) else {}
    gt_vehicle = gt.get("vehicle", {}) if isinstance(gt.get("vehicle"), dict) else {}

    gt_cam_lat = _safe_float(gt_camera.get("lat"))
    gt_cam_lon = _safe_float(gt_camera.get("lon"))
    gt_cam_az = _safe_float(gt_camera.get("azimuth_deg"))

    gt_vehicle_lat = _safe_float(gt_vehicle.get("lat"))
    gt_vehicle_lon = _safe_float(gt_vehicle.get("lon"))

    init_lat = _safe_float(camera_refine.get("camera_init_lat"))
    init_lon = _safe_float(camera_refine.get("camera_init_lon"))
    refined_lat = _safe_float(camera_refine.get("camera_refined_lat"))
    refined_lon = _safe_float(camera_refine.get("camera_refined_lon"))
    refined_az = _safe_float(camera_refine.get("camera_refined_azimuth_deg"))

    exif_error_m = None
    refined_error_m = None
    azimuth_error_deg = None
    vehicle_error_m = None

    if gt_cam_lat is not None and gt_cam_lon is not None:
        if init_lat is not None and init_lon is not None:
            exif_error_m = _haversine_m(init_lat, init_lon, gt_cam_lat, gt_cam_lon)
        if refined_lat is not None and refined_lon is not None:
            refined_error_m = _haversine_m(refined_lat, refined_lon, gt_cam_lat, gt_cam_lon)

    if gt_cam_az is not None and refined_az is not None:
        azimuth_error_deg = _angle_delta_deg(refined_az, gt_cam_az)

    pred_vehicle_lat = _safe_float(vehicle_geo.get("vehicle_lat"))
    pred_vehicle_lon = _safe_float(vehicle_geo.get("vehicle_lon"))
    if gt_vehicle_lat is not None and gt_vehicle_lon is not None and pred_vehicle_lat is not None and pred_vehicle_lon is not None:
        vehicle_error_m = _haversine_m(pred_vehicle_lat, pred_vehicle_lon, gt_vehicle_lat, gt_vehicle_lon)

    scene_features = camera_refine.get("scene_features", {}) if isinstance(camera_refine.get("scene_features"), dict) else {}
    quality = scene_features.get("quality", {}) if isinstance(scene_features.get("quality"), dict) else {}

    row: Dict[str, Any] = {
        "image_path": str(item.get("image_path") or ""),
        "output_dir": output_dir,
        "run_mode": run_mode,

        "gt_camera_lat": gt_cam_lat,
        "gt_camera_lon": gt_cam_lon,
        "gt_camera_azimuth_deg": gt_cam_az,
        "gt_vehicle_lat": gt_vehicle_lat,
        "gt_vehicle_lon": gt_vehicle_lon,

        "camera_init_lat": init_lat,
        "camera_init_lon": init_lon,
        "camera_refined_lat": refined_lat,
        "camera_refined_lon": refined_lon,
        "camera_refined_azimuth_deg": refined_az,

        "camera_confidence": _safe_float(camera_refine.get("camera_confidence")),
        "camera_uncertainty_m": _safe_float(camera_refine.get("camera_uncertainty_m")),
        "camera_status": camera_refine.get("status"),
        "camera_needs_manual_review": bool(camera_refine.get("needs_manual_review", False)),
        "camera_refine_method": camera_refine.get("refine_method"),
        "scene_score": _safe_float(quality.get("scene_score")),
        "has_two_sides": bool(quality.get("has_two_sides", False)),
        "has_vanishing_point": bool(quality.get("has_vanishing_point", False)),
        "has_lane_like_geometry": bool(quality.get("has_lane_like_geometry", False)),

        "vehicle_lat": pred_vehicle_lat,
        "vehicle_lon": pred_vehicle_lon,
        "vehicle_distance_m": _safe_float(vehicle_geo.get("vehicle_distance_m")),
        "vehicle_bearing_deg": _safe_float(vehicle_geo.get("vehicle_bearing_deg")),
        "vehicle_confidence": _safe_float(vehicle_geo.get("vehicle_confidence")),
        "vehicle_status": vehicle_geo.get("status"),
        "vehicle_needs_manual_review": bool(vehicle_geo.get("needs_manual_review", False)),

        "exif_error_m": round(float(exif_error_m), 6) if exif_error_m is not None else None,
        "refined_error_m": round(float(refined_error_m), 6) if refined_error_m is not None else None,
        "camera_azimuth_error_deg": round(float(azimuth_error_deg), 6) if azimuth_error_deg is not None else None,
        "vehicle_error_m": round(float(vehicle_error_m), 6) if vehicle_error_m is not None else None,
    }

    return row


def _write_rows_csv(path: str, rows: List[Dict[str, Any]]) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)

    keys: List[str] = []
    seen = set()
    for row in rows:
        for k in row.keys():
            if k not in seen:
                seen.add(k)
                keys.append(k)

    with open(path, "w", encoding="utf-8-sig", newline="") as f:
        wr = csv.DictWriter(f, fieldnames=keys)
        wr.writeheader()
        for row in rows:
            wr.writerow(row)


def _build_summary(rows: List[Dict[str, Any]]) -> Dict[str, Any]:
    exif_errors = [_safe_float(r.get("exif_error_m")) for r in rows]
    refined_errors = [_safe_float(r.get("refined_error_m")) for r in rows]
    vehicle_errors = [_safe_float(r.get("vehicle_error_m")) for r in rows]
    azimuth_errors = [_safe_float(r.get("camera_azimuth_error_deg")) for r in rows]

    scene_scores = [_safe_float(r.get("scene_score")) for r in rows]
    cam_conf = [_safe_float(r.get("camera_confidence")) for r in rows]
    cam_unc = [_safe_float(r.get("camera_uncertainty_m")) for r in rows]

    summary: Dict[str, Any] = {
        "items_total": int(len(rows)),
        "camera_metrics": {
            "exif_error_m": _metric_summary([v for v in exif_errors if v is not None]),
            "refined_error_m": _metric_summary([v for v in refined_errors if v is not None]),
            "azimuth_error_deg": _metric_summary([v for v in azimuth_errors if v is not None]),
        },
        "vehicle_metrics": {
            "vehicle_error_m": _metric_summary([v for v in vehicle_errors if v is not None]),
        },
        "correlations": {
            "refined_error_vs_scene_score": _pearson(refined_errors, scene_scores),
            "refined_error_vs_camera_confidence": _pearson(refined_errors, cam_conf),
            "refined_error_vs_camera_uncertainty": _pearson(refined_errors, cam_unc),
            "vehicle_error_vs_scene_score": _pearson(vehicle_errors, scene_scores),
            "vehicle_error_vs_camera_confidence": _pearson(vehicle_errors, cam_conf),
            "vehicle_error_vs_camera_uncertainty": _pearson(vehicle_errors, cam_unc),
        },
        "counts": {
            "with_gt_camera": int(sum(1 for r in rows if r.get("gt_camera_lat") is not None and r.get("gt_camera_lon") is not None)),
            "with_gt_vehicle": int(sum(1 for r in rows if r.get("gt_vehicle_lat") is not None and r.get("gt_vehicle_lon") is not None)),
            "with_exif_error": int(sum(1 for v in exif_errors if v is not None)),
            "with_refined_error": int(sum(1 for v in refined_errors if v is not None)),
            "with_vehicle_error": int(sum(1 for v in vehicle_errors if v is not None)),
        },
    }

    return summary


def evaluate_manifest(manifest_path: str, report_dir: str) -> Dict[str, Any]:
    manifest_obj = _read_json(manifest_path)
    top_cfg, items = _extract_top_items(manifest_obj)

    if not items:
        raise RuntimeError("В manifest нет items")

    os.makedirs(report_dir, exist_ok=True)

    rows: List[Dict[str, Any]] = []
    errors: List[Dict[str, Any]] = []

    for idx, item in enumerate(items):
        try:
            camera_refine, vehicle_geo, output_dir, run_mode = _run_or_load_item(
                item=item,
                top_cfg=top_cfg,
                report_dir=report_dir,
                index=idx,
            )
            row = _build_row(
                item=item,
                camera_refine=camera_refine,
                vehicle_geo=vehicle_geo,
                output_dir=output_dir,
                run_mode=run_mode,
            )
            rows.append(row)
        except Exception as e:
            errors.append({
                "index": int(idx),
                "image_path": str(item.get("image_path") or ""),
                "error": f"{type(e).__name__}: {e}",
            })

    summary = _build_summary(rows)
    result = {
        "manifest_path": os.path.abspath(manifest_path),
        "report_dir": os.path.abspath(report_dir),
        "summary": summary,
        "rows_count": int(len(rows)),
        "errors_count": int(len(errors)),
        "errors": errors,
    }

    _write_rows_csv(os.path.join(report_dir, "validation_rows.csv"), rows)
    _write_json(os.path.join(report_dir, "validation_rows.json"), {"rows": rows})
    _write_json(os.path.join(report_dir, "validation_summary.json"), result)

    return result


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser()
    ap.add_argument("--manifest", required=True, help="Путь к validation_manifest.json")
    ap.add_argument("--report-dir", required=True, help="Папка для validation_rows.csv и validation_summary.json")
    return ap


def main() -> int:
    args = build_parser().parse_args()
    result = evaluate_manifest(args.manifest, args.report_dir)
    print(json.dumps(result["summary"], ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())