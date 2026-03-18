from importlib import import_module
from typing import Any, Dict, Optional

def run(image_path: str, out_dir: str, cfg: Dict[str, Any], device_mode: str = "auto", state: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
    return import_module("notebooks.rotation_detect").run(image_path, out_dir, cfg, device_mode)