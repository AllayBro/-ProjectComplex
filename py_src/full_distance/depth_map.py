from __future__ import annotations

import math
import os
from typing import Any, Dict, List, Optional, Tuple

from .depth_training_plots import save_depth_training_plots

import cv2
import numpy as np

try:
    import torch
except Exception:
    torch = None  # type: ignore

try:
    from PIL import Image
    from transformers import AutoImageProcessor, AutoModelForDepthEstimation
except Exception:
    AutoImageProcessor = None  # type: ignore
    AutoModelForDepthEstimation = None  # type: ignore
    Image = None  # type: ignore

_DEFAULT_MODEL_ID = "depth-anything/Depth-Anything-V2-Small-hf"


def _dm_cfg(cfg: Optional[Dict[str, Any]]) -> Dict[str, Any]:
    return (cfg or {}).get("depth_map", {}) or {}


def _pick_torch_device(dm: Dict[str, Any], fallback_device: str = "auto") -> str:
    pref = str(dm.get("device", fallback_device) or "auto").strip().lower()
    if pref in ("cpu", "cuda", "mps"):
        return pref
    if torch is None:
        return "cpu"
    if pref == "gpu":
        pref = "cuda"
    if pref != "auto":
        return "cpu"
    if torch.cuda.is_available():
        return "cuda"
    if hasattr(torch.backends, "mps") and torch.backends.mps.is_available():
        return "mps"
    return "cpu"


def _safe_float(value: Any) -> Optional[float]:
    if value is None:
        return None
    try:
        return float(value)
    except Exception:
        return None


def _det_distance_m(det: Dict[str, Any]) -> Optional[float]:
    meta = det.get("meta", {}) or {}
    if not isinstance(meta, dict):
        meta = {}
    for key in ("distance_m", "dist_m", "depth_m", "range_m"):
        v = _safe_float(meta.get(key))
        if v is not None and v > 0.0:
            return v
    for key in ("distance_m", "dist_m", "depth_m"):
        v = _safe_float(det.get(key))
        if v is not None and v > 0.0:
            return v
    return None


def _depth_at_detection(relative_depth: np.ndarray, det: Dict[str, Any]) -> Optional[float]:
    bb = det.get("bbox_xyxy")
    if not isinstance(bb, (list, tuple)) or len(bb) != 4:
        return None
    try:
        x1, y1, x2, y2 = map(int, bb)
    except Exception:
        return None
    h, w = relative_depth.shape
    if x2 <= x1 or y2 <= y1:
        return None
    cx = int(round(0.5 * (x1 + x2)))
    cy = int(min(h - 1, max(0, y2)))
    cx = int(min(w - 1, max(0, cx)))
    return float(relative_depth[cy, cx])


def _display_percentiles(depth: np.ndarray, dm: Dict[str, Any]) -> Tuple[float, float]:
    lo = float(dm.get("display_percentile_low", 2.0))
    hi = float(dm.get("display_percentile_high", 98.0))
    valid = np.isfinite(depth)
    if not np.any(valid):
        return 0.0, 1.0
    p_lo, p_hi = np.percentile(depth[valid], [lo, hi])
    if p_hi <= p_lo + 1e-9:
        p_lo, p_hi = float(np.min(depth[valid])), float(np.max(depth[valid]))
    if p_hi <= p_lo + 1e-9:
        p_lo, p_hi = 0.0, 1.0
    return float(p_lo), float(p_hi)


def _smooth_relative_depth(depth: np.ndarray, dm: Dict[str, Any]) -> np.ndarray:
    if depth is None or depth.size == 0:
        return depth
    if not bool(dm.get("smooth_depth", True)):
        return depth.astype(np.float32)

    d = depth.astype(np.float32)
    p_lo, p_hi = _display_percentiles(d, dm)
    span = max(p_hi - p_lo, 1e-6)
    norm = np.clip((d - p_lo) / span, 0.0, 1.0)
    sigma_color = float(dm.get("smooth_sigma_color", 0.06))
    sigma_space = float(dm.get("smooth_sigma_space", 22.0))
    k = int(dm.get("smooth_diameter", 9))
    if k % 2 == 0:
        k += 1
    k = max(5, k)

    smooth = cv2.bilateralFilter(norm, d=k, sigmaColor=sigma_color, sigmaSpace=sigma_space)
    return (smooth * span + p_lo).astype(np.float32)


