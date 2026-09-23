# Dedicated runs

One simulation, the whole machine. `bench_run` hosts exactly one simulation — any library entry, the rulestring entry with your rule, or your own plugin DLL — in its own process, with no window, no timeline and no frame clock, and writes what it measured to disk as it goes.

## Why a separate process

The workbench is built for exploring. It cuts each step batch off after 8 ms so it can paint, copies the field into the timeline, and while a single slow step runs it can only re-show its last frame and wait. That is the right trade for looking around and the wrong one for a run you have already set up and want to be big, long, or both.

A dedicated run has none of that overhead. It also cannot take the workbench down: if the run crashes — including inside your own plugin — the window carries on.

## From the workbench

**Tools → Dedicated run** offers 100,000 steps, 1,000,000 steps, or until stopped. Learning sims also offer **Dedicated training** for 100 or 1,000 of their own epochs.

- It uses the settings the window has **applied**. A staged setup that has not been applied is refused: apply or discard it first, so the run is the one you set.
- The window's own run pauses so the dedicated run gets the machine. The window stays usable.
- Progress shows in the status bar, with an estimate of the time left; the outcome shows when it finishes.
- **Tools → Stop dedicated run** finishes the current step and writes the results. If it is still unanswered after ten seconds it is forced; every row written before that stays on disk.
- **Tools → Open dedicated results** opens the results folder.
- Closing the workbench stops a run it started, cleanly if the run has begun. For a run that should outlive the window, use the command line.

**Not carried over:** cells painted by hand, a loaded pattern, a custom Movement Lab body (a dedicated movement run uses the starter body, and says so), or how far the window's own run has got. A dedicated run is a new run from the same settings.

## From the command line

```
bench_run --list
bench_run --describe life
bench_run life --set size=4096 --steps 100000
bench_run gridworld --epochs 1000
bench_run rule --rule B36/S23 --set size=2048 --seconds 600
bench_run ..\plugins\my_rule.dll --steps 50000 --frames 5000
```

| Option | Meaning |
|---|---|
| `--set key=value` | Apply a setting; repeatable. `--describe` lists a sim's settings, ranges and choices. |
| `--rule B3/S23` | The rule, for the `rule` entry. |
| `--steps N` | Stop after N steps. `1e6` is accepted. |
| `--epochs N` | Train: stop after N of the sim's own epochs. Learning sims only. |
| `--seconds S` | Stop after S seconds of wall time. Combines with the two above. |
| `--sample N` | A metrics row every N steps. |
| `--frames N` | A PNG every N steps, plus the first and final states. Off by default. |
| `--frame-size PX` | Longest side of a frame, 16–8192 (default 512). |
| `--out DIR` | Results folder. |
| `--progress-every S` | Seconds between progress lines (default 1). |

With no `--steps`, `--epochs` or `--seconds` it runs until stopped: Ctrl+C, or create a file named `stop` in the results folder. Either way it finishes the current step and writes `run.json`.

## What it writes

To `runs\<sim>-<date>-<time>\` beside `native\`, or to `--out`:

- **`metrics.csv`** — `step`, `generation`, `elapsed_s`, then the sim's own metrics. By default a bounded run records up to 2,000 rows; an open-ended one records every 100 steps. Streamed and flushed row by row, so a run that is killed keeps every row it wrote. The first row is the starting state and the last is the final state.
- **`metrics-by-<epoch>.csv`** — for learning sims, one row per completed epoch, including epochs completed during a step run.
- **`run.json`** — the reason the run ended, its exit code, any error, timing, final metrics, warnings, and **every setting's final value**, including the ones nobody changed. A run is only reproducible if the untouched values are on the record too.
- **`frame-<n>.png`** — with `--frames`. Frames are stored uncompressed, so they are large; use them sparingly.
- **`runner.log`** — the run's own output, when started from the workbench.

Metric names are quoted where a spreadsheet would split them. A value that is not a number is a blank cell, not `nan`. Some sims report nothing until they have something true to say — the movement lab publishes no fitness until a generation completes — so the columns are fixed by the first report, and a metric that first appears later is listed in `run.json` as late rather than dropped silently.

## Settings

Applied exactly as **Apply & restart** applies them: each setting through the sim's own control, then one reset. A sim opened with no settings is not reset, because opening it in the window does not reset it either.

Read exactly as the panel reads typed input: finite, inside the setting's range, rounded to its grid. **Out of range is refused, never clamped.** A choice accepts the label the app shows (`size=4096`) or its index (`size=4`); labels are matched first, so a numeric label is never mistaken for an index. If a sim corrects a value itself — 3D Life clamps its seed size to the world — the sim's value stands and `run.json` records the difference.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | Reached the steps, epochs or time asked for |
| 1 | Stopped early: Stop, Ctrl+C, the stop file, or a learner whose epoch could not complete |
| 2 | Bad arguments: unknown sim, unknown setting, value out of range |
| 3 | The plugin DLL could not be loaded |
| 4 | The simulation threw; rows written before it are kept |
| 5 | Out of memory; rows written before it are kept |

## Limits

- **The size ceilings are the sims' own.** Life offers worlds up to 4096 across and 3D Life up to 160³, in the window and here alike. A dedicated run removes the window's overhead and lets a big world run for as long as it takes; it does not raise those ceilings. Several of them were set for the window's frame time rather than for memory, so raising them for dedicated runs is a separate decision, per sim.
- One step, or one epoch of a learning sim, still runs to completion; Stop waits for it.
- The workbench runs one dedicated run at a time.
- Plugin DLLs follow the workbench's rule: the same compiler and C++ runtime as the host.

## Verification

Release build on Windows with MinGW GCC 16.2.0, CMake and Ninja, 2026-09-12: **all CTest suites pass, with zero build warnings** — 17 when this shipped, 18 since the worker-thread suite joined them.

- `runner` — 64 in-process checks, led by the property that matters most: a dedicated run is the same simulation as stepping it by hand, cell for cell and metric for metric, with settings and without. Also a plugin loaded end to end, stop, Ctrl+C and time limits, a throwing sim that keeps every row it wrote, a sim that reports late, frames, and the summary.
- `runner_cli`, `runner_cli_refuses` — the built executable itself: one run that must finish, and one refusal matched on its message rather than on a non-zero exit, which a crash would also produce.
- `ui_layout` — the real window procedure, 645 checks before and 653 after. A staged setup is refused; a launch pauses the window's own run; the run exits 0 having run exactly the density the window had applied; a Stop pressed the instant a run starts is delivered.

Measured on the development machine, one run at a time:

| Run | Units | Time | Per unit |
|---|---|---|---|
| Life at 4096 × 4096 (16.8 M cells) | 300 steps | 18.3 s | 60.9 ms |
| 3D Life at 160³ (4.1 M cells) | 100 steps | 2.2 s | 22.4 ms |
| Gridworld training | 2,000 episodes | 0.02 s | 0.011 ms |

At 4096 across, each step is nearly four of the window's 16 ms frames: the case a dedicated run is for.

## Implementation

`native/src/runner.hpp` holds the runner; `native/src/runner.cpp` is its command line. The workbench builds the command from the current setup and reads the run's log incrementally. A stop request is held until the run reports `running`, because the run clears any stop file left in a reused folder as it starts, and a Stop pressed in the first instant would otherwise be deleted as left over.
