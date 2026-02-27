import argparse
import json
import os
import sys
import traceback
import time
from io import StringIO
from pathlib import Path
from typing import Any, Dict, List, Optional

from result_contract import build_result

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT))


def read_json(path: str) -> Dict[str, Any]:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def write_json(path: str, obj: Dict[str, Any]) -> None:
    with open(path, "w", encoding="utf-8") as f:
        json.dump(obj, f, ensure_ascii=False, indent=2)


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(prog="runner.py")
    ap.add_argument("--task", required=True, choices=["cluster", "distance_full"])
    ap.add_argument("--cluster-id", type=int, default=0)
    ap.add_argument("--input", required=True)
    ap.add_argument("--output-dir", required=True)
    ap.add_argument("--device", default="auto", choices=["auto", "gpu", "cpu"])
    ap.add_argument("--config", required=True)
    ap.add_argument("--result-json", required=True)
    return ap


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


def _split_lines(s: str) -> List[str]:
    if not s:
        return []
    return s.splitlines()


def main() -> int:
    ap = build_parser()

    if len(sys.argv) == 1:
        ap.print_help()
        print("Этот скрипт запускается из Qt-приложения vk_qt_app.exe")
        print("python runner.py --task cluster --cluster-id 1 --input ПУТЬ_К_КАРТИНКЕ --output-dir ПУТЬ_К_out --device auto --config ПУТЬ_К_default_config.json --result-json ПУТЬ_К_result.json")
        return 0

    if any(a in ("-h", "--help") for a in sys.argv[1:]):
        ap.print_help()
        return 0

    args = ap.parse_args()

    cfg = read_json(args.config)
    os.makedirs(args.output_dir, exist_ok=True)

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
                    raise SystemExit("cluster-id must be 1..4")

                mod_name = f"prototypes.cluster_{args.cluster_id}"
                mod = __import__(mod_name, fromlist=["run"])
                module_result = mod.run(
                    image_path=args.input,
                    out_dir=args.output_dir,
                    cfg=cfg,
                    device_mode=args.device
                )
            else:
                from full_distance.full_distance import run as run_full
                module_result = run_full(
                    image_path=args.input,
                    out_dir=args.output_dir,
                    cfg=cfg,
                    device_mode=args.device
                )

    except SystemExit:
        raise
    except Exception as e:
        exit_code = 1
        tb = traceback.format_exc()
        err_obj = {
            "type": type(e).__name__,
            "message": str(e),
            "traceback": tb,
        }

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
    raise SystemExit(main())