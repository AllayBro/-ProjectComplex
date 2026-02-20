from __future__ import annotations

import importlib
import inspect
import os
import sys
import traceback
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Optional, Tuple


@dataclass(frozen=True)
class ClusterCall:
    cluster_id: int
    image_path: str
    out_dir: str
    cfg: Dict[str, Any]
    state: Dict[str, Any]


def _normalize_module_name(name: str) -> str:
    return name.strip().replace("\\", ".").replace("/", ".").strip(".")


def _import_module(module_name: str, extra_sys_path: Optional[Path]) -> Any:
    if extra_sys_path is not None:
        p = str(extra_sys_path.resolve())
        if p not in sys.path:
            sys.path.insert(0, p)
    return importlib.import_module(module_name)


def _call_run(mod: Any, call: ClusterCall) -> Dict[str, Any]:
    if not hasattr(mod, "run"):
        raise RuntimeError(f"Модуль '{mod.__name__}' не содержит функцию run(...)")

    fn = getattr(mod, "run")
    sig = inspect.signature(fn)

    kwargs: Dict[str, Any] = {}
    params = sig.parameters

    if "image_path" in params:
        kwargs["image_path"] = call.image_path
    if "out_dir" in params:
        kwargs["out_dir"] = call.out_dir
    if "cfg" in params:
        kwargs["cfg"] = call.cfg
    if "state" in params:
        kwargs["state"] = call.state
    if "cluster_id" in params:
        kwargs["cluster_id"] = call.cluster_id

    result = fn(**kwargs)

    if not isinstance(result, dict):
        raise RuntimeError(f"run(...) в '{mod.__name__}' должен вернуть dict, получено: {type(result)}")

    return result


def resolve_cluster_module(cfg: Dict[str, Any], cluster_id: int) -> Tuple[str, Optional[Path]]:
    modules = cfg.get("cluster_modules", {})
    if not isinstance(modules, dict):
        raise RuntimeError("cluster_modules в cfg должен быть объектом JSON (dict)")

    key = str(cluster_id)
    module_name = modules.get(key)
    if not module_name:
        raise RuntimeError(f"Для cluster_id={cluster_id} нет записи в cfg.cluster_modules['{key}']")

    module_name = _normalize_module_name(str(module_name))

    extra = cfg.get("python_module_root", None)
    extra_path = Path(extra) if extra else None

    return module_name, extra_path


def run_cluster(cluster_id: int, image_path: str, out_dir: str, cfg: Dict[str, Any], state: Dict[str, Any]) -> Dict[str, Any]:
    module_name, extra_path = resolve_cluster_module(cfg, cluster_id)
    mod = _import_module(module_name, extra_path)

    call = ClusterCall(
        cluster_id=cluster_id,
        image_path=image_path,
        out_dir=out_dir,
        cfg=cfg,
        state=state,
    )

    return _call_run(mod, call)


def write_error_file(out_dir: str, exc: BaseException) -> str:
    Path(out_dir).mkdir(parents=True, exist_ok=True)
    p = Path(out_dir) / "error_details.txt"
    tb = "".join(traceback.format_exception(type(exc), exc, exc.__traceback__))
    p.write_text(tb, encoding="utf-8")
    return str(p.resolve())