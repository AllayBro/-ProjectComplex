import json
import os
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple


def _now_ms() -> int:
    return int(time.time() * 1000)


def _utc_iso(ms: int) -> str:
    sec = ms / 1000.0
    dt = datetime.fromtimestamp(sec, tz=timezone.utc)
    return dt.isoformat()


def _jsonable(v: Any) -> Any:
    if v is None or isinstance(v, (str, int, float, bool)):
        return v

    if isinstance(v, Path):
        return str(v)

    if isinstance(v, bytes):
        try:
            return v.decode("utf-8", errors="replace")
        except Exception:
            return str(v)

    try:
        import numpy as np  # type: ignore
        if isinstance(v, (np.integer,)):
            return int(v)
        if isinstance(v, (np.floating,)):
            return float(v)
        if isinstance(v, (np.ndarray,)):
            return v.tolist()
    except Exception:
        pass

    if isinstance(v, dict):
        out = {}
        for k, x in v.items():
            out[str(k)] = _jsonable(x)
        return out

    if isinstance(v, (list, tuple)):
        return [_jsonable(x) for x in v]

    return str(v)


def _abs(p: str) -> str:
    if not p:
        return ""
    try:
        return os.path.abspath(p)
    except Exception:
        return str(p)


def _file_exists(p: str) -> bool:
    try:
        return bool(p) and os.path.exists(p)
    except Exception:
        return False


def _extract_model_paths(cfg: Any) -> List[str]:
    res: List[str] = []

    def walk(x: Any) -> None:
        if isinstance(x, dict):
            for k, v in x.items():
                kk = str(k).lower()
                if isinstance(v, (str, Path)) and ("model_path" in kk or kk.endswith("weights") or kk.endswith("weight")):
                    s = str(v)
                    if s and s not in res:
                        res.append(s)
                walk(v)
        elif isinstance(x, list):
            for v in x:
                walk(v)

    walk(cfg)
    return res


def _rat(v: Any) -> Optional[float]:
    if v is None:
        return None
    try:
        if isinstance(v, tuple) and len(v) == 2:
            a = float(v[0])
            b = float(v[1])
            if b == 0.0:
                return None
            return a / b
        return float(v)
    except Exception:
        return None


def _gps_to_deg(coord: Any) -> Optional[float]:
    try:
        if not isinstance(coord, (list, tuple)) or len(coord) != 3:
            return None
        d = _rat(coord[0])
        m = _rat(coord[1])
        s = _rat(coord[2])
        if d is None or m is None or s is None:
            return None
        return float(d) + float(m) / 60.0 + float(s) / 3600.0
    except Exception:
        return None


def _read_exif_from_image(image_path: str) -> Tuple[Dict[str, Any], Dict[str, Any]]:
    exif_data: Dict[str, Any] = {}
    gps_out: Dict[str, Any] = {}

    try:
        from PIL import Image, ExifTags  # type: ignore
    except Exception:
        return exif_data, gps_out

    try:
        img = Image.open(image_path)
    except Exception:
        return exif_data, gps_out

    try:
        ex = img.getexif()
    except Exception:
        return exif_data, gps_out

    try:
        from PIL import ExifTags  # type: ignore
        tag_map = getattr(ExifTags, "TAGS", {}) or {}
        gps_tag_map = getattr(ExifTags, "GPSTAGS", {}) or {}
    except Exception:
        tag_map = {}
        gps_tag_map = {}

    try:
        for tid, val in dict(ex).items():
            name = tag_map.get(int(tid), str(tid))
            exif_data[str(name)] = _jsonable(val)
    except Exception:
        pass

    gps_ifd = None
    try:
        gps_ifd = ex.get_ifd(34853)  # GPSInfo
    except Exception:
        try:
            gps_ifd = ex.get(34853, None)
        except Exception:
            gps_ifd = None

    gps_named: Dict[str, Any] = {}
    if isinstance(gps_ifd, dict):
        for k, v in gps_ifd.items():
            nm = gps_tag_map.get(int(k), str(k))
            gps_named[str(nm)] = v

    lat = None
    lon = None
    alt = None

    try:
        lat_ref = gps_named.get("GPSLatitudeRef", None)
        lat_val = gps_named.get("GPSLatitude", None)
        lon_ref = gps_named.get("GPSLongitudeRef", None)
        lon_val = gps_named.get("GPSLongitude", None)

        lat = _gps_to_deg(lat_val)
        lon = _gps_to_deg(lon_val)

        if isinstance(lat_ref, bytes):
            lat_ref = lat_ref.decode("utf-8", errors="replace")
        if isinstance(lon_ref, bytes):
            lon_ref = lon_ref.decode("utf-8", errors="replace")

        if lat is not None and str(lat_ref).upper().startswith("S"):
            lat = -abs(lat)
        if lon is not None and str(lon_ref).upper().startswith("W"):
            lon = -abs(lon)

        alt_val = gps_named.get("GPSAltitude", None)
        alt = _rat(alt_val)
    except Exception:
        pass

    if gps_named:
        gps_out["raw"] = _jsonable(gps_named)
    if lat is not None and lon is not None:
        gps_out["lat"] = float(lat)
        gps_out["lon"] = float(lon)
    if alt is not None:
        gps_out["alt_m"] = float(alt)

    return _jsonable(exif_data), _jsonable(gps_out)


