# Status

## Current release

These are the published binaries from the 2026-09-23 post-publish build. A later build should be checked against `dist/SHA256SUMS.txt`, which is the live checksum list on the machine that produced it. The figures below were verified against the copies that shipped; they replace any earlier hashes in this section.

| File | SHA256 | Size |
|---|---|---|
| `native/workbench.exe` | `835C4E32B0F8ECDD76EB9BDF7BA2EFB933A044AB65D2A3415258DA36C7E13936` | 6,255,029 bytes (~2026-09-23 8:27 AM ET) |
| `native/bench_run.exe` | `77DC391E0E755DDAC65DC8FB8C8DD5746341812C44F32B37293219857BD1C7FA` | 5,465,185 bytes (~2026-09-23 8:24 AM ET) |
| `dist/LifeSimWorkbench-Setup.exe` | `E8622B86F74D6368B9B86C1061A88A2280CA9437F16841330A1363A768B68F9D` | 429,567,479 bytes (~2026-09-23 8:32 AM ET) |

`native/build/` may be stale. The development launcher uses the copies at `native/workbench.exe` and `native/bench_run.exe`.

The setup executable is 429,567,479 bytes. A figure of about 98 MB does not describe this installer. The "about 330 MB" note in [installer/README.md](installer/README.md) is the toolchain source download, not `LifeSimWorkbench-Setup.exe`.

## Open

Source on this branch has moved past those binaries. The 2026-09-28 follow-ups landed the following, and left the rest explicit.

**Particle Collision Lab** (`collision2d`) steps with vendored **Box2D 3.1.1** (`native/third_party/box2d`), a fixed 1/60 s step and four substeps. The one-dimensional elastic oracle remains an analytical check. Landed on this branch: denser gas/pile presets with timing/memory checks, place/erase/kick/drag editing, and a scene checkpoint in the project body (positions, velocities, generation). Box2D is MIT and linked into the main binary, so it is not a separate installer pack; see [native/PACKS.md](native/PACKS.md). Jolt for 3D stays a later stage.

**Fly Arena** (`flyarena`) is registered. It hosts the reusable actor contract in `native/src/actors/fly.hpp` with synthetic food/obstacles/light/wind scenes from `fly_world.hpp` and the hand-written `ReactiveController`. Project save stores the live arena in the document body. This is the first embodiment from the research brief — not a Brian2 brain and not FlyWire data.

**Block world: a town** replants wood. Oak leaves drop a sapling about one time in twenty as they rot, and a sapling in light of at least 9 advances one stage in seven, then grows a tree. The diamond-problem map in `voxelcraft.hpp` stays a finite stand, because the published step counts were measured on a map that does not replant itself.

**Create simulation** can copy a library template into a saved `.benchsim`. The copy's model is that registry id and its settings start at the template defaults. It does not capture painted cells, timeline history or a trained brain, which is the same limit as every other save. The menu itself is Win32 and was not exercised on the Linux build.

**Fly Brain on Windows** — local path landed on this checkout: userdata/brian2-venv (Python 3.11, Brian2 2.7.x) imports; Cython codegen works after MSVC cvars64.bat. Scripts/docs: 
ative/scripts/setup_brian2_windows.ps1, 
ative/BRIAN2-WINDOWS.md. Optional FlyWire v783 pack under userdata/packs/flywire-nc/ (CC BY-NC, gitignored) with dataset_hash in MANIFEST.json; C++ discovery in ly_pack.hpp. Never staged into the installer. Eon, DOOMFLY and full FlyGym remain unverified. CI still does not install these runtimes. The registered lyarena keeps ReactiveController.

**Jolt 3D / FlyGym** — honest groundwork only: vendored pin notes + CMake stub + compileable SphereWorld scaffolding and embodiment-boundary notes (jolt_and_mujoco_share_contacts = false). Full Jolt link and FlyGym assets are not claimed done.