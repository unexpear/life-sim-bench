"""Shiu/Brian2 reference helper callable from the fly actor tooling.

Loads optional local FlyWire pack (CC BY-NC) and runs a tiny Brian2 network so
the Windows path is proven end-to-end. This is NOT an embodied controller and
does not speak the Observation/Action contract yet — see fly_brian2.hpp.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def default_pack() -> Path:
    env = os.environ.get("LIFESIM_FLYWIRE_PACK", "").strip()
    if env:
        return Path(env)
    return repo_root() / "userdata" / "packs" / "flywire-nc"


def load_manifest(pack: Path) -> dict:
    man = pack / "MANIFEST.json"
    if not man.is_file():
        return {}
    return json.loads(man.read_text(encoding="utf-8"))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--pack", type=Path, default=None, help="FlyWire local pack dir")
    ap.add_argument("--skip-network", action="store_true", help="Only check pack + import")
    args = ap.parse_args()

    pack = args.pack or default_pack()
    print(f"pack_dir={pack}")
    man = load_manifest(pack)
    if man:
        print(f"dataset_hash={man.get('dataset_hash','')}")
        print(f"dataset={man.get('dataset','')}")
        print(f"license={man.get('license','')}")
        if man.get("not_for_installer") is not True:
            print("WARNING: manifest missing not_for_installer=true", file=sys.stderr)
    else:
        print("manifest=missing (pack optional; synthetic flyarena still runs)")

    con = pack / "Connectivity_783.parquet"
    comp = pack / "Completeness_783.csv"
    if con.is_file() and comp.is_file():
        print(f"connectivity_bytes={con.stat().st_size}")
        print(f"completeness_bytes={comp.stat().st_size}")
        try:
            import pandas as pd

            # Schema probe only — do not load full tables into a brain here.
            head = pd.read_parquet(con, columns=None)
            print(f"connectivity_rows={len(head)} cols={list(head.columns)[:8]}")
        except Exception as e:
            print(f"parquet_probe_failed={type(e).__name__}: {e}", file=sys.stderr)
    else:
        print("connectivity=missing")

    import brian2

    print(f"brian2={brian2.__version__}")
    if args.skip_network:
        return 0

    from brian2 import NeuronGroup, prefs, run, set_device

    set_device("runtime")
    # Prefer whatever works in this shell; setup script documents MSVC for Cython.
    G = NeuronGroup(2, "dv/dt = -v / (10*ms) : 1")
    run(0.5 * brian2.ms)
    print(f"brian2_network_ok target={prefs.codegen.target}")
    print("note=reference helper only; ReactiveController remains the registered flyarena brain")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