def _read_exif_from_module_artifact(module_result: Optional[Dict[str, Any]]) -> Tuple[Dict[str, Any], Dict[str, Any]]:
    if not isinstance(module_result, dict):
        return {}, {}

    inline = module_result.get("exif", None)
    if isinstance(inline, dict) and inline:
        gps_out: Dict[str, Any] = {}
        gps_raw = inline.get("GPS", None)
        if isinstance(gps_raw, dict) and gps_raw:
            gps_out["raw"] = _jsonable(gps_raw)

            lat = gps_raw.get("GPSLatitudeDecimal", None)
            lon = gps_raw.get("GPSLongitudeDecimal", None)
            if isinstance(lat, (int, float)) and isinstance(lon, (int, float)):
                gps_out["lat"] = float(lat)
                gps_out["lon"] = float(lon)

            alt = gps_raw.get("GPSAltitude", None)
            alt_m = _rat(alt)
            if alt_m is not None:
                gps_out["alt_m"] = float(alt_m)

        return _jsonable(inline), _jsonable(gps_out)

    artifacts = module_result.get("artifacts", {}) or {}
    if not isinstance(artifacts, dict):
        return {}, {}

    p = str(artifacts.get("exif_json_path", "") or "")
    p = _abs(p)
    if not _file_exists(p):
        return {}, {}

    try:
        with open(p, "r", encoding="utf-8") as f:
            payload = json.load(f)
    except Exception:
        return {}, {}

    exif_data = payload.get("exif", {}) or {}
    gps_out: Dict[str, Any] = {}

    gps = exif_data.get("GPSInfo", None)
    if isinstance(gps, dict):
        gps_out["raw"] = _jsonable(gps)

    return _jsonable(exif_data), _jsonable(gps_out)

def _collect_images(input_path: str, module_result: Optional[Dict[str, Any]]) -> List[Dict[str, Any]]:
    items: List[Dict[str, Any]] = []

    inp = _abs(input_path)
    if isinstance(module_result, dict):
        dp = _abs(str(module_result.get("input_display_path", "") or ""))
        if dp and _file_exists(dp):
            inp = dp

    items.append({"role": "input", "path": inp})

    if isinstance(module_result, dict):
        ap = _abs(str(module_result.get("annotated_image_path", "") or ""))
        cp = _abs(str(module_result.get("cleaned_image_path", "") or ""))
        if ap and _file_exists(ap):
            items.append({"role": "annotated", "path": ap})
        if cp and _file_exists(cp):
            items.append({"role": "cleaned", "path": cp})

        artifacts = module_result.get("artifacts", {}) or {}
        if isinstance(artifacts, dict):
            for k, v in artifacts.items():
                if not isinstance(v, str):
                    continue
                s = _abs(v)
                low = s.lower()
                if low.endswith((".jpg", ".jpeg", ".png", ".bmp", ".webp", ".tif", ".tiff")) and _file_exists(s):
                    items.append({"role": f"artifact:{k}", "path": s})

    uniq: List[Dict[str, Any]] = []
    seen = set()
    for it in items:
        p = str(it.get("path", "") or "")
        if not p or p in seen:
            continue
        seen.add(p)
        uniq.append(it)
    return uniq


def _detections_inline_table(module_result: Optional[Dict[str, Any]]) -> Optional[Dict[str, Any]]:
    if not isinstance(module_result, dict):
        return None
    dets = module_result.get("detections", None)
    if not isinstance(dets, list):
        return None

    cols = ["id", "cls_name", "conf", "x1", "y1", "x2", "y2", "meta"]
    rows: List[List[Any]] = []

    idx = 0
    for d in dets:
        if not isinstance(d, dict):
            continue
        bb = d.get("bbox_xyxy", None)
        if not isinstance(bb, (list, tuple)) or len(bb) != 4:
            continue
        idx += 1
        cls_name = d.get("cls_name", "")
        conf = d.get("conf", None)
        meta = d.get("meta", {}) or {}
        rows.append([
            idx,
            _jsonable(cls_name),
            _jsonable(conf),
            _jsonable(bb[0]),
            _jsonable(bb[1]),
            _jsonable(bb[2]),
            _jsonable(bb[3]),
            _jsonable(meta),
        ])

    return {"name": "detections", "type": "inline", "columns": cols, "rows": rows}


