# UI and simulation workflow review

The active simulation owns the workspace. The searchable library uses metadata only; switching releases the previous model, snapshots and rendering buffers before constructing the next. Browsing pauses and preserves the active run.

## Interaction changes

Controls, Metrics and Guide have separate tabs. Parameters have native choice menus, exact numeric entry and steppers. Setup edits wait for Apply & restart; dependency explanations follow pending settings. Specialized actions use the model's existing implementation. Camera views, painting states and the Voxel Town command pad appear where supported.

Balanced mode retains bounded snapshots. Large run mode drops snapshots, keeps metric traces, and limits host display to 15 fps. Actual elapsed stepping time is checked inside free-run batches so an old cost estimate cannot schedule a long batch after a workload change. The movement lab trains in short interruptible batches. Other models still finish individual expensive steps and complete training epochs synchronously.

## Per-simulation decisions

The movement lab addition passed all 14 suites on 2026-09-12. Results: [1,131 core checks](build-check/movement-core-detail.log), [220 workflow checks](build-check/movement-workflow-detail.log), [239 focused movement checks](build-check/movement-focused.log), and [645 UI checks](build-check/movement-final-ui.log). The [workspace test run](build-check/movement-workspace-tests.log), [nine subsystem/plugin suites](build-check/movement-subsystems.log), and [maximum-size physical trial](build-check/movement-maximum.log) all pass. The [full roster render](build-check/movement-roster.log) contains 34 nonblank tiles; the Life/Rulestring duplicate is intentional.

The new [body workshop](build-check/movement-body-workspace.png), [small editor](build-check/movement-body-small.png), [walking](build-check/movement-workspace.png), [stairs](build-check/movement-stairs-workspace.png), and [jumping](build-check/movement-jump-workspace.png) screens were inspected visually. The [installation record](build-check/movement-installation.json) confirms the installed executable matches the tested build. See [Movement Lab](MOVEMENT-LAB.md) for controls and model limitations. The older verification table below records the preceding 33-model library release.

Each entry was checked against actual controls and interaction methods. Remaining controls stay available under Advanced or Display settings. Rules and defaults were preserved.

| Simulation | Focus | Main controls (keys) | Context action |
|---|---|---|---|
| `locomotion` | Build a body and evolve movement | `task`, `population`, `seconds`, `speed` | Body tools, starter bodies, undo, save/load, champion replay and follow best |
| `life` | Explore cellular patterns | `density`, `size`, `shape` | Seed patch |
| `vonneumann` | Program a constructor | `program`, `code`, `size` | Place wire |
| `loops` | Grow a colony | `colonies`, `size` | Plant loop |
| `highlife` | Explore HighLife | `density`, `size`, `shape` | Seed patch |
| `wireworld` | Build a circuit | `clockA`, `clockB`, `size` | Model-specific run, training or editing tools |
| `brain` | Follow firing waves | `density`, `size` | Model-specific run, training or editing tools |
| `seeds` | Explore a birth-only rule | `density`, `size` | Seed patch |
| `daynight` | Explore complementary patterns | `density`, `shape`, `size` | Seed patch |
| `ant` | Follow the turning rule | `turns`, `speed`, `size` | Model-specific run, training or editing tools |
| `life3d` | Sculpt a living volume | `world`, `seed`, `size`, `density`, `el`, `eu`, `fl`, `fu` | Seed volume |
| `clouds3d` | Explore stable structures | `world`, `seed`, `density`, `el`, `eu`, `fl`, `fu` | Seed volume |
| `nowakmay` | Watch cooperation spread | `b`, `size` | Add defectors |
| `boids` | Balance flocking forces | `size`, `separation`, `alignment`, `cohesion`, `radius` | Disturb flock |
| `particles` | Explore particle interactions | `size`, `rmax`, `beta`, `friction`, `force` | Disturb particles |
| `pps` | Explore local turning | `size`, `alpha`, `beta`, `v`, `r` | Disturb particles |
| `gridworld` | Teach an agent a route | `epsilon`, `alpha`, `gamma`, `size` | Model-specific run, training or editing tools |
| `netviz` | Look inside a learning network | `showcase`, `hidden`, `lr` | New weights |
| `continual` | Compare learning and forgetting | `taskB`, `phase1`, `lr` | New weights |
| `neuralq` | Compare a network with a table | `replay`, `target`, `lr`, `epsilon` | Model-specific run, training or editing tools |
| `cartpole` | Balance a pole | `bins`, `epsilon`, `lr` | Shove pole |
| `platformer` | Evolve a player | `seeenemies`, `population`, `parentsel`, `maxnodes` | Jump |
| `pokebattle` | Compare battle policies | `epsilon`, `alpha` | New battle run |
| `voxelcraft` | Climb the crafting tree | `agent`, `size`, `seed` | New world |
| `voxelcity` | Run a town or drive an agent | `size`, `policy`, `agents`, `roles`, `share`, `hunger` | Model-specific run, training or editing tools |
| `ga` | Compare evolutionary search | `problem`, `selection`, `population`, `mutrate`, `crossover` | Add immigrants |
| `lifeengine` | Evolve bodies | `size`, `produce`, `upkeep`, `mutate`, `food` | Add organism |
| `kinesis` | Explore aggregation | `size`, `slow`, `turn`, `patches`, `agents` | Scatter agents |
| `cutemold` | Grow and disperse molds | `size`, `upkeep`, `mutate`, `spore`, `light` | Plant mold |
| `hexplanet` | Explore a generated planet | `sea`, `subdiv`, `grow`, `pents` | New planet |
| `traffic` | Create and inspect a traffic jam | `length`, `density`, `rule`, `p`, `vmax` | Brake traffic |
| `trafficidm` | Compare driving and lane changes | `length`, `density`, `lanes`, `T`, `v0`, `rules`, `polite` | Brake traffic |
| `rule` | Write a cellular rule | `density`, `size`, `seed` | Model-specific run, training or editing tools |
| `mysim` | Experiment with annealing | `density`, `size` | Model-specific run, training or editing tools |

