# The roster

The desktop library now contains 37 simulations. The notes below cover the original collection, the movement lab and sorting labs; [the registry](native/src/registry.hpp) lists every current entry. Each simulation's in-app Guide states its provenance, approximations and replication verdict.

## Sorting

**Sorting lab: 2D** (`sorting2d`), **Sorting lab: 3D** (`sorting3d`) and **Sorting lab: disparity sphere** (`sortingsphere`) are three starting views of one model with eighteen incremental algorithms:
- the classic seven: Bubble, Selection, Insertion, Shell, Merge, three-way Quick and Heap
- Cocktail shaker, Odd-even, Comb, Gnome, Cycle and Pancake
- the Bitonic and Odd-even merge sorting networks
- base-10 Radix LSD and MSD
- a bead-by-bead Gravity sort

Watch 16–512 values as bars, columns, a disparity circle or a disparity sphere, compare repeatable inputs, change views during a run, and inspect actual comparison, swap, write and key-read counts. They rearrange data and do not replicate. See the [sorting lab guide](native/SORTING-LAB.md).

## Creature movement

**Creature evolution: movement lab** (`locomotion`) is an original sandbox inspired by Evolution by Keiwan. Build a body from joints, bones and muscles, then evolve neural controllers for walking, jumping or stairs. It supports custom body files, undo, champion replay and interruptible training. Controllers are bred by the experiment; creatures do not reproduce in the physical world. See the [movement lab guide](native/MOVEMENT-LAB.md).

Verdicts: ● replicates · ◐ disputed · ○ does not

## Where it started

| | Sim | Year | Who |
|---|---|---|---|
| ● | **Conway's Game of Life** | 1970 | Conway; Gardner's column, *Scientific American* 223(4) |
| ● | **Von Neumann, 29 states** | c.1952 | von Neumann, on Ulam's lattice suggestion; pub. Burks 1966 |

Life is the reason anyone knows this field exists. The von Neumann machine is the thing it was a deliberate simplification **of** — a universal constructor plus a universal computer reading a tape describing both. The 1995 Nobili–Pesavento implementation needs a tape 145,315 cells long and 63 billion timesteps for one copy, which is exactly why Conway went looking for something smaller.

Life's own replication took forty years: Gemini (Andrew Wade, 2010).

## The lattice lineage

| | Sim | Year | Note |
|---|---|---|---|
| ● | **Langton's Loops** | 1984 | 86 cells. Langton threw away universal construction and bought a factor of 2000 in size. Replicates and does nothing else. |
| ● | **HighLife** | 1994 | B36/S23 — one digit from Life, and it has a free 12-cell replicator. Which is why replication *alone* was never the interesting property. |
| ○ | **Wireworld** | 1987 | Not alive, not trying. A medium for building circuits; a full CPU was built in it in 2002. |
| ○ | **Brian's Brain** | 1980s | Everything moves, nothing persists. The refractory state stops waves re-igniting behind themselves. |
| ○ | **Seeds** | — | B2/S. Nothing survives even one generation. The far side of the boundary Life sits on. |
| ○ | **Day & Night** | 1997 | B3678/S34678. Symmetric under swapping live and dead. |
| ○ | **Langton's Ant** | 1986 | Two rules, one agent. ~10,000 steps of chaos, then a highway nobody has proved must appear. |

## Game theory on a lattice

| | Sim | Year | Note |
|---|---|---|---|
| ○ | **Spatial prisoner's dilemma** | 1992 | Nowak & May, *Nature* 359:826–829. Strategies spread by **imitation, not reproduction** — a cell copies its best neighbour rather than making a copy of itself. On the bench anyway, because a purely game-theoretic rule with no biology in it produces gliders and dynamic fractals. |

Payoffs are the weak PD they used: T=b, R=1, P=S=0. Because P and S are both zero, a cell's score is just its cooperating-neighbour count, times **b** if it defects. That is the whole model — no memory, no reciprocity, no tit-for-tat.

Cooperation survives here on **geometry alone**: cooperators clump, and clumps feed themselves. Axelrod needed repeated play and reputation to get cooperation; this needs only neighbours.

Measured across b (600 steps, 121² torus):

| b | cooperators | churn |
|---|---|---|
| 1.0 | 96% | **0** — frozen utopia |
| 1.2 | 91% | 0.06 |
| 1.4 | 87% | 0.009 |
| 1.6 | 63% | **0.24** — the interlocking-lattice regime |
| 1.8 | 41% | 0.24 |
| 2.0 | 2% | 0.005 |
| 2.2 | 4% | **0** — frozen collapse |

Frozen at both ends, alive strictly between. A perfect world and a dead one are equally static.

## Continuous space — no lattice at all

| | Sim | Year | Note |
|---|---|---|---|
| ○ | **Boids** | 1987 | Reynolds, SIGGRAPH '87. Pure phenotype — no bird has a description of itself to pass on. |
| ○ | **Particle Life** | 2010s | Ventrella's *Clusters*, popularised by CodeParade. **Self-organises; does not replicate**, whatever the internet says. The attraction matrix is copied by whoever runs the simulation, never by the particles. |
| ● | **Primordial Particle System** | 2016 | Schmickl, Stefanec & Crailsheim, *Sci Rep* 6:37969. The only continuous system here that earns a Yes: structures condense from a uniform gas, grow, and divide. |

A note on Reynolds' naming, since it is usually got wrong: the 1987 paper calls the rules **Collision Avoidance, Velocity Matching and Flock Centering**. Separation / Alignment / Cohesion are his own *later* names, from his boids page and the 1999 steering-behaviours work. He coined both — but not in 1987.

A note on PPS and the word "replication": it divides, genuinely. But there is no transcribed description being copied, so it is **division, not von Neumann's genotype/phenotype replication**. Both are real. They are not the same thing, and the bench says so rather than letting a ● imply more than it means.

## Your own

| | Sim | Note |
|---|---|---|
| ○ | **Anneal** | `sims/my_sim.hpp` — the copy-me template. A real documented rule (B4678/S35678, Vichniac's twisted majority), implemented in about forty lines because `GridSim` does the rest. |

## Known approximation

**PPS's equation is a reconstruction, not a transcription.** The published Equation 1 is a typeset graphic and machine readings of it disagree; one extraction gives `Δφ = α·(R−L) + β·N`, which cannot be right, since with the published α=180° a single neighbour imbalance would spin a particle through several half-turns. The implemented form is `Δφ = α + β·N·sign(R−L)`, consistent with the published parameter set ⟨r=5, α=180°, β=17°, v=0.67⟩ and with the reference implementations. Flagged in the source and on screen.

`sign(0)` must be **0**. A ternary that never returns zero breaks the tie in a fixed direction and corrupts every symmetric particle, which is most of the gas.
