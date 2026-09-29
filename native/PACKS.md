# Optional packs

The Windows installer already offers **core**, **tools** (compiler + matching
source) and per-template components generated from --export-templates. That is
the pattern for optional content.

## Particle Collision Lab (2D)

Box2D 3.1.1 is MIT-licensed and vendored under 
ative/third_party/box2d. It is
compiled into the workbench and dedicated runner, so the collision template does
**not** need a separate offline pack. An empty-bench install still runs; choosing
the collision2d template uses the linked engine.

Build note: configure the native tree with CMake 3.22+ and the MinGW UCRT64
toolchain used for the rest of the bench. dd_subdirectory(third_party/box2d)
builds a static ox2d library; samples and upstream tests are not required.

## FlyWire NC (local optional — never in the installer)

FlyWire public data is **CC BY-NC 4.0**. Notices: [licenses/FlyWire-CC-BY-NC-4.0.txt](../licenses/FlyWire-CC-BY-NC-4.0.txt),
[flywire.ai/guidelines](https://flywire.ai/guidelines). Code licenses do **not**
relicense this data.

| Item | Location |
| --- | --- |
| Local pack (gitignored) | userdata/packs/flywire-nc/ |
| Fetch script | 
ative/scripts/fetch_flywire_nc.ps1 |
| Manifest / dataset_hash | userdata/packs/flywire-nc/MANIFEST.json |
| Discovery (C++) | 
ative/src/actors/fly_pack.hpp |
| Env override | LIFESIM_FLYWIRE_PACK = absolute pack directory |

Contents fetched for the Shiu reference: Connectivity_783.parquet,
Completeness_783.csv, NOTICE.txt, MANIFEST.json. On this machine the
connectivity SHA-256 is recorded in the manifest as dataset_hash.

**Hard rule:** do not copy this pack into installer/ staging, Inno components,
or main shipped templates. Empty-bench installs must run without it. The
registered flyarena template uses synthetic sensors only; when the pack is
present, FlyArena::subtitle and fly::pack::dataset_hash() reflect the local
hash for Identity accounting.

## Brian2 / Shiu Windows runtime (local optional)

Portable venv under userdata/brian2-venv/ (gitignored). Setup:

ative/scripts/setup_brian2_windows.ps1. Docs: [BRIAN2-WINDOWS.md](BRIAN2-WINDOWS.md).
Override interpreter with LIFESIM_BRIAN2_PYTHON. Cython codegen needs MSVC;
numpy codegen works without it.

## Particle Lab 3D (Jolt)

Pin notes and analytic SphereWorld live under `native/third_party/jolt/`
(see PIN.md). Default build uses **analytic sphere/floor contacts** via
`jolt_stub` (no upstream Jolt compile). Optional fetch:
`native/scripts/fetch_jolt_pin.ps1` → gitignored `JoltPhysics/`. CMake
`-DBENCH_WITH_JOLT=ON` links the pin when present. Body-boundary notes:
`native/src/physics/jolt_body_notes.hpp` (Jolt spheres vs FlyGym/MuJoCo fly —
**no shared contacts**).

## Later packs (summary)

| Pack | Why separate | Status |
| --- | --- | --- |
| Particle Lab 3D (Jolt) | Larger engine, separate validation | Analytic SphereWorld default; optional full link |
| Fly Arena (reactive stub) | Synthetic only; no NC data | In core via flyarena registry entry |
| Fly Brain runtime/data | FlyWire CC BY-NC 4.0; must not enter the main GPL installer | Local scripts + userdata pack/venv + embodied bridge |
| Eon / DOOMFLY | Windows DLL / WSL blockers | Documented stubs only — see EON-DOOMFLY-WINDOWS.md |
| Detailed Fly Body (FlyGym/MuJoCo) | Large assets; Windows route unverified | Notes only; separate world from Jolt |

When a future pack is added: pin version and hash, add notices under licenses/,
stage sources with the installer when GPL corresponding-source rules apply, and
keep empty-bench installs working without the pack.