## Defect fixes

- Source-only plugins are discoverable before their first build.
- Legal RLE whitespace, including `x= 3`, parses correctly.
- Export filename sanitization preserves the parent directory, including spaces.
- Toolbars wrap within their pane; panels clip drawing and input. Timeline input uses the drawn track.
- Plugin retention tests separate snapshot count from wall-clock throttling.
- The example plugin marks density as setup, and reset clears its generation counter.
- Failed stepping or training stops the run instead of repeatedly retrying the same failure.

The earlier inert-control audit's flags were diagnostic observations, not 27 proven broken controls. Camera settings can change the image without changing metrics, and rare transitions may not occur in a short run. Models were not retuned merely to eliminate those flags.

## Verification

Verified on Windows with MinGW GCC 16.2.0, a CMake Release build and Ninja, 2026-09-12. All 13 registered test suites passed across the full core/subsystem run and the targeted workflow/UI runs.

| Check | Result | Evidence |
|---|---|---|
| Core rules | 1,116 checks, zero failures | [Full tests](build-check/full-tests.log) |
| Runtime limits, eight subsystem suites, plugin integration | All pass | [Full tests](build-check/full-tests.log) |
| Workflow regressions and all 33 profiles | 214 checks, zero failures | [Workflow/UI run](build-check/verified-ui-tests.log) |
| Final real Win32 UI, input and build workflow | 616 checks, zero failures | [Final UI output](build-check/verified-ui-detail.log) |
| Large worlds through the UI | 4096-wide Life renders with zero snapshot bytes; 160-cubed Life steps successfully | Included in final UI checks |
| Rendered roster | 33 tiles, zero blank; Life/Rulestring duplicate is intentional | [Render log](build-check/ux-all-sims.log) |
| Visual inspection | Library, controls, driving, learning plots, guide, 3D and focus layouts | [Library](build-check/ux-library.png), [learning](build-check/ux-learning.png), [town](build-check/ux-town.png), [guide](build-check/ux-guide.png), [focus](build-check/ux-focus.png) |

UI verification uses the actual layout and window procedure at 1084×661 and 1500×900 client sizes. It covers native text entry, search before repaint, staged setup, all three inspector tabs across every built-in model, and compiling/loading new source from a directory containing spaces. Visual inspection complements the rectangle checks.

The verified application was copied to `native/workbench.exe`, which the existing launcher uses. The bundled example DLL was updated from the tested build. Installed file hashes match the build outputs. Previous binaries are in `native/backups/release-20260912`; earlier source copies are in `native/backups/ui-20260907`.

Remaining practical limits: the host still processes an individual simulation step or full training epoch synchronously, and some models render internally during a step. Large run mode reduces host overhead; it does not remove those model costs. The timeline is a visual history, not a restorable checkpoint.

## Implementation references

- [Microsoft: popup menus](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-trackpopupmenuex)
- [Microsoft: child-window clipping](https://learn.microsoft.com/en-us/windows/win32/winmsg/window-styles)
- [Microsoft: clipped text and ellipsis](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-drawtextw)
- [Microsoft: process creation flags](https://learn.microsoft.com/en-us/windows/win32/procthread/process-creation-flags)
- [Golly: pattern formats](https://golly.sourceforge.io/Help/formats.html)
- [CMake: resource compilation](https://cmake.org/cmake/help/latest/command/enable_language.html)
