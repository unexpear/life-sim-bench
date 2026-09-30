# Life-sim Workbench

A native Windows workbench with 60 optional simulation templates and support for your own simulations. Choose a template or saved simulation from the searchable library, then work with that one world in a dedicated workspace.

Free and open source under **GPL-3.0-or-later**. See [licensing](LICENSING.md),
[third-party credits](THIRD_PARTY_NOTICES.md) and the [license audit](LICENSE-AUDIT.md).

## Run

Build the installer using [these instructions](installer/README.md); its output is **dist/LifeSimWorkbench-Setup.exe**. Generated executables are not stored in the source repository. Choose all templates, individual templates, or an empty workbench. The optional **C++ build tools** let you build custom simulations without installing a separate compiler. Everything is included in this one offline installer, including matching source and license notices. Open **Life-sim Workbench** from the Start menu after installing. **Tools → License & source** locates the included notices and source.

For the development checkout:

Build the app first (see below), then run `native/build/workbench.exe`. The convenience **Life-sim Workbench.bat** launcher uses that build when it exists, and otherwise the copy at `native/workbench.exe`. Search or filter the library, then open a simulation. It opens paused so you can configure it first. **Library / F4** returns to the collection; **Return to workspace** keeps the current world. Opening another entry starts a new run and releases the previous world and history.

The library reads metadata without constructing simulations. There is one active simulation instance per app process.

## Save and reopen

**Save as / Ctrl+S** stores a named `.benchsim` simulation. It keeps settings, custom rulestrings, custom creature bodies, and a custom C++ simulation's source and compiled DLL when available. Named simulations stay in the library after restarting, including files saved outside the default folder. **Open simulation / Ctrl+O** imports a `.benchsim` file and copies its custom assets into your library.

Switching simulations or closing normally saves the current named project automatically. An unnamed template is kept as **Last workspace**. **Create simulation** immediately creates a permanent project, even before its first successful build. It can start a rule, a C++ source file, or a copy of a library template with that template's default settings.

Reopening starts a new run from the saved setup. This is not a full checkpoint: painted cells, timeline history, current sorting progress and trained populations/brains are not restored.

Installed copies keep saved simulations under `%LOCALAPPDATA%\LifeSimWorkbench\Simulations`; development checkouts use `userdata/Simulations`. **Saved files** opens that folder. Updates and uninstall leave saved simulations alone. To share a custom simulation, copy its `.benchsim` file and matching `.assets-*` folder together. Save copies the primary `.cpp` and DLL; keep any additional custom headers/resources with that source yourself.

## Controls

- **Controls** shows actions and main parameters selected for that model. Choices use menus. Numerical controls have minus/plus buttons and accept an exact value when clicked; Enter accepts and Escape cancels.
- Setup changes wait for **Apply & restart**. Live parameters apply immediately. **Advanced controls** exposes the remaining model settings.
- **Metrics** contains measurements, learning curves, comparisons and CSV export. **Guide** explains the model, its provenance and palette.
- **Run / Pause**, **Step**, and **Reset** control the current world. Learning models also have a **Train** menu using their episode or generation boundaries.
- 3D models have named camera views, fit and projection controls. Voxel Town includes an agent command pad. Painting is offered where the model exposes editable state.
- **Inspector / Tab** toggles the right panel. **F3** focuses the canvas; its Exit focus button restores controls. **F11** toggles fullscreen.

## Rule lab

Open **Rule lab · custom cellular automata**, or choose **Create simulation → Rule-based simulation (no coding)** even on an empty bench. Paste or edit B/S, Generations (2–256 states), Hensel rules such as `B2-a/S12`, or four/six-neighbour rules with `V`/`H` suffixes. The editor validates drafts and offers examples, selection, paste, undo, Apply and Cancel. Applying retains your world settings; saves and dedicated runs use the applied rule. See the [rulestring guide](native/RULE-LAB.md).

Completed source research for a reusable fly-brain actor, its own simulation, and 2D/3D particle collisions is in [Simulation research brief](native/RESEARCH-NEXT.md). Particle Collision Lab steps with vendored Box2D 3.1.1, linked into the workbench. Fly Arena is a registered template. The optional Brian2 runtime and FlyWire pack stay on this machine and are not part of the installer.

## Creature evolution

Search **evolution** and open **Creature evolution: movement lab**, inspired by Evolution by Keiwan. **Edit body** places joints, bones and muscles, with starter bodies and undo. **Run** watches the population; **Train** breeds generations; **Replay champion** watches the best completed trial. Walking, jumping and stairs have separate objectives. Save and load custom bodies using `.creature` files. See the [movement lab guide](native/MOVEMENT-LAB.md).

## Sorting lab

Search **sorting** and open **Sorting lab: 2D**, **Sorting lab: 3D** or **Sorting lab: disparity sphere**. All three offer eighteen algorithms:
- Bubble, Cocktail shaker, Odd-even, Comb and Gnome
- Selection, Cycle and Pancake
- Insertion and Shell
- Merge, three-way Quick and Heap
- the Bitonic and Odd-even merge sorting networks
- Radix LSD and Radix MSD (base 10)
- Gravity (bead) sort, drawn bead by bead