def _collect_tables(module_result: Optional[Dict[str, Any]], exif_data: Dict[str, Any], gps_data: Dict[str, Any]) -> List[Dict[str, Any]]:
    tables: List[Dict[str, Any]] = []

    det_tbl = _detections_inline_table(module_result)
    if det_tbl is not None:
        tables.append(det_tbl)

    if isinstance(module_result, dict):
        artifacts = module_result.get("artifacts", {}) or {}
        if isinstance(artifacts, dict):
            csv_path = str(artifacts.get("csv_path", "") or "")
            csv_path = _abs(csv_path)
            if csv_path:
                name = "table"
                low = os.path.basename(csv_path).lower()
                if "distance" in low:
                    name = "distance_results"
                elif "detect" in low:
                    name = "detections_csv"
                tables.append({"name": name, "type": "csv", "path": csv_path})

            exif_json_path = str(artifacts.get("exif_json_path", "") or "")
            exif_json_path = _abs(exif_json_path)
            if exif_json_path:
                tables.append({"name": "exif_json", "type": "json", "path": exif_json_path})

    if exif_data or gps_data:
        tables.append({"name": "EXIF", "type": "exif", "data": exif_data, "gps": gps_data})

    return tables

def _collect_plots(module_result: Optional[Dict[str, Any]]) -> List[Dict[str, Any]]:
    plots: List[Dict[str, Any]] = []
    if not isinstance(module_result, dict):
        return plots

    artifacts = module_result.get("artifacts", {}) or {}
    if not isinstance(artifacts, dict):
        return plots

    for k, v in artifacts.items():
        if not isinstance(v, str):
            continue
        p = _abs(v)
        low = p.lower()
        if not low.endswith((".png", ".jpg", ".jpeg", ".svg")):
            continue
        if "plot" in str(k).lower() or "graph" in str(k).lower() or "chart" in str(k).lower():
            if _file_exists(p):
                plots.append({"name": str(k), "path": p})

    return plots


def _build_map(input_path: str, exif_gps: Dict[str, Any]) -> Dict[str, Any]:
    pts: List[Dict[str, Any]] = []
    lat = exif_gps.get("lat", None)
    lon = exif_gps.get("lon", None)

    if isinstance(lat, (int, float)) and isinstance(lon, (int, float)):
        pts.append({
            "lat": float(lat),
            "lon": float(lon),
            "image_path": _abs(input_path),
            "label": os.path.basename(input_path),
        })

    return {"points": pts}


def build_result(
        *,
        task: str,
        cluster_id: int,
        input_path: str,
        out_dir: str,
        result_json_path: str,
        config_path: str,
        cfg: Dict[str, Any],
        device_requested: str,
        module_result: Optional[Dict[str, Any]],
        console_lines: List[str],
        started_ms: int,
        finished_ms: int,
        error: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    input_abs = _abs(input_path)
    out_abs = _abs(out_dir)
    res_abs = _abs(result_json_path)
    cfg_abs = _abs(config_path)

    model_paths = _extract_model_paths(cfg)

    exif_data, gps_data = _read_exif_from_module_artifact(module_result)
    if not exif_data:
        exif_data2, gps_data2 = _read_exif_from_image(input_abs)
        exif_data = exif_data2
        gps_data = gps_data2
    else:
        if not gps_data:
            _, gps_data2 = _read_exif_from_image(input_abs)
            gps_data = gps_data2

    meta: Dict[str, Any] = {
        "schema": "vk_result_v1",
        "task": str(task),
        "cluster_id": int(cluster_id),
        "module_id": str(module_result.get("module_id")) if isinstance(module_result, dict) and module_result.get("module_id") is not None else "",
        "time": {
            "started_ms": int(started_ms),
            "finished_ms": int(finished_ms),
            "duration_ms": int(max(0, finished_ms - started_ms)),
            "started_utc": _utc_iso(int(started_ms)),
            "finished_utc": _utc_iso(int(finished_ms)),
        },
        "device": {
            "requested": str(device_requested),
            "used": str(module_result.get("device_used")) if isinstance(module_result, dict) and module_result.get("device_used") is not None else "",
        },
        "paths": {
            "input": input_abs,
            "output_dir": out_abs,
            "result_json": res_abs,
            "config_json": cfg_abs,
        },
        "model": {
            "paths": model_paths,
        },
        "status": "error" if error else "ok",
    }

    if error:
        meta["error"] = _jsonable(error)

    images = _collect_images(input_abs, module_result)
    tables = _collect_tables(module_result, exif_data, gps_data)
    plots = _collect_plots(module_result)

    return {
        "vk_schema": "vk_envelope_v1",
        "meta": _jsonable(meta),
        "console": {"stdout": [str(x) for x in (console_lines or [])], "stderr": []},
        "module": _jsonable(module_result) if isinstance(module_result, dict) else {},
        "images": _jsonable(images),
        "tables": _jsonable(tables),
        "plots": _jsonable(plots),
        "exif": {"data": _jsonable(exif_data), "gps": _jsonable(gps_data)},
        "map": _jsonable(_build_map(input_abs, gps_data)),
    }