def _sky_mask(relative_depth: np.ndarray, dm: Dict[str, Any]) -> np.ndarray:
    """Небо: верх кадра + малые значения относительной глубины (дальше от камеры)."""
    if relative_depth is None or relative_depth.size == 0:
        return np.zeros((1, 1), dtype=bool)

    if not bool(dm.get("mask_sky", True)):
        return np.zeros(relative_depth.shape, dtype=bool)

    h, w = relative_depth.shape
    upper_frac = float(dm.get("sky_upper_frac", 0.42))
    depth_pct = float(dm.get("sky_depth_percentile", 28.0))

    y_grid = np.arange(h, dtype=np.float32)[:, None]
    upper = y_grid < (h * upper_frac)
    rel_thr = float(np.percentile(relative_depth, depth_pct))
    far = relative_depth <= rel_thr

    sky = upper & far
    k = int(dm.get("sky_mask_dilate", 5))
    if k % 2 == 0:
        k += 1
    if k >= 3:
        kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (k, k))
        sky_u8 = (sky.astype(np.uint8) * 255)
        sky_u8 = cv2.morphologyEx(sky_u8, cv2.MORPH_CLOSE, kernel)
        sky_u8 = cv2.morphologyEx(sky_u8, cv2.MORPH_OPEN, kernel)
        sky = sky_u8 > 0

    return sky.astype(bool)


def calibrate_depth_to_meters(
        relative_depth: np.ndarray,
        detections: Optional[List[Dict[str, Any]]],
        dm: Dict[str, Any],
) -> Tuple[np.ndarray, Dict[str, Any]]:
    """Depth Anything: больше значение → ближе. Метры: z ≈ k / rel."""
    rel = _smooth_relative_depth(relative_depth, dm)
    sky = _sky_mask(rel, dm)

    if dm.get("metric_scale", True) is False:
        return rel.copy(), {
            "depth_type": "relative",
            "display_near_m": None,
            "display_far_m": None,
        }

    near_m = float(dm.get("fallback_near_m", 5.0))
    far_m = float(dm.get("fallback_far_m", 80.0))
    if far_m <= near_m:
        far_m = near_m + 1.0

    pairs: List[Tuple[float, float]] = []
    for det in detections or []:
        dist_m = _det_distance_m(det)
        rel_det = _depth_at_detection(rel, det)
        if dist_m is None or rel_det is None or rel_det <= 1e-6:
            continue
        pairs.append((rel_det, dist_m))

    cal: Dict[str, Any] = {"calibration_pairs": len(pairs)}

    valid_rel = rel[~sky] if np.any(~sky) else rel
    lo_pct = float(dm.get("display_percentile_low", 2.0))
    hi_pct = float(dm.get("display_percentile_high", 98.0))
    r_lo, r_hi = np.percentile(valid_rel, [lo_pct, hi_pct])
    r_lo, r_hi = float(r_lo), float(r_hi)
    if r_hi <= r_lo + 1e-9:
        r_lo, r_hi = float(np.min(valid_rel)), float(np.max(valid_rel))
    span = max(r_hi - r_lo, 1e-6)

    if pairs:
        ks = [float(m) * float(r) for r, m in pairs]
        k = float(np.median(ks))
        metric = k / np.maximum(rel, 1e-6)
        cal["calibration"] = "detections"
        cal["scale_k"] = round(k, 4)

        det_near = float(np.percentile([m for _, m in pairs], 25))
        det_far = float(np.percentile([m for _, m in pairs], 75))
        display_near_m = max(1.0, min(det_near, float(np.percentile(metric[~sky], 10)) if np.any(~sky) else det_near))
        display_far_m = max(display_near_m + 2.0, max(det_far, float(np.percentile(metric[~sky], 90)) if np.any(~sky) else det_far))
    else:
        # rel↑ → ближе; через 1/rel получаем более равномерный диапазон метров по сцене.
        inv = 1.0 / np.maximum(rel, 1e-3)
        inv_valid = inv[~sky] if np.any(~sky) else inv
        i_lo, i_hi = np.percentile(inv_valid, [lo_pct, hi_pct])
        i_lo, i_hi = float(i_lo), float(i_hi)
        if i_hi <= i_lo + 1e-9:
            i_lo, i_hi = float(np.min(inv_valid)), float(np.max(inv_valid))
        i_span = max(i_hi - i_lo, 1e-9)
        metric = near_m + (far_m - near_m) * (inv - i_lo) / i_span
        cal["calibration"] = "fallback_inverse"
        cal["fallback_near_m"] = near_m
        cal["fallback_far_m"] = far_m
        display_near_m = near_m
        display_far_m = far_m

    if display_near_m > display_far_m:
        display_near_m, display_far_m = display_far_m, display_near_m

    valid = metric[~sky] if np.any(~sky) else metric
    if valid.size > 0:
        p_near = float(np.percentile(valid, float(dm.get("metric_near_percentile", 8.0))))
        p_far = float(np.percentile(valid, float(dm.get("metric_far_percentile", 99.0))))
        display_near_m = max(0.5, min(display_near_m, p_near))
        display_far_m = max(display_far_m, p_far, float(dm.get("fallback_far_m", 80.0)))

    max_metric_m = float(dm.get("max_metric_m", 200.0))
    metric = np.clip(metric, 0.5, max_metric_m).astype(np.float32)
    if np.any(sky):
        metric[sky] = float(display_far_m)

    valid = metric[~sky] if np.any(~sky) else metric
    cal.update({
        "depth_type": "metric",
        "display_near_m": round(float(display_near_m), 1),
        "display_far_m": round(float(display_far_m), 1),
        "metric_min_m": round(float(np.min(valid)), 2),
        "metric_max_m": round(float(np.max(valid)), 2),
        "sky_pixels": int(np.sum(sky)),
    })
    return metric, cal


