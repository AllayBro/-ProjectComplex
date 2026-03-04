import argparse
import base64
import json
import mimetypes
import os
import platform
import sys
import time
import traceback
from datetime import datetime, timezone
from io import StringIO
from pathlib import Path
from typing import Any, Dict, List, Optional

from PIL import Image, ImageOps, ExifTags

ROOT = Path(__file__).resolve().parent
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))


def _enable_heic_heif() -> None:
    try:
        from pillow_heif import register_heif_opener  # type: ignore
        register_heif_opener()
        print("[INFO] HEIC/HEIF enabled via pillow-heif")
    except Exception as e:
        print(f"[WARNING] HEIC/HEIF not enabled: {e}")


def load_image_bgr(path: str):
    try:
        import numpy as np  # type: ignore
    except Exception:
        return None

    try:
        im = Image.open(path)
        im.load()
        im = ImageOps.exif_transpose(im)
        if im.mode != "RGB":
            im = im.convert("RGB")
        rgb = np.asarray(im)
        if getattr(rgb, "ndim", 0) != 3 or rgb.shape[2] < 3:
            return None
        bgr = rgb[:, :, ::-1].copy()
        return bgr
    except Exception:
        return None


def _patch_cv2_imread() -> None:
    try:
        import cv2  # type: ignore
    except Exception as e:
        print(f"[WARNING] OpenCV patch skipped: {e}")
        return

    orig = cv2.imread

    def patched(path: str, flags: int = cv2.IMREAD_COLOR):
        img = orig(path, flags)
        if img is not None:
            return img
        return load_image_bgr(path)

    cv2.imread = patched
    print("[INFO] cv2.imread patched (Pillow fallback for unsupported formats)")

def _open_image_any(path: str) -> Image.Image:
    """Открыть изображение (включая HEIC/HEIF при наличии pillow-heif)."""
    p = Path(path)
    ext = p.suffix.lower()
    try:
        im = Image.open(path)
        im.load()
        return im
    except Exception as e:
        if ext in {".heic", ".heif"}:
            # запасной вариант: pyheif (если установлен)
            try:
                import pyheif  # type: ignore

                h = pyheif.read(path)
                im = Image.frombytes(h.mode, h.size, h.data, "raw", h.mode)
                im.load()
                return im
            except Exception:
                raise RuntimeError(
                    "HEIC/HEIF не прочитан. Установите пакет pillow-heif (рекомендуется) "
                    "или pyheif."
                ) from e
        raise

_enable_heic_heif()
_patch_cv2_imread()



from result_contract import build_result  # noqa: E402


def read_json(path: str) -> Dict[str, Any]:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def write_json(path: str, obj: Dict[str, Any]) -> None:
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(obj, f, ensure_ascii=False, indent=2)


def _split_lines(s: str) -> List[str]:
    return s.splitlines() if s else []


class _Tee:
    def __init__(self, a, b):
        self.a = a
        self.b = b

    def write(self, s):
        try:
            self.a.write(s)
            self.a.flush()
        except Exception:
            pass
        try:
            self.b.write(s)
            self.b.flush()
        except Exception:
            pass
        return len(s)

    def flush(self):
        try:
            self.a.flush()
        except Exception:
            pass
        try:
            self.b.flush()
        except Exception:
            pass


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(prog="runner.py")
    ap.add_argument("--task", required=True, choices=["cluster", "distance_full", "exif", "preview"])
    ap.add_argument("--cluster-id", type=int, default=0)
    ap.add_argument("--input", required=True)
    ap.add_argument("--output-dir", required=True)
    ap.add_argument("--device", default="auto", choices=["auto", "gpu", "cpu"])
    ap.add_argument("--config", required=True)
    ap.add_argument("--result-json", required=True)
    return ap


def _iso_utc(ts: float) -> str:
    return datetime.fromtimestamp(ts, tz=timezone.utc).isoformat()


def _safe_b64(b: bytes) -> str:
    return base64.b64encode(b).decode("ascii")


def _exif_tag_name(tag_id: int) -> str:
    return ExifTags.TAGS.get(tag_id, f"tag_{tag_id}")


def _gps_tag_name(tag_id: int) -> str:
    return ExifTags.GPSTAGS.get(tag_id, f"gps_{tag_id}")


def _to_jsonable(v: Any) -> Any:
    if v is None:
        return None
    if isinstance(v, (str, int, float, bool)):
        return v
    if isinstance(v, bytes):
        return {"_type": "bytes", "base64": _safe_b64(v), "len": len(v)}
    if isinstance(v, (list, tuple)):
        return [_to_jsonable(x) for x in v]
    if isinstance(v, dict):
        return {str(k): _to_jsonable(val) for k, val in v.items()}
    try:
        return str(v)
    except Exception:
        return repr(v)


