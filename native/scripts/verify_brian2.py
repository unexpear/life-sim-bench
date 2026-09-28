"""Verify the local Brian2 Windows runtime (import + optional Cython codegen)."""
from __future__ import annotations

import argparse
import os
import sys


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cython", action="store_true", help="Require Cython codegen (needs MSVC on PATH).")
    p.add_argument("--numpy-only", action="store_true", help="Force numpy codegen target.")
    args = p.parse_args()

    import brian2

    print(f"brian2 {brian2.__version__}")
    print(f"python {sys.version.split()[0]}")
    print(f"executable {sys.executable}")

    from brian2 import NeuronGroup, prefs, run, set_device

    set_device("runtime")
    if args.numpy_only:
        prefs.codegen.target = "numpy"
    elif args.cython:
        prefs.codegen.target = "cython"
    # else: auto

    G = NeuronGroup(3, "dv/dt = -v / (10*ms) : 1")
    run(1 * brian2.ms)
    print(f"runtime ok target={prefs.codegen.target}")
    if args.cython and prefs.codegen.target != "cython":
        print("ERROR: Cython target was requested but not used", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as e:
        print(f"VERIFY FAILED: {type(e).__name__}: {e}", file=sys.stderr)
        if "Cython" in str(e) or "compiler" in str(e).lower():
            print(
                "Hint: open a VS x64 Developer shell (vcvars64.bat) or install "
                "MSVC Build Tools; without it use --numpy-only.",
                file=sys.stderr,
            )
        raise SystemExit(1)
