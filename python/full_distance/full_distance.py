from __future__ import annotations

from typing import Any, Dict, Optional

from python.runner import run_full_distance



def run(image_path: str, out_dir: str, cfg: Dict[str, Any], state: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
    device_mode = str(cfg.get("device", {}).get("mode", "auto"))
    res = run_full_distance(image_path, out_dir, device_mode, cfg)

    artifacts = []
    p = str(res.get("annotated_image_path", "") or "")
    if p:
        artifacts.append(p)
    p = str(res.get("cleaned_image_path", "") or "")
    if p:
        artifacts.append(p)

    art = res.get("artifacts", {}) or {}
    if isinstance(art, dict):
        for v in art.values():
            if isinstance(v, str) and v:
                artifacts.append(v)

    return {"artifacts": artifacts, "data": res}