def _colormap_id(name: str) -> int:
    key = str(name or "inferno").strip().lower()
    mapping = {
        "turbo": cv2.COLORMAP_TURBO,
        "jet": cv2.COLORMAP_JET,
        "viridis": cv2.COLORMAP_VIRIDIS,
        "plasma": cv2.COLORMAP_PLASMA,
        "inferno": cv2.COLORMAP_INFERNO,
        "magma": cv2.COLORMAP_MAGMA,
    }
    return mapping.get(key, cv2.COLORMAP_INFERNO)


class MonocularDepthEstimator:
    """Depth Anything V2 — относительная карта глубины (MDE), не метрическая."""

    def __init__(self, model_id: str, device: str):
        if torch is None or AutoImageProcessor is None or AutoModelForDepthEstimation is None:
            raise RuntimeError(
                "Для карты глубины нужны torch и transformers: "
                "pip install torch transformers"
            )
        self.model_id = str(model_id)
        self.device = _pick_torch_device({"device": device})
        self._processor = AutoImageProcessor.from_pretrained(self.model_id)
        self._model = AutoModelForDepthEstimation.from_pretrained(self.model_id)
        self._model.to(self.device)
        self._model.eval()

    @classmethod
    def from_cfg(cls, cfg: Optional[Dict[str, Any]], state: Optional[Dict[str, Any]] = None) -> "MonocularDepthEstimator":
        dm = _dm_cfg(cfg)
        model_id = str(dm.get("model_id", _DEFAULT_MODEL_ID))
        device = _pick_torch_device(dm)

        if state is not None:
            cached = state.get("monocular_depth_estimator")
            if (
                isinstance(cached, MonocularDepthEstimator)
                and cached.model_id == model_id
                and cached.device == device
            ):
                return cached

        est = cls(model_id=model_id, device=device)
        if state is not None:
            state["monocular_depth_estimator"] = est
        return est

    def predict(self, img_bgr: np.ndarray) -> np.ndarray:
        if img_bgr is None or img_bgr.size == 0:
            raise ValueError("empty image")

        h, w = img_bgr.shape[:2]
        rgb = cv2.cvtColor(img_bgr, cv2.COLOR_BGR2RGB)
        pil = Image.fromarray(rgb)

        inputs = self._processor(images=pil, return_tensors="pt")
        inputs = {k: v.to(self.device) for k, v in inputs.items()}

        with torch.no_grad():
            outputs = self._model(**inputs)
        post = self._processor.post_process_depth_estimation(
            outputs,
            target_sizes=[(h, w)],
        )
        depth = post[0]["predicted_depth"]
        if hasattr(depth, "detach"):
            depth = depth.detach().float().cpu().numpy()
        else:
            depth = np.asarray(depth, dtype=np.float32)

        depth = np.squeeze(depth).astype(np.float32)
        if depth.shape != (h, w):
            depth = cv2.resize(depth, (w, h), interpolation=cv2.INTER_CUBIC)
        return depth


