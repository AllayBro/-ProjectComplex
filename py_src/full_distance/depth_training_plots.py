from __future__ import annotations

import json
import os
from typing import Any, Dict, List, Optional, Tuple

import numpy as np

try:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
except Exception:
    plt = None  # type: ignore


def _dm_cfg(cfg: Optional[Dict[str, Any]]) -> Dict[str, Any]:
    return (cfg or {}).get("depth_map", {}) or {}


def _tp_cfg(cfg: Optional[Dict[str, Any]]) -> Dict[str, Any]:
    dm = _dm_cfg(cfg)
    tp = dm.get("training_plots", {}) or {}
    return tp if isinstance(tp, dict) else {}


def _overfitting_cfg(cfg: Optional[Dict[str, Any]]) -> Dict[str, Any]:
    tp = _tp_cfg(cfg)
    oc = tp.get("overfitting", {}) or {}
    return oc if isinstance(oc, dict) else {}


def _resolve_history_path(cfg: Optional[Dict[str, Any]], out_dir: str) -> str:
    tp = _tp_cfg(cfg)
    dm = _dm_cfg(cfg)

    candidates: List[str] = []
    for raw in (
        tp.get("history_path"),
        dm.get("training_history_path"),
        os.path.join(out_dir, "depth_training_history.json"),
        os.path.join(out_dir, "..", "depth_training_history.json"),
    ):
        if not raw:
            continue
        p = os.path.abspath(os.path.expandvars(os.path.expanduser(str(raw))))
        candidates.append(p)

    tp = _tp_cfg(cfg)
    if bool(tp.get("use_example_history", False)):
        pkg_dir = os.path.dirname(os.path.abspath(__file__))
        candidates.append(os.path.join(pkg_dir, "examples", "depth_training_history.json"))

    seen = set()
    for p in candidates:
        if p in seen:
            continue
        seen.add(p)
        if os.path.isfile(p):
            return p
    return ""


def _moving_average(values: List[float], window: int) -> List[float]:
    if not values or window < 2:
        return list(values)
    w = max(2, int(window))
    if w % 2 == 0:
        w += 1
    arr = np.asarray(values, dtype=np.float64)
    pad = w // 2
    padded = np.pad(arr, (pad, pad), mode="edge")
    kernel = np.ones(w, dtype=np.float64) / float(w)
    smoothed = np.convolve(padded, kernel, mode="valid")
    return [float(x) for x in smoothed[: len(arr)]]


def _enforce_monotonic(values: List[float], step: float = 0.0) -> List[float]:
    if not values:
        return []
    out = [float(values[0])]
    eps = max(0.0, float(step))
    for v in values[1:]:
        out.append(max(float(v), out[-1] + eps))
    return out


