# Rule lab

Open **Rule lab · custom cellular automata** from the library, or choose **Create simulation → Rule-based simulation (no coding)**. Creating a rule works even with an empty template installation and without C++ build tools.

Click the rulestring field to edit. It supports selection, cursor movement, copying, pasting and undo. **Ctrl+A** selects all. An invalid draft shows a specific error and leaves the current world intact.

**Apply & restart / Enter** applies the rule and any staged world settings, then starts a fresh paused world. World size, density and seed settings are retained. **Cancel / Escape** restores the currently applied rule text. Example rules fill the editor; apply them when ready. The preview shows neighbourhood size, state count and the normalized rule.

## Supported notation

| Family | Example | Meaning |
| --- | --- | --- |
| Birth/survival | `B3/S23` | Birth with 3 live neighbours; survival with 2 or 3 |
| Older survival/birth | `23/3` | Same as `B3/S23` |
| Generations | `B2/S/3` or `/2/3` | Three states; only state 1 counts as a live neighbour |
| Hensel | `B2-a/S12` | Birth with two neighbours except adjacent corner/edge pairs; survival with one or two |
| Four neighbours | `B2/S12V` | Orthogonal north/east/south/west neighbours |
| Six neighbours | `B2/S34H` | Hexagonal connectivity on the displayed square grid; northeast and southwest are excluded |
| Combined | `B2-a/S12/256` | Hensel conditions with 256 states |

Lowercase, reversed B/S fields, underscore separators and surrounding whitespace are accepted. State counts may also have a `C` or `G` prefix. Generations supports **2–256 states**; a two-state rule normalizes to ordinary B/S notation. States 2 onward advance until they return to zero and cannot be reborn during that decay.

Hensel letters describe neighbourhood arrangements up to rotation and reflection. A minus excludes the listed arrangements. The editor normalizes letters and chooses the shorter equivalent positive/excluded form. Hensel arrangement letters apply to the eight-neighbour grid, not to H/V neighbourhoods.

The world is a finite square grid that wraps at all edges. `H` changes connections, not how the cells are drawn. **B0, MAP, Larger than Life/HROT and topology suffixes such as `:T30,20` are not supported in this version**; the editor reports an error instead of substituting another rule.

## Saving, imports and large runs

**Save as** stores the applied rule and world settings in a `.benchsim` file. Apply an edited rule before saving it. Named simulations are also saved when switching or closing normally. Reopening starts the same seeded initial setup, including Generations rules; it does not restore the live cell arrangement or history. Export/import an RLE pattern separately when needed.

When importing RLE into a supported rulestring world, an incompatible rule header or an out-of-range cell state is refused before the existing cells are cleared. Equivalent notations are accepted. Apply the matching rule first, then import the pattern.

Dedicated runs use the applied rule and applied settings, regardless of unfinished text in the editor. The same parser and transition table drive the app and runner.

## References and verification

Rule definitions were checked against [Golly QuickLife](https://golly.sourceforge.io/Help/Algorithms/QuickLife.html), its [Hensel diagram](https://golly.sourceforge.io/Help/Algorithms/hensel.png), the factual neighbourhood assignments in [Golly's liferules.cpp](https://raw.githubusercontent.com/AlephAlpha/golly/master/gollybase/liferules.cpp), and [Golly Generations](https://golly.sourceforge.io/Help/Algorithms/Generations.html). The parser, symmetry expansion and table construction are independently implemented here.

`bench_rulestrings` checks malformed inputs, canonical examples, exhaustive local transitions, Hensel symmetry/class coverage, wrapped boundaries, the 256-state decay limit, matching seeded restarts and agreement with five reference models. The hidden Win32 harness additionally exercises editing, application, cancellation, import mismatches, dedicated-run handoff and saved rules in fresh processes.
