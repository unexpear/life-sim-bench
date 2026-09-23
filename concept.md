# Concept

Historical design note. The current desktop app uses Win32/GDI; SDL3 below
describes the initial design and is not a current dependency. See README.md for
the current architecture and build instructions.

A bench, not a game. You load a rule, you watch it, you affect it, and the presentation is good enough that watching is worth doing.

## Why a bench

The earlier attempt wrapped these simulations in an incremental game. The simulations were the good part and the game was the part in the way — every mechanic added a reason to *not* look at the screen. So: drop the economy, keep the automata, and spend the effort on making them worth looking at.

## Three rules of the house

**1 · The rule is the published rule.** Not "inspired by". Where something is approximated it says so on screen, next to the thing being approximated. A bench that quietly fudges a transition table is worthless for the one thing a bench is for.

**2 · Every sim states whether it actually replicates.** Self-replication is the most over-claimed property in artificial life — "look, it's alive!" attaches to anything that moves. So the roster carries a three-way verdict — replicates / disputed / does not — with the reasoning, never just the badge. Seeds and Langton's Ant are on the bench precisely *because* they don't replicate.

**3 · The renderer never learns what a cell means.** A sim publishes a grid of palette indices and a palette with labels. The renderer uploads indices. This keeps the rules dependency-free and testable, means a new rule never touches drawing code, and — the reason it actually matters — makes it impossible for the legend to drift out of sync with the colours, because they come from the same array.

## Why the split build

`bench_selftest` has no dependencies. `bench` needs SDL3.

The rules have to be **correct**; the renderer has to be **fast**. Those are different problems with different failure modes, and mixing them means you can debug neither. It also means a graphics driver that will not start can never stop you verifying a transition table.

## Why rendering is the focus

A still frame of a cellular automaton is close to meaningless. What matters is what *moved*, and a fresh clear every frame throws exactly that away. So the renderer's centrepiece is an accumulation buffer: decay the previous frame instead of clearing it, and take the brighter of decayed-versus-current per channel. Gliders acquire trails, wavefronts acquire direction, and a field of dots becomes something with history.

Everything else follows from wanting to actually see things: device-pixel-ratio awareness (single-pixel cells on a hi-dpi screen are otherwise a blurry upscale), zoom that tracks the cursor, pan, nearest-neighbour by default with filtering as a toggle, and a PNG export because a good frame deserves keeping.

## Why C++, and why SDL3 only

Native, because the interesting grids are large and the interesting particle counts are high, and a browser tab spends its budget elsewhere.

SDL3 and nothing else, because this bench draws exactly one thing: a rectangle of indexed pixels. A streaming texture does that at full speed. Every dependency past the first is a dependency someone has to build before they can look at a glider.

If shader-side post-processing later earns its place, that is a decision to take then, on its own evidence.