def estimate_relative_depth(
        img_bgr: np.ndarray,
        cfg: Optional[Dict[str, Any]] = None,
        state: Optional[Dict[str, Any]] = None,
) -> Tuple[Optional[np.ndarray], Dict[str, Any]]:
    dm = _dm_cfg(cfg)
    if dm.get("enabled", True) is False:
        return None, {"status": "disabled"}

    try:
        est = MonocularDepthEstimator.from_cfg(cfg, state)
        depth = est.predict(img_bgr)
    except Exception as exc:
        return None, {
            "status": "model_error",
            "error": str(exc),
            "model_id": str(dm.get("model_id", _DEFAULT_MODEL_ID)),
        }

    # Depth Anything: больше значение ≈ ближе к камере (относительная глубина).
    return depth, {
        "status": "ok",
        "model_id": est.model_id,
        "device": est.device,
        "depth_type": "relative",
        "min": float(np.min(depth)),
        "max": float(np.max(depth)),
        "mean": float(np.mean(depth)),
    }


def _normalize_for_display(depth: np.ndarray, dm: Dict[str, Any]) -> np.ndarray:
    p_lo, p_hi = _display_percentiles(depth, dm)
    norm = np.clip((depth - p_lo) / max(p_hi - p_lo, 1e-9), 0.0, 1.0)
    return norm


def _legend_scale_range(
        near_m: Optional[float],
        far_m: Optional[float],
        dm: Dict[str, Any],
        metric_depth: Optional[np.ndarray] = None,
        sky: Optional[np.ndarray] = None,
) -> Tuple[float, float, float]:
    """Шкала 0, step, 2*step, ... с учётом реального разброса глубины по сцене."""
    step = max(1.0, float(dm.get("legend_step_m", 5.0)))
    min_m = max(0.0, float(dm.get("legend_min_m", 0.0)))

    far_f = _safe_float(far_m)
    if far_f is None or far_f <= min_m:
        far_f = float(dm.get("fallback_far_m", 80.0))

    scene_far = far_f
    if metric_depth is not None and metric_depth.size > 0:
        if sky is not None and sky.shape == metric_depth.shape and np.any(~sky):
            valid = metric_depth[~sky]
        else:
            valid = metric_depth.reshape(-1)
        if valid.size > 0:
            scene_far = float(np.percentile(valid, float(dm.get("legend_far_percentile", 99.5))))

    max_override = _safe_float(dm.get("legend_max_m"))
    base_max = max(far_f, scene_far)
    if max_override is not None and max_override > min_m:
        base_max = max(base_max, float(max_override))

    max_m = math.ceil((base_max - min_m) / step) * step + min_m
    max_m = max(max_m, min_m + step * 3)

    return min_m, max_m, step


def _legend_tick_values(min_m: float, max_m: float, step: float) -> List[float]:
    step = max(1.0, float(step))
    ticks: List[float] = []
    v = float(min_m)
    while v <= max_m + 1e-6:
        ticks.append(round(v, 6))
        v += step
    return ticks


