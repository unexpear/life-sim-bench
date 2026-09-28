# Fly-brain actors and particle collisions: research brief

Researched 2026-09-23 against the current workbench, primary papers, official documentation and upstream source. **Source research complete; integrations are not implemented.** No neural benchmarks or third-party physics builds were run for this report. Upstream resource figures are not measured requirements for this workbench.

## Confirmed project direction

The user accepted both research directions and confirmed that the project is intended to be free and open source. Existing project notes also explicitly accept GPL dependencies. This removes any assumed requirement to use only permissive code licenses; it does not replace third-party license conditions or turn noncommercial data into unrestricted data.

The initial 2026-09-23 check found no Git repository in this folder and no project license. The subsequent authorized licensing pass selected GPL-3.0-or-later, preserved upstream notices and prepared a source-only Git repository. See [the current audit](../LICENSE-AUDIT.md). This project license does not change the separate data conditions described below.

## Recommendation

Keep **one workbench, one active world**, with optional templates and reusable actors. A template selects a world and compatible runtime; an actor supplies a controller and a body supported by that world. Every template does not have to use the same physics engine.

1. Start **Particle Collision Lab in 2D with Box2D**. This is the smallest useful new simulation and can later host a simple brain-controlled actor.
2. Develop **Fly Brain Lab** as an optional pack. Reproduce a Shiu reference experiment before attempting movement; evaluate Eon's accelerated implementation against that reference.
3. Make that controller reusable in a simple arena and compatible collision scene, with an explicitly experimental sensory/movement mapping.
4. Add **Jolt for 3D particle collisions**. Separately evaluate **FlyGym/MuJoCo for a detailed fly body**; a MuJoCo body cannot simply be dropped into a Jolt world with correct shared contacts.
5. Treat MaleCNS/game-playing integrations as a separate experimental backend. They are useful examples, not proof that general-purpose learning has been solved.

These are engineering recommendations, not promises of real-time speed or unrestricted compatibility.

## Findings in this workbench

| Inspected code | Finding and consequence |
| --- | --- |
| src/sim.hpp | Whole-world Sim interface, including stepping, metrics, surfaces, cameras and editing. No common actor/observation/action interface. Hosts must explicitly advertise actor support. |
| src/plugin.hpp | A DLL returns a C++ Sim object through bench_create_sim; compiler and standard library must match. Preserve existing plugins and add a separate versioned actor boundary. |
| src/sim_worker.hpp | One worker owns the simulation while busy. Shutdown joins it; an unbounded backend call would delay shutdown. Calls need bounded work and cancellation. |
| src/projects.hpp | Version 1 stores model/source/DLL, rule, creature body, knobs and switches. Unknown fields and documents above 4 MiB are rejected. Arbitrary actor scenes and neural checkpoints need a format extension. |
| src/sims/continuous.hpp | Particle Life applies attraction/repulsion and damping; it is not a physical collision engine. Preserve it as its own model. |
| src/sims/locomotion.hpp | Evolution uses simplified joints/bones/muscles, and explicitly excludes self-collision, flight and aerodynamics. It is not a validated fly body. |
| src/render/ and src/sim.hpp | Existing software surfaces and camera controls can display a first collision lab without replacing the UI. |
| CMakeLists.txt and ../installer/workbench.iss | Native C++20, MSYS2 UCRT64 and optional offline components already exist. Documented MinGW support is encouraging but is not a completed test with this exact toolchain. |

## Fly brain: candidates and evidence

