# Jolt Physics pin (Particle Lab 3D)

Pinned release from native/RESEARCH-NEXT.md:

| Field | Value |
| --- | --- |
| Project | [JoltPhysics](https://github.com/jrouwe/JoltPhysics) |
| Tag | v5.6.0 |
| Commit | e77f175595e64cb44218cc9d9d56fc365ad0e36a |
| License | MIT |
| Zipball | https://github.com/jrouwe/JoltPhysics/zipball/v5.6.0 |

## What is in this tree

| Path | Role |
| --- | --- |
| `include/JoltStub/` | Always-on analytic `SphereWorld` (sphere/floor contacts) |
| `CMakeLists.txt` | `jolt_stub` INTERFACE; optional `BENCH_WITH_JOLT` link |
| `JoltPhysics/` | Fetched upstream pin (gitignored) |
| `FETCHED.txt` | Written by the fetch script |

Box2D was small enough to vendor completely under `third_party/box2d`. Jolt is
larger: fetch locally, keep the pin out of the default installer image, and
link only when `BENCH_WITH_JOLT=ON`.

```powershell
# from repo root — lands in a gitignored work folder, not installer staging
powershell -ExecutionPolicy Bypass -File native\scripts\fetch_jolt_pin.ps1
cmake -S native -B native/build-jolt -DBENCH_WITH_JOLT=ON ...
```

## Honest limits

- Full MuJoCo (FlyGym) and Jolt **do not** share contacts. A detailed fly body
  remains a FlyGym/MuJoCo world; Jolt / analytic SphereWorld is for 3D
  spheres/obstacles in Particle Lab.
- Default builds use the analytic SphereWorld without compiling upstream Jolt.
- MinGW UCRT64 support is documented upstream; measure before shipping a 3D pack.
- Do not claim shared-engine embodiment. See RESEARCH-NEXT.md stage 6.