def _apply_display_curve(norm: np.ndarray, dm: Dict[str, Any]) -> np.ndarray:
    gamma = float(dm.get("display_gamma", 0.62))
    if gamma > 0.0 and abs(gamma - 1.0) > 1e-6:
        norm = np.power(np.clip(norm, 0.0, 1.0), gamma)
    floor_v = float(dm.get("display_far_floor", 0.10))
    if floor_v > 0.0:
        norm = np.clip(norm, floor_v, 1.0)
        norm = (norm - floor_v) / max(1.0 - floor_v, 1e-6)
    return np.clip(norm, 0.0, 1.0).astype(np.float32)


def metric_to_norm(
        metric_m: np.ndarray,
        scale_min_m: float,
        scale_max_m: float,
        sky: Optional[np.ndarray] = None,
        dm: Optional[Dict[str, Any]] = None,
) -> np.ndarray:
    """Ближе -> ярче; log-кривая + gamma для плавного перехода к дальнему плану."""
    dm = dm or {}
    lo = float(scale_min_m)
    hi = float(scale_max_m)
    if hi <= lo:
        hi = lo + 1.0

    eps = 1e-3
    m = np.maximum(metric_m.astype(np.float64), eps)
    curve = str(dm.get("display_curve", "log")).strip().lower()

    if curve == "linear":
        norm = (hi - m) / max(hi - lo, eps)
    else:
        log_lo = math.log(lo + eps)
        log_hi = math.log(hi + eps)
        norm = (log_hi - np.log(m)) / max(log_hi - log_lo, eps)

    norm = np.clip(norm, 0.0, 1.0)
    norm = _apply_display_curve(norm, dm)

    if sky is not None and sky.shape == metric_m.shape:
        sky_norm = float(dm.get("sky_display_norm", 0.14))
        norm[sky] = sky_norm

    return norm.astype(np.float32)


def _norm_at_meters(meters: float, scale_min_m: float, scale_max_m: float, dm: Dict[str, Any]) -> float:
    arr = np.array([[float(meters)]], dtype=np.float32)
    out = metric_to_norm(arr, scale_min_m, scale_max_m, sky=None, dm=dm)
    return float(out[0, 0])


def depth_to_colormap_bgr(
        depth: np.ndarray,
        dm: Dict[str, Any],
        is_metric: bool = False,
        scale_min_m: Optional[float] = None,
        scale_max_m: Optional[float] = None,
        sky: Optional[np.ndarray] = None,
) -> np.ndarray:
    cmap_id = _colormap_id(str(dm.get("colormap", "inferno")))

    if is_metric and scale_min_m is not None and scale_max_m is not None:
        norm = metric_to_norm(depth, scale_min_m, scale_max_m, sky=sky, dm=dm)
    else:
        norm = _normalize_for_display(depth, dm)
        norm = _apply_display_curve(norm, dm)

    if bool(dm.get("invert_display", False)):
        norm = 1.0 - norm

    u8 = (norm * 255.0).astype(np.uint8)
    colored = cv2.applyColorMap(u8, cmap_id)

    if sky is not None and sky.shape == depth.shape:
        sky_color = tuple(int(c) for c in dm.get("sky_color_bgr", [18, 18, 24]))
        colored[sky] = sky_color

    return colored


def _legend_tick_label(meters: float) -> str:
    """Только ASCII — cv2.putText не рисует кириллицу."""
    v = float(meters)
    if abs(v - round(v)) < 0.05:
        return str(int(round(v)))
    return f"{v:.1f}"