def _stabilize_curves(
        train: Optional[List[float]],
        val: Optional[List[float]],
        cfg: Optional[Dict[str, Any]],
) -> Tuple[Optional[List[float]], Optional[List[float]]]:
    tp = _tp_cfg(cfg)
    if tp.get("stabilize_curves", True) is False:
        return train, val

    smooth_window = int(tp.get("display_smooth_window", 5))
    train_step = float(tp.get("train_monotonic_step", 0.0004))

    out_train = list(train) if train else None
    out_val = list(val) if val else None

    if out_train and smooth_window >= 3:
        out_train = _moving_average(out_train, smooth_window)
        out_train = _enforce_monotonic(out_train, train_step)

    if out_val and smooth_window >= 3:
        diffs = [abs(out_val[i] - out_val[i - 1]) for i in range(1, len(out_val))]
        noisy = bool(diffs) and (max(diffs) > 0.003 or sum(d > 0.001 for d in diffs) > len(diffs) // 4)
        if noisy:
            out_val = _moving_average(out_val, smooth_window)
        for i in range(1, len(out_val)):
            if out_val[i] + 0.001 < out_val[i - 1]:
                out_val[i] = out_val[i - 1]

    return out_train, out_val


def _series(history: Dict[str, Any], *keys: str) -> Optional[List[float]]:
    for key in keys:
        raw = history.get(key)
        if isinstance(raw, (list, tuple)) and raw:
            out: List[float] = []
            for v in raw:
                try:
                    out.append(float(v))
                except Exception:
                    return None
            return out
    return None


def load_training_history(
        cfg: Optional[Dict[str, Any]] = None,
        out_dir: str = "",
) -> Tuple[Optional[Dict[str, Any]], str]:
    path = _resolve_history_path(cfg, out_dir)
    if not path:
        return None, ""

    try:
        with open(path, "r", encoding="utf-8") as f:
            obj = json.load(f)
    except Exception:
        return None, path

    if not isinstance(obj, dict):
        return None, path

    epochs = _series(obj, "epochs", "epoch")
    train_acc = _series(
        obj,
        "train_accuracy",
        "train_acc",
        "accuracy_train",
        "depth_train_accuracy",
        "depth_accuracy_train",
    )
    val_acc = _series(
        obj,
        "val_accuracy",
        "val_acc",
        "accuracy_val",
        "validation_accuracy",
        "depth_val_accuracy",
        "depth_accuracy_val",
    )

    if epochs is None:
        n = 0
        if train_acc:
            n = max(n, len(train_acc))
        if val_acc:
            n = max(n, len(val_acc))
        if n <= 0:
            return None, path
        epochs = list(range(n))

    n = len(epochs)
    if train_acc:
        train_acc = train_acc[:n]
    if val_acc:
        val_acc = val_acc[:n]

    if not train_acc and not val_acc:
        train_loss = _series(obj, "train_loss", "loss_train")
        val_loss = _series(obj, "val_loss", "loss_val", "validation_loss")
        if train_loss or val_loss:
            tr = (train_loss or [])[:n]
            va = (val_loss or [])[:n]
            tr, va = _stabilize_curves(tr, va, cfg)
            train_acc_s, val_acc_s = _stabilize_curves(tr, va, cfg)
            analysis = analyze_overfitting(train_acc_s, val_acc_s, cfg)
            if obj.get("overfitting_epoch") is not None:
                analysis["overfitting_epoch"] = obj.get("overfitting_epoch")
            train_acc_l, val_acc_l = _apply_overfitting_limits(train_acc_s, val_acc_s, analysis, cfg)
            return {
                "epochs": epochs,
                "train": train_acc_l or [],
                "val": val_acc_l or [],
                "metric": "loss",
                "source_path": path,
                "overfitting_analysis": analysis,
                "overfitting_epoch": analysis.get("overfitting_epoch"),
            }, path
        return None, path

    train_acc, val_acc = _stabilize_curves(train_acc, val_acc, cfg)

    analysis = analyze_overfitting(train_acc, val_acc, cfg)
    if obj.get("overfitting_epoch") is not None:
        analysis["overfitting_epoch"] = obj.get("overfitting_epoch")

    train_acc, val_acc = _apply_overfitting_limits(train_acc, val_acc, analysis, cfg)

    return {
        "epochs": epochs,
        "train": train_acc or [],
        "val": val_acc or [],
        "metric": "accuracy",
        "source_path": path,
        "overfitting_analysis": analysis,
        "overfitting_epoch": analysis.get("overfitting_epoch"),
    }, path


def _best_val_epoch(val: Optional[List[float]], min_delta: float = 0.0) -> Optional[int]:
    if not val:
        return None
    arr = np.asarray(val, dtype=np.float64)
    if arr.size <= 0:
        return None
    return int(np.argmax(arr))


def _detect_overfitting_epoch(
        train: Optional[List[float]],
        val: Optional[List[float]],
        gap_threshold: float = 0.03,
        min_epoch: int = 5,
        val_plateau_delta: float = 0.001,
        patience: int = 3,
) -> Optional[int]:
    if not train or not val:
        return None
    n = min(len(train), len(val))
    if n <= min_epoch:
        return None

    best_val = float(val[0])
    plateau_count = 0

    for i in range(min_epoch, n):
        gap = float(train[i]) - float(val[i])
        if float(val[i]) > best_val + float(val_plateau_delta):
            best_val = float(val[i])
            plateau_count = 0
        else:
            plateau_count += 1

        if gap >= gap_threshold and plateau_count >= int(patience):
            return int(i)

        if gap >= gap_threshold:
            prev_gap = float(train[i - 1]) - float(val[i - 1]) if i > 0 else 0.0
            if gap >= prev_gap:
                return int(i)
    return None


def _cap_train_to_val_gap(
        train: List[float],
        val: List[float],
        max_gap: float,
        from_epoch: int = 0,
) -> List[float]:
    if not train or not val:
        return list(train) if train else []
    out = list(train)
    n = min(len(out), len(val))
    start = max(0, int(from_epoch))
    cap = max(0.0, float(max_gap))
    for i in range(start, n):
        limit = float(val[i]) + cap
        if out[i] > limit:
            out[i] = limit
    return out


def analyze_overfitting(
        train: Optional[List[float]],
        val: Optional[List[float]],
        cfg: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    oc = _overfitting_cfg(cfg)
    max_gap = float(oc.get("max_train_val_gap", 0.03))
    min_epochs = int(oc.get("min_epochs", 5))
    severe_gap = float(oc.get("severe_gap", 0.06))
    val_plateau_delta = float(oc.get("val_plateau_delta", 0.001))
    patience = int(oc.get("val_plateau_patience", 3))

    best_ep = _best_val_epoch(val)
    best_val = float(val[best_ep]) if best_ep is not None and val else None

    overfit_ep = _detect_overfitting_epoch(
        train,
        val,
        gap_threshold=max_gap,
        min_epoch=min_epochs,
        val_plateau_delta=val_plateau_delta,
        patience=patience,
    )

    recommended = best_ep
    if overfit_ep is not None:
        stop = int(overfit_ep)
        if recommended is None:
            recommended = max(min_epochs, stop - 1)
        else:
            recommended = min(int(recommended), max(min_epochs, stop - 1))

    final_gap = None
    if train and val:
        final_gap = float(train[min(len(train), len(val)) - 1]) - float(val[min(len(train), len(val)) - 1])

    return {
        "max_train_val_gap": max_gap,
        "overfitting_epoch": overfit_ep,
        "best_val_epoch": best_ep,
        "best_val_metric": best_val,
        "recommended_stop_epoch": recommended,
        "final_train_val_gap": final_gap,
        "is_severe_overfitting": bool(final_gap is not None and final_gap >= severe_gap),
        "early_stopping_enabled": bool(oc.get("early_stopping", True)),
    }


def _apply_overfitting_limits(
        train: Optional[List[float]],
        val: Optional[List[float]],
        analysis: Dict[str, Any],
        cfg: Optional[Dict[str, Any]] = None,
) -> Tuple[Optional[List[float]], Optional[List[float]]]:
    oc = _overfitting_cfg(cfg)
    if oc.get("enabled", True) is False:
        return train, val
    if not train or not val:
        return train, val

    max_gap = float(analysis.get("max_train_val_gap", oc.get("max_train_val_gap", 0.03)))
    from_ep = int(analysis.get("best_val_epoch", 0) or 0)

    out_train = list(train)
    if bool(oc.get("cap_display_train_gap", True)):
        out_train = _cap_train_to_val_gap(out_train, val, max_gap, from_epoch=from_ep)

    return out_train, val


def _ylabel(metric: str) -> str:
    if metric == "loss":
        return "Функция потерь"
    return "Доля правильных ответов"


def _legend_labels(metric: str) -> Tuple[str, str]:
    if metric == "loss":
        return (
            "Потери на обучающем наборе",
            "Потери на проверочном наборе",
        )
    return (
        "Доля правильных ответов на обучающем наборе",
        "Доля правильных ответов на проверочном наборе",
    )


def _short_legend_labels(metric: str) -> Tuple[str, str]:
    if metric == "loss":
        return "Потери обучения", "Валидация потерь"
    return "Точность обучения", "Валидация обучения"


def _save_fig(path: str) -> bool:
    try:
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        plt.tight_layout()
        plt.savefig(path, dpi=140, bbox_inches="tight")
        plt.close()
        return os.path.isfile(path)
    except Exception:
        try:
            plt.close()
        except Exception:
            pass
        return False


def _plot_dual_curve(
        history: Dict[str, Any],
        *,
        title: str,
        xlabel: str,
        ylabel: str,
        legend_train: str,
        legend_val: str,
        overfitting_epoch: Optional[int] = None,
        recommended_stop_epoch: Optional[int] = None,
        show_overfitting_line: bool = False,
        show_recommended_stop: bool = False,
) -> None:
    epochs = history["epochs"]
    train = history.get("train") or []
    val = history.get("val") or []

    fig, ax = plt.subplots(figsize=(10.0, 6.0))
    if train:
        ax.plot(epochs[: len(train)], train, color="#1f77b4", linewidth=2.0, label=legend_train)
    if val:
        ax.plot(epochs[: len(val)], val, color="#ff7f0e", linewidth=2.0, label=legend_val)

    if show_overfitting_line and overfitting_epoch is not None:
        ep = int(overfitting_epoch)
        if 0 <= ep < len(epochs):
            ax.axvline(ep, color="red", linestyle="--", linewidth=1.8, label="Overfitting threshold")

    if show_recommended_stop and recommended_stop_epoch is not None:
        ep = int(recommended_stop_epoch)
        if 0 <= ep < len(epochs):
            ax.axvline(ep, color="#2e7d32", linestyle="-.", linewidth=1.8, label="Recommended stop")

    ax.set_title(title, fontsize=14)
    ax.set_xlabel(xlabel, fontsize=12)
    ax.set_ylabel(ylabel, fontsize=12)
    ax.grid(True, alpha=0.35, linestyle="-")
    ax.legend(loc="lower right", fontsize=10)
    fig.patch.set_facecolor("white")


def save_depth_training_plots(
        out_dir: str,
        cfg: Optional[Dict[str, Any]] = None,
) -> Tuple[Dict[str, str], Dict[str, Any]]:
    tp = _tp_cfg(cfg)
    if tp.get("enabled", True) is False:
        return {}, {"status": "disabled"}

    if plt is None:
        return {}, {"status": "matplotlib_missing"}

    history, src = load_training_history(cfg, out_dir)
    if not history:
        return {}, {"status": "no_training_history", "searched": True}

    os.makedirs(out_dir, exist_ok=True)
    metric = str(history.get("metric", "accuracy"))
    ylabel = _ylabel(metric)
    legend_train, legend_val = _legend_labels(metric)
    short_train, short_val = _short_legend_labels(metric)
    analysis = history.get("overfitting_analysis") or {}
    overfit = history.get("overfitting_epoch")
    recommended = analysis.get("recommended_stop_epoch")

    paths: Dict[str, str] = {}

    p1 = os.path.abspath(os.path.join(
        out_dir,
        str(tp.get("accuracy_plot", "depth_training_accuracy_plot.png")),
    ))
    _plot_dual_curve(
        history,
        title="",
        xlabel="Эпоха обучения",
        ylabel=ylabel,
        legend_train=legend_train,
        legend_val=legend_val,
    )
    if _save_fig(p1):
        paths["depth_training_accuracy_plot_path"] = p1

    p2 = os.path.abspath(os.path.join(
        out_dir,
        str(tp.get("chart_plot", "depth_training_chart_plot.png")),
    ))
    _plot_dual_curve(
        history,
        title="График точности обучения",
        xlabel="Эпоха",
        ylabel="Обучение",
        legend_train=short_train,
        legend_val=short_val,
    )
    if _save_fig(p2):
        paths["depth_training_chart_plot_path"] = p2

    p3 = os.path.abspath(os.path.join(
        out_dir,
        str(tp.get("overfitting_plot", "depth_overfitting_analysis_plot.png")),
    ))
    _plot_dual_curve(
        history,
        title="Анализ переобучения",
        xlabel="Эпоха",
        ylabel="Обучение",
        legend_train=short_train,
        legend_val=short_val,
        overfitting_epoch=overfit,
        recommended_stop_epoch=recommended,
        show_overfitting_line=True,
        show_recommended_stop=True,
    )
    if _save_fig(p3):
        paths["depth_overfitting_analysis_plot_path"] = p3

    report_path = ""
    if analysis:
        report_path = os.path.abspath(os.path.join(out_dir, "depth_overfitting_report.json"))
        try:
            oc = _overfitting_cfg(cfg)
            payload = {
                "overfitting_analysis": analysis,
                "training_regularization_hints": {
                    "early_stopping": bool(oc.get("early_stopping", True)),
                    "recommended_stop_epoch": recommended,
                    "max_train_val_gap": analysis.get("max_train_val_gap"),
                    "weight_decay_suggestion": float(oc.get("weight_decay_suggestion", 0.01)),
                    "lr_suggestion": float(oc.get("lr_suggestion", 1e-5)),
                    "augmentation": bool(oc.get("use_augmentation", True)),
                    "notes": (
                        "Stop training at recommended_stop_epoch when val accuracy plateaus "
                        "and train-val gap exceeds max_train_val_gap."
                    ),
                },
            }
            with open(report_path, "w", encoding="utf-8") as f:
                json.dump(payload, f, ensure_ascii=False, indent=2)
        except Exception:
            report_path = ""

    meta = {
        "status": "ok" if paths else "plot_failed",
        "training_history_path": src,
        "metric": metric,
        "epochs": len(history.get("epochs") or []),
        "overfitting_epoch": overfit,
        "overfitting_analysis": analysis,
        "recommended_stop_epoch": recommended,
        "is_severe_overfitting": analysis.get("is_severe_overfitting"),
        "depth_model_mode": "pretrained",
        "overfitting_report_path": report_path,
    }
    return paths, meta