| Candidate | Verified role | Recommended use |
| --- | --- | --- |
| [Shiu brain model](https://github.com/philshiu/Drosophila_brain_model/tree/91bdd1e7dcf193f3e7ca5a8933497fcef63b7960) | MIT Brian2 research code; neuron activation/silencing produces spikes/rates. Default data is FlyWire v630; its README explains selecting v783. | Scientific reference and first neural-only lab. Separate reproduction from newer-data experiments. |
| [Eon fly-brain](https://github.com/eonsystemspbc/fly-brain/tree/a3db62f9436074e485c0278290c2164ed6150808) | FlyWire v783 LIF model, CPU/GPU backends and comparison tools. README specifies GPL-2.0-or-later with separate upstream notices. | Acceleration candidate, not a ready-made embodied actor. |
| [DOOMFLY](https://github.com/nftechie/doomfly/tree/71ecf53d78eaffaf1a57ed7b0ccf5d458abc9f33) | MIT original code; engineered sensory/action mappings connect MaleCNS to a game. The current report says v6 failed visual, conditioning and survival validation gates. | Study the adapter and validation approach; do not advertise demonstrated learned survival. |
| [FlyGym / NeuroMechFly](https://neuromechfly.org/) | Biomechanics, senses and environments; Apache-2.0 code. The 2.x API is incompatible with 1.x. | Preferred detailed walking/sensing body investigation; verify required features against the chosen release. |
| [flybody](https://github.com/TuragaLab/flybody/tree/d015e9bfe441bd90ae431bac24c55cb74bdbce26) | Apache-2.0 MuJoCo body and walking/flight imitation tasks, with optional learning dependencies. | Alternative for locomotion/flight. Learned motor policies are distinct from a connectome controller. |

The [Shiu paper](https://www.nature.com/articles/s41586-024-07763-9) validates particular sensorimotor predictions, not arbitrary navigation or learning. Its 91% result refers to 164 tested predictions. The model omits morphology, receptor dynamics and neuromodulation, among other biology; its authors caution against treating absolute firing rates as accurate biological measurements. The UI should distinguish anatomical wiring, modeled dynamics, added sensory encoding and engineered or trained motor control.

### Data terms and practical size

- **FlyWire public data is CC BY-NC 4.0**, including public v783. Code licenses do not replace these data terms. Commercial distribution must not assume unrestricted reuse; resolve the intended distribution against the actual dataset terms before bundling. [Publisher guidelines](https://flywire.ai/guidelines)
- **MaleCNS v1.0 links CC BY 4.0**. Its publisher lists a 1.1 GB connection table, 13 MB annotations and 42 MB neurotransmitter predictions: roughly 1.15 GB before runtime dependencies and caches. Larger morphology/synapse-position files are unnecessary for the first controller experiment. [Official downloads](https://male-cns.janelia.org/download/)
- Female FlyWire and male CNS are different reconstructions, with different identifiers and coverage. Do not transfer neuron-ID mappings without a verified crosswalk. MaleCNS includes the ventral nerve cord, but does not supply body dynamics or a validated action decoder. [Research overview](https://research.google/blog/a-connectomics-milestone-mapping-the-complete-male-fruit-fly-brain/)
- Preserve per-pack code/data attribution, changes, revisions and hashes. A separate process is a technical boundary, not a finding that licensing obligations disappear.

Eon's README documents Ubuntu/WSL2 with CUDA, a 97 MB connectivity file, 3.2 MB neuron list and generated sparse caches around 288–289 MB each. Its 0.1 ms neural step implies 10,000 updates per simulated second. These are not total installed-size or peak-memory estimates. Avoid installing every backend: some require separate environments or custom builds. [Pinned documentation](https://github.com/eonsystemspbc/fly-brain/blob/a3db62f9436074e485c0278290c2164ed6150808/README.md)

### Windows and dependency checks

The Shiu README documents Windows support, but an integrated portable runtime still needs verification. Brian2's current Windows guidance uses Microsoft's compiler for runtime Cython generation; the bench's bundled GCC is not automatically sufficient. Pin a tested environment rather than applying the newest requirements to old research code. [Brian2 installation](https://brian2.readthedocs.io/en/stable/introduction/install.html)

FlyGym **2.1.0** declares Python >=3.12,<3.15 and MuJoCo >=3.9,<3.10; Warp is optional. Its package excludes some large meshes and loads them lazily. An offline pack must prefetch the selected body's complete assets and pass a disconnected first-launch test. The inspected installation guide does not establish native Windows compatibility for our complete combination. [Pinned manifest](https://github.com/NeLy-EPFL/flygym/blob/ca65a510c2afe6ac61c51df4f274c8d190c2f95f/pyproject.toml), [installation](https://neuromechfly.org/installation/)

DOOMFLY documents several GB of RAM. Its inspected kernel builder selects a macOS dynamic library or a .so file otherwise, invokes clang++ with -fPIC, and has no Windows DLL branch. It needs a port or separate Linux runtime before native Windows packaging. [Builder source](https://github.com/nftechie/doomfly/blob/71ecf53d78eaffaf1a57ed7b0ccf5d458abc9f33/doom/build_kernel.py)

### Pinned Windows path (2026-09-28)

The intended first Windows path is the Shiu model on Brian2, as a reference experiment, before any accelerated backend and before any embodied actor. FlyWire connectivity stays an optional pack under CC BY-NC 4.0. It is not part of the main GPL installer, and a code license does not relicense that data. An empty or full template install must still run without it.

These Windows and runtime questions stay open, and this checkout does not install them in CI:

- Brian2 on the bench's bundled GCC, including whether runtime Cython generation still needs Microsoft's compiler.
- Eon fly-brain as an acceleration candidate against the Shiu reference, including its Ubuntu/WSL2 and CUDA notes.
- DOOMFLY, which has no Windows DLL branch in the inspected kernel builder.
- FlyGym 2.1.0 / MuJoCo, whose inspected install guide does not establish native Windows for the combination we would ship.

`native/src/actors/fly.hpp` and `fly_world.hpp` are hosted by the registered `flyarena` template (`sims/fly_arena.hpp`). They remain a synthetic reactive controller — not a Brian2 brain and not a FlyWire build. Optional pack/venv discovery is separate (`fly_pack.hpp`, `fly_brian2.hpp`).

### Proposed actor contract and ownership

Use an optional controller process owned by the active world. Load it when needed, release it on world switching, and show loading/progress/cancellation. Keep incompatible Python dependency sets in separate environments.

The versioned contract needs:

- **Identity:** actor instance, backend version, dataset hash, mapping version, body type and required host capabilities.
- **Observation:** simulation tick, coordinate frame, physical units and named sensory values. Brightness and a spike rate in hertz must not be interchangeable implicitly.
- **Action:** target tick, supported movement/actuation values, limits and validity. Reject stale or invalid responses.
- **Lifecycle:** initialize, seeded reset, advance a bounded interval, inspect, save/load supported state, cancel and shutdown.
- **Isolation:** independent dynamic neural/RNG state per actor. Immutable connectivity may be shared; mutable voltages, delay queues and learned weights must not accidentally be shared.

Use fixed simulation clocks independent of display FPS. A proposed 10 ms controller interval contains 100 steps of a 0.1 ms neural model; this is an initial experiment, not a validated biological rate. Preserve spike delays and decoder windows between calls, record the timing policy, and pause on backend lag rather than silently substituting actions.

**First embodiment:** a clearly labeled simple body with light/contact/odor-field inputs and a configurable movement decoder. The arena and collision lab can share the controller contract. Verify neural responses before interpreting a population as a natural motor command.

**Detailed embodiment:** FlyGym/MuJoCo must own the fly and its contacting surroundings in that world. Do not integrate the same body in both MuJoCo and Box2D/Jolt. Cross-engine coupling would require separate force/contact/timing validation. Cell grids and sorting templates have no meaningful fly-actor mapping by default.

### UI proposal

The standalone lab should have Stimulus, Brain activity, Body/world and Run panels, with select-neuron/group, stimulate, silence, pause, single interval, reset and traces. Default to selected neurons or grouped activity so drawing does not dominate computation; a full connectivity view is optional.

In compatible hosts, **Add actor → Fly brain** selects an installed backend, body and saved mapping. Show missing packs by name. Save actor presets without source editing. Label behavior as a reference experiment, engineered controller or validated learned policy.

## Particle Collision Lab

Working scope: visible **physical disks and spheres**, including gas-like motion and falling grains. This is separate from Particle Life.

| Option | Evidence and fit | Decision |
| --- | --- | --- |
| [Box2D 3.1.1](https://github.com/erincatto/box2d/releases/tag/v3.1.1) | MIT portable C17/C API; 2D shapes, contacts and constraints. Official build source handles MinGW. | First 2D lab; build the library with the current toolchain and keep the dependency scoped to this feature. |
| [Jolt 5.6.0](https://github.com/jrouwe/JoltPhysics/releases/tag/v5.6.0) | MIT C++ rigid-body physics; documented Windows/MSYS2 MinGW support. | Preferred 3D spheres/obstacles candidate; validate UCRT64, matching build settings and CPU baseline. |
| [MuJoCo](https://mujoco.readthedocs.io/en/stable/overview.html) | Articulated-body/contact simulation used by the detailed fly frameworks. | Use for the detailed fly's environment, not a forced replacement for all existing models. |
| [LAMMPS granular models](https://docs.lammps.org/Howto_granular.html) | Finite-size particles, friction/contact forces, rotation and torque. A 2D setup can still use spherical inertia unless configured for disks. | Later scientific granular/material mode; more model choices and validation than an initial collision lab. |
| [Geant4](https://geant4.web.cern.ch/about/) | Particle transport/interactions for detector, nuclear, radiation and related simulations. | Separate project if “particle collisions” means accelerator/subatomic physics. A bouncing-ball engine does not provide that. |

Box2D's stable release differs from current development main. Implement against release source/API rather than mixing examples from the moving branch. [Pinned build file](https://github.com/erincatto/box2d/blob/8c661469c9507d3ad6fbd2fea3f1aa71669c2fe3/CMakeLists.txt), [Jolt platform/build instructions](https://github.com/jrouwe/JoltPhysics/blob/e77f175595e64cb44218cc9d9d56fc365ad0e36a/Build/README.md)

### Controls and rendering

Presets: two-body impact, mixed masses, gas box and falling grains. Direct tools: place/select particles or obstacles, set velocity with an arrow, launch, move, erase and inspect. Group controls into Scene, Materials, Forces, Timing and Measurements, with numerical settings under Advanced.

Use physical units separate from pixels. Keep mass/radius/density consistent, with explicit overrides. Disks and spheres need different rotational inertia. Switching 2D/3D should create or explicitly convert an experiment, not silently treat a projected view as a new physical model.

Reuse crisp circles and velocity overlays in 2D. Use real depth, orbit/pan/fit, restrained lighting and optional contact markers/trails in 3D. Rendering quality must not alter numerical timestep. Profile before replacing the existing renderer.

### Correctness traps found

Box2D recommends fixed stepping, commonly 1/60 second with four substeps. Default CCD covers moving bodies against static geometry; bullet mode extends coverage but excludes bullet-versus-bullet CCD. Making every particle a bullet therefore does not solve all pairwise tunneling. Low-speed impacts can deliberately be inelastic through the restitution threshold; simultaneous-contact bounce is approximate. These matter for gas/energy experiments. [Official simulation manual](https://box2d.org/documentation/md_simulation.html)

Design consequences:

- Bound radii/speeds and verify timestep convergence. Explicitly test fast moving-versus-moving pairs; reduce the step or reject unsupported settings when necessary.
- For idealized elastic presets, disable damping/friction/sleep and deliberately choose the low-speed bounce policy. Measure residual energy error rather than promise exact conservation.
- Begin with walls. Periodic boundaries require seam-aware interactions; wrapping positions alone is incorrect.
- Reject invalid mass/radius, non-finite inputs and impossible packing before stepping. Choose count limits from measured resources.
- Let the engine own broad-phase/contact work instead of adding another all-pairs loop.

Box2D's parallel interface uses application-provided tasks; it does not create threads. Start single-threaded inside the existing worker, then add one controlled pool only if measurements justify it. Avoid competing physics/neural pools. [Official foundations](https://box2d.org/documentation/md_foundation.html)

Report translational and rotational kinetic energy, momentum, time, contacts and wall impulse. Define collision rate so resting contacts are not counted every substep. Account for walls, gravity and dissipation; particle momentum alone is not conserved in a walled box.

Use the isolated one-dimensional elastic collision as an analytical oracle:

v1 = ((m1 − m2)u1 + 2m2u2) / (m1 + m2)

v2 = (2m1u1 + (m2 − m1)u2) / (m1 + m2)

Acceptance cases: equal-mass exchange, unequal masses, glancing impact, restitution below one, wall bounce, overlap recovery, friction/rotation, fast crossing particles, dense piles and timestep refinement. Set tolerances before accepting results. Benchmark 100, 1,000 and 10,000 particles in sparse and dense scenes, stopping at resource limits; these are proposed tests, not supported-capacity claims.

## Saving and optional installation

Save support belongs in the first usable pack.

Continue reading existing v1 projects. Introduce a new version for structured scenes/actors, with a small manifest and versioned binary assets for large state. Do not put neural arrays into the current 4 MiB text document or overload its creature-body field. Resolve bundled paths safely and write updates atomically.

| Action | Required saved content |
| --- | --- |
| Save simulation | Geometry, particles/obstacles, initial conditions, materials, units, timing, seed, actor placement, mappings and exact backend/body/pack/data identities. Reopen without rewriting code or rebuilding the scene. |
| Save actor preset | Backend/body choice, sensory/action mapping, parameters and seed policy; no host-owned world objects. |
| Save checkpoint, when supported | The above plus dynamic physics/neural state, clock, RNG, delay queues, decoder state and learned weights. Require a compatible runtime. |

Eon's inspected PyTorch code passes voltage, conductance, spike, refractory and delay-buffer state explicitly. Saving weights alone would miss the live brain. [Model source](https://github.com/eonsystemspbc/fly-brain/blob/a3db62f9436074e485c0278290c2164ed6150808/code/run_pytorch.py)

DOOMFLY's checkpoint restores neural state into a new arena rather than the exact previous world. Jolt's SaveState covers physics-updated state and requires separate handling for structural/property changes. Neither is a complete project save without additional work. [DOOMFLY checkpoint](https://github.com/nftechie/doomfly/blob/71ecf53d78eaffaf1a57ed7b0ccf5d458abc9f33/doom/checkpoint.py), [Jolt state documentation](https://jrouwe.github.io/JoltPhysics/)

Keep **one offline installer**, with all/selected/empty choices. Proposed components: Particle Lab 2D, Particle Lab 3D, Fly Brain runtime/data and Detailed Fly Body. Share identical dependencies across templates. Display installed sizes after measurement; unchecked components still contribute to the download size of an all-inclusive installer.

Do not make WSL, CUDA or package downloads invisible first-launch requirements. Advertise a fully offline fly pack only after runtime, assets, data and redistribution requirements are resolved. Empty-bench installation must work without them. Preserve user scenes/checkpoints outside application folders across upgrades/uninstall. Missing or changed packs should produce an actionable message, never silently substitute another model.

## Implementation order and evidence required

| Stage | Result | Gate |
| --- | --- | --- |
| 1 | Isolated Box2D prototype | Current compiler build, analytical collisions, fast/dense cases, timing and memory. |
| 2 | Usable collision template | Direct editing, metrics, fresh-process save/reopen, v1 compatibility and optional offline installation. |
| 3 | Neural reference experiment | Pinned model/data, reference responses, reproducible seeds, bounded calls, cold-start/stepping and memory measurements. Compare accelerated outputs statistically when random streams differ. |
| 4 | Standalone brain lab | Responsive inspect/stimulate UI, saved setup, cancellation/crash recovery and a tested Windows runtime route. |
| 5 | Reusable actor | Same controller in two compatible hosts; verified units/timing, isolated state, actor save/reopen and one active world. |
| 6 | Larger/detailed modes | Jolt 3D contacts and separately FlyGym coupling, state restoration and measured limits. |

Measure one brain actor first, then two/four only if memory permits. Record simulated seconds per wall-clock second, cold start, p50/p95 control latency, peak RAM/VRAM and UI responsiveness. Use sparse connectivity and selective traces; GPU batch throughput is not proof of low latency for a single actor.

Before implementation reaches the relevant choice, settle: physical versus subatomic domain, the exact project license and third-party notices for the confirmed free/open distribution, the Windows runtime that passes the prototype, and exact checkpoint support. FlyWire retains its own attribution/noncommercial conditions; charging nothing alone is not the license's definition of noncommercial use. [CC BY-NC terms](https://creativecommons.org/licenses/by-nc/4.0/)

## Revisions and verification limits

| Project | Inspected identity |
| --- | --- |
| Shiu model | 91bdd1e7dcf193f3e7ca5a8933497fcef63b7960 |
| Eon fly-brain | a3db62f9436074e485c0278290c2164ed6150808 |
| DOOMFLY | 71ecf53d78eaffaf1a57ed7b0ccf5d458abc9f33 |
| FlyGym | v2.1.0: ca65a510c2afe6ac61c51df4f274c8d190c2f95f; main also inspected |
| flybody | d015e9bfe441bd90ae431bac24c55cb74bdbce26 |
| Box2D | v3.1.1: 8c661469c9507d3ad6fbd2fea3f1aa71669c2fe3; main differs |
| Jolt | v5.6.0: e77f175595e64cb44218cc9d9d56fc365ad0e36a |

Verified: local integration constraints, published limitations, data/license statements, selected dependency declarations, build/checkpoint source and physics documentation.

Not verified: installation of these runtimes, reproduction of results, native Windows packaging, integration accuracy, total pack size, real-time operation or large-scene capacity. These remain explicit prototype/release gates. The 2026-09-23 published binaries recorded in STATUS.md are unchanged by the notes above.

## Status after the 2026-09-28 follow-up

The findings above stand. What has since been written down in the tree:

- **Particle Collision Lab** (`collision2d`) now builds and steps with vendored Box2D 3.1.1, denser presets, edit tools beyond stamp, and a scene checkpoint in the project body. Box2D ships in-tree (MIT), not as a separate offline pack — see PACKS.md. Jolt 3D stays a later stage for a full link; stub + pin are present (below).
- **Fly Arena** (`flyarena`) is registered and hosts the reusable actor headers under `native/src/actors/` with the synthetic `ReactiveController`.
- **Brian2 / Shiu Windows runtime** — setup script and docs under `native/scripts/` and `native/BRIAN2-WINDOWS.md`. A portable venv at `userdata/brian2-venv` (gitignored) was verified importable on this checkout; Cython codegen needs MSVC (`vcvars64.bat`), not MinGW alone. `fly_brian2.hpp` locates the interpreter for a future bounded subprocess; it does not replace `ReactiveController`.
- **FlyWire local optional pack** — `userdata/packs/flywire-nc/` (CC BY-NC 4.0, gitignored) with Shiu `Connectivity_783.parquet` / `Completeness_783.csv`, `MANIFEST.json` `dataset_hash`, and `fly_pack.hpp` discovery. Never for the GPL installer.
- **Jolt / FlyGym groundwork** — `native/third_party/jolt/PIN.md` (v5.6.0), CMake stub, compileable `SphereWorld` scaffolding, and `jolt_body_notes.hpp` stating MuJoCo≠Jolt shared contacts remain blocked. Optional `fetch_jolt_pin.ps1` downloads upstream sources without enabling `BENCH_WITH_JOLT` by default.