def _gps_to_decimal(gps: Dict[str, Any]) -> Dict[str, Any]:
    def _rat(x) -> float:
        try:
            if isinstance(x, tuple) and len(x) == 2 and x[1]:
                return float(x[0]) / float(x[1])
            return float(x)
        except Exception:
            return float("nan")

    def _dms_to_deg(dms) -> float:
        try:
            d = _rat(dms[0])
            m = _rat(dms[1])
            s = _rat(dms[2])
            return d + m / 60.0 + s / 3600.0
        except Exception:
            return float("nan")

    out: Dict[str, Any] = {}
    try:
        lat_ref = str(gps.get("GPSLatitudeRef", "")).upper().strip()
        lon_ref = str(gps.get("GPSLongitudeRef", "")).upper().strip()
        lat_val = gps.get("GPSLatitude")
        lon_val = gps.get("GPSLongitude")
        lat = _dms_to_deg(lat_val) if lat_val is not None else None
        lon = _dms_to_deg(lon_val) if lon_val is not None else None
        if lat is not None and lat_ref == "S":
            lat = -lat
        if lon is not None and lon_ref == "W":
            lon = -lon
        if lat is not None and lon is not None:
            if -90.0 <= lat <= 90.0 and -180.0 <= lon <= 180.0:
                out["GPSLatitudeDecimal"] = lat
                out["GPSLongitudeDecimal"] = lon
    except Exception:
        pass
    return out


def extract_exif_full(path: str) -> Dict[str, Any]:
    p = Path(path)
    out: Dict[str, Any] = {}

    try:
        st = p.stat()
        out["SourceFile"] = str(p)
        out["FileName"] = p.name
        out["Directory"] = str(p.parent)
        out["FileSize"] = int(st.st_size)
        out["FileModifyDateUTC"] = _iso_utc(st.st_mtime)
        out["FileAccessDateUTC"] = _iso_utc(st.st_atime)
        out["FileInodeChangeDateUTC"] = _iso_utc(getattr(st, "st_ctime", st.st_mtime))
    except Exception as e:
        out["FileError"] = str(e)

    ext = p.suffix.lower()
    out["FileTypeExtension"] = ext.lstrip(".")
    mt, _ = mimetypes.guess_type(str(p))
    if mt:
        out["MIMEType"] = mt

    out["PythonVersion"] = sys.version.replace("\n", " ")
    out["Platform"] = platform.platform()

    try:
        img = Image.open(str(p))
        img.load()
    except Exception as e:
        out["ImageOpenError"] = str(e)
        return out

    try:
        out["ImageFormat"] = getattr(img, "format", None)
        out["ImageMode"] = getattr(img, "mode", None)
        sz = getattr(img, "size", None)
        if sz and isinstance(sz, tuple) and len(sz) == 2:
            out["ImageWidth"] = int(sz[0])
            out["ImageHeight"] = int(sz[1])
            out["ImageSize"] = f"{int(sz[0])}x{int(sz[1])}"
    except Exception:
        pass

    try:
        info = getattr(img, "info", {}) or {}
        if "exif" in info and isinstance(info["exif"], (bytes, bytearray)):
            b = bytes(info["exif"])
            out["EXIFBlob"] = {"len": len(b), "base64": _safe_b64(b)}
        if "xmp" in info:
            x = info["xmp"]
            if isinstance(x, (bytes, bytearray)):
                out["XMP"] = {"len": len(x), "base64": _safe_b64(bytes(x))}
            else:
                out["XMP"] = _to_jsonable(x)
        if "icc_profile" in info and isinstance(info["icc_profile"], (bytes, bytearray)):
            b = bytes(info["icc_profile"])
            out["ICCProfile"] = {"len": len(b), "base64": _safe_b64(b)}
    except Exception:
        pass

    exif_out: Dict[str, Any] = {}
    gps_out: Dict[str, Any] = {}
    try:
        exif = img.getexif()
        if exif:
            for tag_id, val in exif.items():
                name = _exif_tag_name(int(tag_id))
                if name == "GPSInfo" and isinstance(val, dict):
                    for gk, gv in val.items():
                        gps_out[_gps_tag_name(int(gk))] = _to_jsonable(gv)
                else:
                    exif_out[name] = _to_jsonable(val)
    except Exception as e:
        out["EXIFReadError"] = str(e)

    if gps_out:
        gps_out.update(_gps_to_decimal(gps_out))

    if exif_out:
        out["EXIF"] = exif_out
    if gps_out:
        out["GPS"] = gps_out

    return out