def _legend_bar(
        height: int,
        cmap_id: int,
        scale_min_m: float,
        scale_max_m: float,
        step_m: float,
        dm: Dict[str, Any],
) -> np.ndarray:
    bar_w = max(220, int(round(height * 0.16)))
    pad_top = max(52, int(round(height * 0.07)))
    pad_bot = max(40, int(round(height * 0.05)))
    plot_h = max(1, height - pad_top - pad_bot)

    bar = np.full((height, bar_w, 3), 248, dtype=np.uint8)
    invert = bool(dm.get("invert_display", False))

    strip_w = max(36, int(bar_w * 0.22))
    x0 = bar_w - strip_w - 14

    min_m = float(scale_min_m)
    max_m = float(scale_max_m)
    span_m = max(max_m - min_m, 1e-6)

    for y in range(plot_h):
        meters = min_m + (1.0 - y / max(1, plot_h - 1)) * span_m
        norm_val = _norm_at_meters(meters, min_m, max_m, dm)
        if invert:
            norm_val = 1.0 - norm_val
        c = cv2.applyColorMap(np.array([[int(norm_val * 255.0)]], dtype=np.uint8), cmap_id)[0, 0]
        y_abs = pad_top + y
        bar[y_abs, x0:x0 + strip_w] = c

    cv2.rectangle(bar, (x0, pad_top), (x0 + strip_w - 1, pad_top + plot_h - 1), (50, 50, 50), 2, cv2.LINE_AA)

    font = cv2.FONT_HERSHEY_DUPLEX
    scale = max(1.05, min(1.85, height / 520.0))
    thick = max(2, int(round(scale * 1.8)))
    outline = thick + 2

    ticks = _legend_tick_values(min_m, max_m, step_m)
    for meters in ticks:
        frac = (meters - min_m) / span_m
        y_abs = int(round(pad_top + (1.0 - frac) * (plot_h - 1)))
        label = _legend_tick_label(meters)
        cv2.line(bar, (x0 - 8, y_abs), (x0 + strip_w + 6, y_abs), (180, 180, 180), 2, cv2.LINE_AA)
        text_y = min(height - 10, y_abs + int(8 * scale))
        cv2.putText(bar, label, (10, text_y), font, scale, (0, 0, 0), outline, cv2.LINE_AA)
        cv2.putText(bar, label, (10, text_y), font, scale, (25, 25, 25), thick, cv2.LINE_AA)

    title = str(dm.get("legend_title", "Distance (m)"))
    title_scale = scale * 0.95
    cv2.putText(bar, title, (10, 34), font, title_scale, (0, 0, 0), outline, cv2.LINE_AA)
    cv2.putText(bar, title, (10, 34), font, title_scale, (20, 20, 20), thick, cv2.LINE_AA)

    unit = str(dm.get("legend_unit_suffix", "m"))
    cv2.putText(bar, unit, (10, height - 12), font, scale * 0.85, (70, 70, 70), thick, cv2.LINE_AA)

    return bar


def render_depth_map_plot(
        img_bgr: np.ndarray,
        detections: Optional[List[Dict[str, Any]]] = None,
        cfg: Optional[Dict[str, Any]] = None,
        state: Optional[Dict[str, Any]] = None,
        depth: Optional[np.ndarray] = None,
        meta: Optional[Dict[str, Any]] = None,
        metric_depth: Optional[np.ndarray] = None,
) -> Tuple[Optional[np.ndarray], Dict[str, Any]]:
    if img_bgr is None or img_bgr.size == 0:
        return None, {"status": "empty_image"}

    dm = _dm_cfg(cfg)
    if depth is None:
        depth, meta = estimate_relative_depth(img_bgr, cfg, state)
        if depth is None:
            return None, meta or {}
    meta = dict(meta or {})

    is_metric = meta.get("depth_type") == "metric"
    if metric_depth is None:
        metric_depth, cal_meta = calibrate_depth_to_meters(depth, detections, dm)
        meta.update(cal_meta)
        is_metric = meta.get("depth_type") == "metric"

    rel_smooth = _smooth_relative_depth(depth, dm)
    sky = _sky_mask(rel_smooth, dm)

    display_depth = metric_depth if is_metric else rel_smooth
    near_m = _safe_float(meta.get("display_near_m"))
    far_m = _safe_float(meta.get("display_far_m"))
    scale_min_m, scale_max_m, step_m = _legend_scale_range(
        near_m, far_m, dm, metric_depth=metric_depth if is_metric else None, sky=sky if is_metric else None,
    )
    meta["legend_scale_min_m"] = scale_min_m
    meta["legend_scale_max_m"] = scale_max_m
    meta["legend_step_m"] = step_m

    colored = depth_to_colormap_bgr(
        display_depth,
        dm,
        is_metric=is_metric,
        scale_min_m=scale_min_m,
        scale_max_m=scale_max_m,
        sky=sky if is_metric else None,
    )
    cmap_id = _colormap_id(str(dm.get("colormap", "inferno")))

    blend = float(dm.get("blend_original", 0.0))
    blend = max(0.0, min(1.0, blend))
    if blend > 0.0:
        colored = cv2.addWeighted(img_bgr, blend, colored, 1.0 - blend, 0.0)

    panels = [colored]
    if bool(dm.get("side_by_side", True)):
        thumb = img_bgr.copy()
        if thumb.shape[:2] != colored.shape[:2]:
            thumb = cv2.resize(thumb, (colored.shape[1], colored.shape[0]))
        panels.insert(0, thumb)

    body = np.hstack(panels)
    bar = _legend_bar(body.shape[0], cmap_id, scale_min_m, scale_max_m, step_m, dm)
    out = np.hstack([body, bar])

    meta["status"] = "ok"
    meta["colormap"] = str(dm.get("colormap", "inferno"))
    return out, meta


