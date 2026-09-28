# Jolt Physics pin (Particle Lab 3D — scaffolding)

Pinned release from native/RESEARCH-NEXT.md:

| Field | Value |
| --- | --- |
| Project | [JoltPhysics](https://github.com/jrouwe/JoltPhysics) |
| Tag | v5.6.0 |
| Commit | e77f175595e64cb44218cc9d9d56fc365ad0e36a |
| License | MIT |
| Zipball | https://github.com/jrouwe/JoltPhysics/zipball/v5.6.0 |

## What is (not) in this tree

This directory holds **CMake stubs and body-interface notes**, not a full vendored
engine checkout. Box2D was small enough to vendor completely; Jolt is larger and
still a later validation stage. To fetch the pin locally for experiments:

`powershell
# from repo root — lands in a gitignored work folder, not installer staging
powershell -ExecutionPolicy Bypass -File native\scripts\fetch_jolt_pin.ps1
`

## Honest limits

- Full MuJoCo (FlyGym) and Jolt **do not** share contacts. A detailed fly body
  remains a FlyGym/MuJoCo world; Jolt is for 3D spheres/obstacles in Particle Lab.
- MinGW UCRT64 support is documented upstream; it is not yet proven in this
  workbench CI. Default CMake option BENCH_WITH_JOLT is OFF.
- Do not claim shared-engine embodiment. See RESEARCH-NEXT.md stage 6.
