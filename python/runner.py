import argparse
import json
import os
import sys
import traceback
from pathlib import Path
from typing import Any, Dict

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT))


def read_json(path: str) -> Dict[str, Any]:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def write_json(path: str, obj: Dict[str, Any]) -> None:
    with open(path, "w", encoding="utf-8") as f:
        json.dump(obj, f, ensure_ascii=False, indent=2)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--task", required=True, choices=["cluster", "distance_full"])
    ap.add_argument("--cluster-id", type=int, default=0)
    ap.add_argument("--input", required=True)
    ap.add_argument("--output-dir", required=True)
    ap.add_argument("--device", default="auto", choices=["auto", "gpu", "cpu"])
    ap.add_argument("--config", required=True)
    ap.add_argument("--result-json", required=True)
    args = ap.parse_args()

    cfg = read_json(args.config)
    os.makedirs(args.output_dir, exist_ok=True)

    try:
        if args.task == "cluster":
            if args.cluster_id not in (1, 2, 3, 4):
                raise SystemExit("cluster-id must be 1..4")

            mod_name = f"prototypes.cluster_{args.cluster_id}"
            mod = __import__(mod_name, fromlist=["run"])
            res = mod.run(
                image_path=args.input,
                out_dir=args.output_dir,
                cfg=cfg,
                device_mode=args.device
            )
        else:
            from full_distance.full_distance import run as run_full
            res = run_full(
                image_path=args.input,
                out_dir=args.output_dir,
                cfg=cfg,
                device_mode=args.device
            )

        write_json(args.result_json, res)
        print("OK")
    except SystemExit:
        raise
    except Exception:
        traceback.print_exc()
        raise


if __name__ == "__main__":
    main()