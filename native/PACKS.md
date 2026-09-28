# Optional packs

The Windows installer already offers **core**, **tools** (compiler + matching
source) and per-template components generated from `--export-templates`. That is
the pattern for optional content.

## Particle Collision Lab (2D)

Box2D 3.1.1 is MIT-licensed and vendored under `native/third_party/box2d`. It is
compiled into the workbench and dedicated runner, so the collision template does
**not** need a separate offline pack. An empty-bench install still runs; choosing
the `collision2d` template uses the linked engine.

Build note: configure the native tree with CMake 3.22+ and the MinGW UCRT64
toolchain used for the rest of the bench. `add_subdirectory(third_party/box2d)`
builds a static `box2d` library; samples and upstream tests are not required.

## Later packs (not in this tree)

| Pack | Why separate | Status |
| --- | --- | --- |
| Particle Lab 3D (Jolt) | Larger engine, separate validation | Later |
| Fly Brain runtime/data | FlyWire CC BY-NC 4.0; must not enter the main GPL installer | Research only |
| Detailed Fly Body (FlyGym/MuJoCo) | Large assets; Windows route unverified | Research only |

When a future pack is added: pin version and hash, add notices under `licenses/`,
stage sources with the installer when GPL corresponding-source rules apply, and
keep empty-bench installs working without the pack.
