# Brian2 / Shiu Windows runtime

The neural reference path pinned in [RESEARCH-NEXT.md](RESEARCH-NEXT.md) is the
[Shiu Drosophila brain model](https://github.com/philshiu/Drosophila_brain_model)
on **Brian2**, before any accelerated backend and before embodiment.

## Portable venv (this machine)

From the repo root:

```powershell
powershell -ExecutionPolicy Bypass -File native\scripts\setup_brian2_windows.ps1
```

That creates `userdata/brian2-venv` (Python 3.11 via `uv`), installs
[scripts/requirements-brian2.txt](scripts/requirements-brian2.txt), and verifies
import. The venv is **not** committed.

| Check | Command |
| --- | --- |
| Import + numpy codegen | `userdata\brian2-venv\Scripts\python.exe native\scripts\verify_brian2.py --numpy-only` |
| Cython codegen (MSVC) | Open a shell after `vcvars64.bat`, then `...\python.exe native\scripts\verify_brian2.py --cython` |
| Pack + tiny network | `...\python.exe native\scripts\shiu_reference_smoke.py` |
| Observation/Action bridge | `...\python.exe native\scripts\brian2_controller_bridge.py` (JSON lines on stdin) |

### Compiler reality (2026-09-28)

- **Brian2 runtime Cython on Windows requires MSVC** (VS Build Tools / VS Community).
  The bench's bundled MinGW UCRT64 GCC is **not** sufficient for Cython extensions
  that must link against the MSVC-built CPython.
- Without MSVC, Brian2 still **imports** and runs with `prefs.codegen.target = 'numpy'`.
- Pins that worked here: Brian2 2.7.x, NumPy 1.26.x, Cython 3.0.x (Cython 3.3 broke
  Brian2's `get_cython_cache_dir` import). Avoid Brian2 2.9 + NumPy 2.4 together.

### Pointing the fly actor / arena at the reference

| Variable / path | Role |
| --- | --- |
| `userdata/brian2-venv/Scripts/python.exe` | Default interpreter |
| `LIFESIM_BRIAN2_PYTHON` | Override interpreter |
| `LIFESIM_BRIAN2_BRIDGE` | Override bridge script path |
| `LIFESIM_FLY_CONTROLLER=brian2` | Prefer Brian2Controller in Fly Arena |
| `native/scripts/shiu_reference_smoke.py` | Bounded smoke (pack probe + tiny network) |
| `native/scripts/brian2_controller_bridge.py` | Observation/Action JSON-line bridge |
| `native/src/actors/fly_brian2.hpp` | `Brian2Controller` + venv discovery |

The registered **Fly Arena** (`flyarena`) defaults to `ReactiveController`.
Selecting controller=`brian2` (knob) or `LIFESIM_FLY_CONTROLLER=brian2` uses
`Brian2Controller` when the venv and bridge script exist; otherwise it falls
back to reactive. The bridge mapping is engineered rate-coded LIF — not a full
Shiu connectome reproduction.

FlyWire connectivity for the Shiu model is a **separate optional pack** —
[PACKS.md](PACKS.md). Eon/DOOMFLY Windows status: [EON-DOOMFLY-WINDOWS.md](EON-DOOMFLY-WINDOWS.md).
