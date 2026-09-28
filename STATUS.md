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

Source on this branch has moved past those binaries. The 2026-09-28 follow-up landed the following, and left the rest explicit.

**Particle Collision Lab** (`collision2d`) is in the registry. It is a 2D box of disks with the one-dimensional elastic oracle, a fixed 1/60 s step, four substeps, and version-1 project settings. Box2D 3.1.1 is the engine named in [native/RESEARCH-NEXT.md](native/RESEARCH-NEXT.md) and is not vendored. Still to do: build Box2D 3.1.1 with the workbench toolchain; fast and dense acceptance cases, timing and memory; editing beyond the stamp; a checkpoint that stores more than knobs; the optional offline pack. Jolt for 3D stays a later stage.

**Block world: a town** replants wood. Oak leaves drop a sapling about one time in twenty as they rot, and a sapling in light of at least 9 advances one stage in seven, then grows a tree. The diamond-problem map in `voxelcraft.hpp` stays a finite stand, because the published step counts were measured on a map that does not replant itself.

**Create simulation** can copy a library template into a saved `.benchsim`. The copy's model is that registry id and its settings start at the template defaults. It does not capture painted cells, timeline history or a trained brain, which is the same limit as every other save. The menu itself is Win32 and was not exercised on the Linux build.

**Fly actor stubs** are `native/src/actors/fly.hpp` and `fly_world.hpp`. They are not registered and they are not in the installer.

**Fly Brain on Windows** is pinned in the research brief: Shiu/Brian2 first, as a reference; FlyWire data stays an optional CC BY-NC 4.0 pack and never goes in the main GPL installer. Brian2, Eon, DOOMFLY and FlyGym remain unverified on this toolchain. CI does not install those runtimes.