Switch **View** during a run without restarting:
- **2D bars** and **3D columns** show values.
- The **disparity circle** and **disparity sphere** show how far each item is from where it belongs: a finished sort is a full disc or sphere.

The 3D views have orbit, zoom, standard views and parallel projection. A strip below keeps array order visible, or shows the working buffer of Merge and both radix sorts.

Sorting views have smoothed edges, shaded spheres and antialiased lettering. Rendered simulations also resize smoothly throughout the workspace, preserving thin details in small windows. Cell grids keep their sharp magnification.

Choose 16–512 items, five input arrangements, ascending or descending order, and a repeatable seed. Set **Events per step** to 1 to inspect individual comparisons, swaps, copies, key reads and bead-rod drops. Playback stops when sorting finishes; **Replay** runs the same input again. **New data** advances the seed. See the [sorting lab guide](native/SORTING-LAB.md) for counters, algorithms and comparison advice.

## Larger runs

Choose the model's world size, road length or population setting, then apply setup. Existing model limits remain: Life offers worlds up to 4096 cells across and the 3D Life family offers volumes up to 160 cubed.

**Balanced** records a bounded timeline and displays up to 60 frames per second. **Large run** releases timeline snapshots, stops copying fields into history and displays up to 15 frames per second. Measurements remain available. Switching modes does not change model parameters or transition rules.

This reduces host display and recording costs. Some models still render internally during each step. A single step or training epoch measured at more than 50 ms runs on a worker thread. The window keeps showing its last frame with a timer, and **Pause** or **Space** stops the run as soon as that step or epoch lands. Clicks on other controls are refused until then, with a line saying why; a value typed into a box is kept and applied when it lands. The movement lab trains in short batches instead, so Pause works during a generation. The timeline stores pictures of prior states; it does not restore a complete simulation checkpoint.

## Dedicated runs

For a run that is already set up and should be big, long, or both, **Tools → Dedicated run** hands the current applied settings to `bench_run`, which hosts one copy of the simulation in its own process with no window, timeline or frame clock. The workbench stays usable and shows progress in the status bar; a crash in the run cannot take the window down. Learning sims offer **Dedicated training** in their own epochs. Results land in `runs\` as `metrics.csv`, per-epoch CSV, optional frames and a `run.json` recording every setting.

`bench_run` also works on its own, for runs that should outlive the window:

```powershell
native\bench_run.exe --describe life
native\bench_run.exe life --set size=4096 --steps 100000
native\bench_run.exe gridworld --epochs 1000
```

It does not carry over hand-painted cells, loaded patterns or a custom Movement Lab body, and it does not raise a sim's own size ceilings. See the [dedicated runs guide](native/DEDICATED-RUNS.md).

## Add your own

Choose **Create simulation** in the library to create a saved project from the included example source. The new source appears immediately, before it has a DLL. Use **Tools → Edit source in editor**, then **Build**. The compiler runs in a hidden process and the plugin loads when the build succeeds. Compatible control settings survive rebuilding.

Alternatively, use **Open simulation** for a shared `.benchsim`, or place a source file or compatible DLL in **Tools → Open custom code folder** and choose **Refresh**. This is `%LOCALAPPDATA%\LifeSimWorkbench\Plugins` when installed, or `plugins` in a development checkout. Source and DLL should share a filename stem. Plugins export `bench_create_sim` and implement `native/src/sim.hpp`; they can supply controls, metrics, camera interaction, learning epochs and rendered surfaces. Use the same compiler and C++ ABI as the host. The bundled build workflow targets MinGW GCC and statically links its runtime.

Only the selected plugin is loaded. A shadow DLL leaves the original available for rebuilding. Auto-reload pauses while browsing the library.

## Build and verify

On Windows with CMake, Ninja and MinGW GCC:

```powershell
cmake -S native -B native/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBENCH_BUILD_APP=ON
cmake --build native/build --parallel 2
ctest --test-dir native/build --output-on-failure
```

The window uses Win32/GDI; no SDL dependency. On other platforms, use `-DBENCH_BUILD_APP=OFF` for the portable rules and verification tools.

CTest covers core rules, runtime limits, creature physics and evolution, sorting correctness and counters, rendering quality and image scaling, eight subsystem suites, workflow regressions, plugin loading, the dedicated runner and its command line, the stepping worker thread against every library sim, and real Win32 layout/input checks. `bench_ui_shot` renders a selected workspace without a visible window; `bench_sheet` produces a contact sheet.

Installer build instructions and restart checks are in [installer/README.md](installer/README.md).

## Notes

- [UI and simulation workflow review](native/UI-UX-REVIEW.md)
- [Dedicated runs](native/DEDICATED-RUNS.md)
- [Simulation roster](sims.md)
- [License audit](LICENSE-AUDIT.md)
- [Original concept](concept.md)

The `web` folder is an earlier browser prototype. The desktop application is under `native`.