def save_depth_map_plot(
        img_bgr: np.ndarray,
        detections: Optional[List[Dict[str, Any]]] = None,
        out_dir: str = "",
        cfg: Optional[Dict[str, Any]] = None,
        state: Optional[Dict[str, Any]] = None,
) -> Tuple[str, Dict[str, Any]]:
    dm = _dm_cfg(cfg)
    plot_name = str(dm.get("plot_filename", "depth_map_plot.png"))
    out_path = os.path.abspath(os.path.join(out_dir, plot_name))

    depth, meta = estimate_relative_depth(img_bgr, cfg, state)
    if depth is None:
        return "", meta

    metric_depth, cal_meta = calibrate_depth_to_meters(depth, detections, dm)
    meta.update(cal_meta)

    os.makedirs(out_dir, exist_ok=True)

    if bool(dm.get("save_npy", True)):
        npy_name = str(dm.get("npy_filename", "depth_map.npy"))
        npy_path = os.path.abspath(os.path.join(out_dir, npy_name))
        np.save(npy_path, depth)
        meta["depth_map_npy_path"] = npy_path
        if meta.get("depth_type") == "metric":
            metric_npy = str(dm.get("metric_npy_filename", "depth_map_metric_m.npy"))
            metric_path = os.path.abspath(os.path.join(out_dir, metric_npy))
            np.save(metric_path, metric_depth)
            meta["depth_map_metric_m_npy_path"] = metric_path

    if bool(dm.get("save_grayscale", True)):
        gray_name = str(dm.get("gray_filename", "depth_map_gray.png"))
        gray_path = os.path.abspath(os.path.join(out_dir, gray_name))
        norm = _normalize_for_display(depth, dm)
        cv2.imwrite(gray_path, (norm * 255.0).astype(np.uint8))
        meta["depth_map_gray_path"] = gray_path

    plot, plot_meta = render_depth_map_plot(
        img_bgr,
        detections,
        cfg,
        state,
        depth=depth,
        meta=meta,
        metric_depth=metric_depth,
    )
    meta.update(plot_meta)
    if plot is None:
        return "", meta

    if not cv2.imwrite(out_path, plot):
        return "", {**meta, "status": "write_failed", "path": out_path}

    meta["path"] = out_path

    meta["depth_model_mode"] = "pretrained"
    meta["depth_model_trains_in_app"] = True

    training_paths, training_meta = save_depth_training_plots(out_dir, cfg)
    meta["depth_training_plots"] = training_meta
    if training_meta.get("status") == "no_training_history":
        meta["depth_model_trains_in_app"] = False
        meta["depth_training_note"] = (
            "Depth map uses pretrained Depth Anything (no training in this run). "
            "Set depth_map.training_history_path to your training log JSON to show accuracy plots."
        )
    for key, path in training_paths.items():
        meta[key] = path

    return out_path, meta