def main() -> int:
    ap = build_parser()

    if len(sys.argv) == 1:
        ap.print_help()
        print("Этот скрипт запускается из Qt-приложения vk_qt_app.exe")
        return 0

    if any(a in ("-h", "--help") for a in sys.argv[1:]):
        ap.print_help()
        return 0

    args = ap.parse_args()

    cfg = read_json(args.config)
    os.makedirs(args.output_dir, exist_ok=True)

    try:
        exif_full = extract_exif_full(args.input)
    except Exception as e:
        exif_full = {"EXIFExtractError": str(e)}

    started_ms = int(time.time() * 1000)
    buf = StringIO()
    out_tee = _Tee(sys.stdout, buf)
    err_tee = _Tee(sys.stderr, buf)

    module_result: Optional[Dict[str, Any]] = None
    err_obj: Optional[Dict[str, Any]] = None
    exit_code = 0

    try:
        from contextlib import redirect_stdout, redirect_stderr

        with redirect_stdout(out_tee), redirect_stderr(err_tee):
            if args.task == "cluster":
                if args.cluster_id not in (1, 2, 3, 4):
                    raise ValueError("cluster-id must be 1..4")

                mod_name = f"prototypes.cluster_{args.cluster_id}"
                mod = __import__(mod_name, fromlist=["run"])
                module_result = mod.run(
                    image_path=args.input,
                    out_dir=args.output_dir,
                    cfg=cfg,
                    device_mode=args.device,
                )

                if not isinstance(module_result, dict):
                    module_result = {"module_id": mod_name}
                module_result["exif"] = exif_full

            elif args.task == "distance_full":
                from full_distance.full_distance import run as run_full

                module_result = run_full(
                    image_path=args.input,
                    out_dir=args.output_dir,
                    cfg=cfg,
                    device_mode=args.device,
                )

                if not isinstance(module_result, dict):
                    module_result = {"module_id": "distance_full"}
                module_result["exif"] = exif_full
            elif args.task == "preview":
                im0 = _open_image_any(args.input)
                im0 = ImageOps.exif_transpose(im0)

                if im0.mode != "RGBA":
                    im = im0.convert("RGBA")
                else:
                    im = im0

                w, h = im.size

                raw_path = str(Path(args.output_dir) / "preview_rgba.raw")
                with open(raw_path, "wb") as f:
                    f.write(im.tobytes())

                module_result = {
                    "module_id": "preview",
                    "device_used": str(args.device),
                    "image_w": int(w),
                    "image_h": int(h),

                    "input_display_path": str(args.input),

                    "preview_format": "RGBA8888",
                    "preview_w": int(w),
                    "preview_h": int(h),
                    "preview_stride": int(w) * 4,
                    "preview_raw_path": raw_path,

                    "exif": exif_full,
                    "warnings": [],
                    "detections": [],
                    "artifacts": {},
                    "timings_ms": {},
                }
            else:  # exif
                w = int(exif_full.get("ImageWidth", 0) or 0) if isinstance(exif_full, dict) else 0
                h = int(exif_full.get("ImageHeight", 0) or 0) if isinstance(exif_full, dict) else 0
                module_result = {
                    "module_id": "exif_full",
                    "device_used": str(args.device),
                    "image_w": int(w),
                    "image_h": int(h),
                    "exif": exif_full,
                    "warnings": [],
                    "detections": [],
                    "artifacts": {},
                    "timings_ms": {},
                }
    except SystemExit as e:
        exit_code = int(e.code) if isinstance(getattr(e, "code", None), int) else 1
        err_obj = {"type": "SystemExit", "message": str(e), "traceback": traceback.format_exc()}
        try:
            sys.stderr.write(err_obj["traceback"] + "\n")
            sys.stderr.flush()
        except Exception:
            pass

    except Exception as e:
        exit_code = 1
        err_obj = {"type": type(e).__name__, "message": str(e), "traceback": traceback.format_exc()}
        try:
            sys.stderr.write(err_obj["traceback"] + "\n")
            sys.stderr.flush()
        except Exception:
            pass

    except Exception as e:
        exit_code = 1
        err_obj = {"type": type(e).__name__, "message": str(e), "traceback": traceback.format_exc()}
        try:
            sys.stderr.write(err_obj["traceback"] + "\n")
            sys.stderr.flush()
        except Exception:
            pass
    finished_ms = int(time.time() * 1000)
    console_lines = _split_lines(buf.getvalue())
    result_obj = build_result(
        task=str(args.task),
        cluster_id=int(args.cluster_id),
        input_path=str(args.input),
        out_dir=str(args.output_dir),
        result_json_path=str(args.result_json),
        config_path=str(args.config),
        cfg=cfg if isinstance(cfg, dict) else {},
        device_requested=str(args.device),
        module_result=module_result if isinstance(module_result, dict) else None,
        console_lines=console_lines,
        started_ms=started_ms,
        finished_ms=finished_ms,
        error=err_obj,
    )

    write_json(args.result_json, result_obj)

    if exit_code == 0:
        print("OK")
    else:
        print("ERROR")

    return exit_code


if __name__ == "__main__":
    exit_code = main()
    sys.exit(exit_code)