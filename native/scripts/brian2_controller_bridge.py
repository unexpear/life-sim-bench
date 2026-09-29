"""Observation/Action bridge for an embodied Brian2 controller.

Speaks the fly actor contract (native/src/actors/fly.hpp) over stdin/stdout as
JSON lines. This is an engineered rate-coded LIF mapping for embodiment tests —
not a Shiu full-connectome reproduction and not FlyWire NC data in the loop.

Protocol (one JSON object per line):
  {"cmd":"reset","seed":1,"odor_gain":1.0,"light_gain":0.0}
  {"cmd":"act","tick":0,"seconds":0.0,"interval":0.0166667,
   "odor":[0.1,0.2],"light":[0.1,0.1],"clearance":[12,10,12],"contact":false}
  -> {"ok":true,"version":1,"tick":0,"forward":0.8,"turn":0.1,
      "backend":"shiu-brian2","dataset_hash":"","mapping":"lif-rate-v1"}

Optional FlyWire pack: if present, dataset_hash is reported for Identity only.
The network itself stays synthetic so NC data never enters the GPL installer path.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sys
from pathlib import Path


BACKEND = "shiu-brian2"
MAPPING = "lif-rate-v1"
VERSION = 1


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def default_pack() -> Path:
    env = os.environ.get("LIFESIM_FLYWIRE_PACK", "").strip()
    if env:
        return Path(env)
    return repo_root() / "userdata" / "packs" / "flywire-nc"


def dataset_hash(pack: Path | None = None) -> str:
    pack = pack or default_pack()
    man = pack / "MANIFEST.json"
    if not man.is_file():
        return ""
    try:
        data = json.loads(man.read_text(encoding="utf-8"))
        return str(data.get("dataset_hash", "") or "")
    except Exception:
        return ""


class Bridge:
    def __init__(self) -> None:
        self.odor_gain = 1.0
        self.light_gain = 0.0
        self.phase = 0.0
        self.avoid = 0.0
        self.hash = dataset_hash()
        self._brian_ok = False
        self._G = None
        self._import_error = ""
        try:
            from brian2 import NeuronGroup, prefs, set_device

            set_device("runtime")
            try:
                prefs.codegen.target = "numpy"
            except Exception:
                pass
            from brian2 import Network
            self._G = NeuronGroup(
                2,
                """
                dv/dt = (-v + I) / (10*ms) : 1
                I : 1
                """,
                method="exact",
            )
            self._net = Network(self._G)
            self._brian_ok = True
            self._brian2 = __import__("brian2")
            self._prefs = prefs
        except Exception as e:
            self._import_error = f"{type(e).__name__}: {e}"

    def reset(self, seed: int, odor_gain: float, light_gain: float) -> dict:
        self.odor_gain = float(odor_gain)
        self.light_gain = float(light_gain)
        self.phase = (int(seed) % 6283) * 0.001
        self.avoid = 0.0
        self.hash = dataset_hash()
        if self._G is not None:
            self._G.v = 0
            self._G.I = 0
        return {
            "ok": True,
            "backend": BACKEND,
            "mapping": MAPPING,
            "brian2": bool(self._brian_ok),
            "dataset_hash": self.hash,
        }

    def _decode_rates(self, odor, light, clearance, contact: bool, seconds: float):
        def contrast(s):
            return (s[0] - s[1]) / (0.01 + s[0] + s[1])

        turn = 9.0 * (
            self.odor_gain * contrast(odor) + self.light_gain * contrast(light)
        )
        drive = 0.8
        ahead = clearance[1] if len(clearance) > 1 else 24.0
        left = clearance[0] if clearance else 24.0
        right = clearance[2] if len(clearance) > 2 else 24.0
        if ahead < 6.0 or contact:
            if not self.avoid:
                self.avoid = 1.0 if left >= right else -1.0
            turn = self.avoid
            drive = 0.08 if ahead < 2.0 else 0.35
        else:
            self.avoid = 0.0
            turn += 0.07 * math.sin(seconds * 1.7 + self.phase)
        if self.odor_gain > 0 and min(odor[0], odor[1]) > 0.99:
            drive = 0.0

        if self._brian_ok and self._G is not None:
            self._G.I = [max(0.0, drive * 1.2), turn]
            self._net.run(0.5 * self._brian2.ms)
            v0 = float(self._G.v[0])
            v1 = float(self._G.v[1])
            drive = max(0.0, min(1.0, 0.55 + 0.4 * math.tanh(v0)))
            turn = max(-1.0, min(1.0, math.tanh(v1)))
        else:
            turn = max(-1.0, min(1.0, turn))
            drive = max(0.0, min(1.0, drive))
        return drive, turn

    def act(self, msg: dict) -> dict:
        tick = int(msg.get("tick", 0))
        odor = list(msg.get("odor") or [0.0, 0.0])
        light = list(msg.get("light") or [0.0, 0.0])
        clearance = list(msg.get("clearance") or [24.0, 24.0, 24.0])
        while len(odor) < 2:
            odor.append(0.0)
        while len(light) < 2:
            light.append(0.0)
        while len(clearance) < 3:
            clearance.append(24.0)
        contact = bool(msg.get("contact", False))
        seconds = float(msg.get("seconds", 0.0))
        forward, turn = self._decode_rates(odor, light, clearance, contact, seconds)
        out = {
            "ok": True,
            "version": VERSION,
            "tick": tick,
            "forward": forward,
            "turn": turn,
            "backend": BACKEND,
            "mapping": MAPPING,
            "dataset_hash": self.hash,
            "brian2": bool(self._brian_ok),
        }
        if not self._brian_ok:
            out["brian2_error"] = self._import_error or "import failed"
        return out


def handle_line(bridge: Bridge, line: str) -> dict:
    msg = json.loads(line)
    cmd = msg.get("cmd", "act")
    if cmd == "reset":
        return bridge.reset(
            int(msg.get("seed", 1)),
            float(msg.get("odor_gain", 1.0)),
            float(msg.get("light_gain", 0.0)),
        )
    if cmd == "ping":
        return {
            "ok": True,
            "backend": BACKEND,
            "mapping": MAPPING,
            "brian2": bridge._brian_ok,
            "dataset_hash": bridge.hash,
        }
    if cmd == "act":
        return bridge.act(msg)
    return {"ok": False, "error": f"unknown cmd {cmd}"}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--once",
        action="store_true",
        help="Read one JSON object from stdin and exit",
    )
    args = ap.parse_args()
    bridge = Bridge()
    if args.once:
        line = sys.stdin.readline()
        if not line:
            print(json.dumps({"ok": False, "error": "empty stdin"}))
            return 1
        print(json.dumps(handle_line(bridge, line)), flush=True)
        return 0
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            print(json.dumps(handle_line(bridge, line)), flush=True)
        except Exception as e:
            print(
                json.dumps({"ok": False, "error": f"{type(e).__name__}: {e}"}),
                flush=True,
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
