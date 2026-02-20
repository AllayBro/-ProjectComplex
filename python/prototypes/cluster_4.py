from pathlib import Path
import sys

_THIS_DIR = Path(__file__).resolve().parent
_PY_ROOT = _THIS_DIR.parent
if str(_PY_ROOT) not in sys.path:
    sys.path.insert(0, str(_PY_ROOT))

from runner import run_cluster


def _device_mode(cfg: dict) -> str:
    v = cfg.get("device_mode")
    if v:
        return str(v).lower()
    v = (cfg.get("device") or {}).get("mode")
    if v:
        return str(v).lower()
    return "auto"


def run(image_path: str, out_dir: str, cfg: dict, state: dict = None) -> dict:
    res = run_cluster(4, image_path, out_dir, _device_mode(cfg), cfg)

    artifacts = []
    p = res.get("annotated_image_path") or ""
    if p:
        artifacts.append(p)

    csv_path = ((res.get("artifacts") or {}).get("csv_path")) or ""
    if csv_path:
        artifacts.append(csv_path)

    return {
        "artifacts": artifacts,
        "data": res,
        "state_update": {"detections": res.get("detections", [])}
    }