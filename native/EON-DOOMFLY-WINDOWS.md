# Eon / DOOMFLY on Windows — honest status

Pinned identities from [RESEARCH-NEXT.md](RESEARCH-NEXT.md):

| Project | Pin |
| --- | --- |
| Eon fly-brain | `a3db62f9436074e485c0278290c2164ed6150808` |
| DOOMFLY | `71ecf53d78eaffaf1a57ed7b0ccf5d458abc9f33` |

## DOOMFLY — Windows DLL blocker (documented)

Inspected `doom/build_kernel.py` at the pin builds:

```text
out = ... ('libneural.dylib' if sys.platform=='darwin' else 'libneural.so')
command = ['clang++', '-O3', '-std=c++17', '-shared', '-fPIC', source, '-o', temporary]
```

There is **no Windows `.dll` branch**. The builder assumes Unix-style
`clang++ -shared -fPIC` output (`.dylib` / `.so`). Native Windows packaging
therefore needs either:

1. a port of `build_kernel.py` (and likely `kernel.cpp` link flags) to produce
   `libneural.dll` with a MSVC or MinGW toolchain, or
2. a separate Linux/WSL runtime, not the main Win32 workbench process.

Until one of those lands, DOOMFLY is **not** a shippable Windows controller.
Compileable host stubs under `native/src/actors/fly_doomfly_stub.hpp` record the
Observation/Action adapter shape for study only — they do not load MaleCNS or
claim survival validation (upstream v6 failed those gates).

## Eon fly-brain — Windows path

Eon's documented path is Ubuntu/WSL2 with optional CUDA, GPL-2.0-or-later code,
and large sparse caches. It is an **acceleration candidate against the Shiu /
Brian2 reference**, not a ready-made embodied actor.

This checkout:

- Does **not** vendor Eon into the GPL installer image.
- Does **not** claim a measured Windows/CUDA pack.
- Provides `native/src/actors/fly_eon_stub.hpp` as a compileable Identity /
  lifecycle placeholder so hosts can reserve a backend slot beside
  `ReactiveController` and `Brian2Controller`.

Preferred Windows neural path remains Brian2 + optional local FlyWire NC pack
([BRIAN2-WINDOWS.md](BRIAN2-WINDOWS.md), [PACKS.md](PACKS.md)).

## What we will not pretend

- No DOOMFLY learned survival on Windows.
- No Eon real-time guarantees from these stubs.
- No FlyWire NC data inside installer staging.
