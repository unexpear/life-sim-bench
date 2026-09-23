# Sorting lab

Open **Sorting lab: 2D**, **Sorting lab: 3D** or **Sorting lab: disparity sphere** from the library. These are three starting views of the same model; only one simulation is loaded at a time. **View** switches between 2D bars, 3D columns, the disparity circle and the disparity sphere without touching the current sort, input or counters.

## Watch and control

- **Run / Pause** controls playback. **Step** advances the number of events in **Events per step**. Choose **1** for a single comparison, swap, copy, key read or rod drop.
- Sorting stops automatically at completion. **Replay** (or Space) restarts the same input. **Reset** also returns to that input.
- **New data** advances the visible seed and pauses at a fresh input. For Sorted and Reversed inputs the seed has no effect; Nearly sorted and Few unique depend on it.
- Algorithm, Items, Starting data, Sort order and Data seed are setup choices. Use **Apply & restart** after changing them. View and Events per step change live.
- In 3D columns and the disparity sphere, drag to orbit and scroll to zoom. Use Front, Top, Isometric, Fit view and the projection control. Columns follow array order left to right in each row, starting at the front and progressing to the back. Orbiting changes where these rows appear on screen.
- Amber marks a comparison, coral a swap or copy, violet a key read, green a completed sort. The readout under the picture describes the latest event in words, including the actual compared values. Merge compares items in its working buffer, highlighted in the lower strip instead of the main array.

## Views

| View | What it shows |
| --- | --- |
| 2D bars | One bar per item, height by value, in array order. For Gravity each row becomes its beads, one cell per rod, once that row's beads are laid; rows still waiting keep their bar. |
| 3D columns | The same bars as shaded columns, in rows from front to back. |
| Disparity circle | Items as wedges clockwise from the top. A wedge reaches the rim when its item is in place and shrinks toward the centre the further the item is from home. Distance is counted both ways round the circle, because the array wraps there. A finished sort is a full colour wheel. |
| Disparity sphere | Items as balls along a golden-angle spiral from the top of the sphere to the bottom, evenly spaced. A ball sits on the surface when its item is in place and nearer the centre the further it is from home, counted along the array. A finished sort is a full sphere. |

In both disparity views colour follows value, from red at the lowest key to magenta at the highest, and is kept when the sort completes. **Home** is the run of positions an item's key occupies in the finished array, so a duplicate anywhere inside its run counts as in place. A value the finished array does not contain is never home, which is how Gravity's rows read while they are being rebuilt. Every item keeps a small minimum reach, and a circle wedge too narrow to cover a pixel is marked with a dot, so every item is drawn at any size. Home is known only to the display; it is not work any algorithm performs, and it is not counted.

## Algorithms

| Algorithm | What to watch | Stable for equal values? |
| --- | --- | --- |
| Bubble | Adjacent out-of-order values exchange; a pass with no exchanges finishes early. | Yes |
| Selection | Each scan selects the next extreme and moves it into position. | No |
| Insertion | Values move backwards through an already ordered prefix. Try Nearly sorted. | Yes |
| Shell | Insertion sorts with decreasing `3h+1` gaps, ending with adjacent items. | No |
| Merge | Runs of length 1, 2, 4, … merge using a visible working buffer. | Yes |
| Quick (3-way) | The middle item's value partitions a range into less, equal and greater groups. Try Few unique. | No |
| Heap | Builds a heap, moves its root to the end, then restores the remaining heap. | No |
| Cocktail shaker | Bubble passes alternate direction: forward carries the last item home, backward the first. | Yes |
| Odd-even | Brick sort: every odd-indexed pair, then every even-indexed pair, until a round of both exchanges nothing. | Yes |
| Comb | Bubble passes over a gap that shrinks by a factor of 1.3; a clean pass at gap 1 finishes. | No |
| Gnome | Steps forward past ordered pairs, exchanges a disordered pair and steps back. | Yes |
| Cycle | Takes an item out, counts the keys that belong before it and writes it there, taking out the item it displaces. Every misplaced item is written exactly once. The readout shows the held value. | No |
| Pancake | Finds the item that belongs last in the unsorted prefix, flips it to the front, then flips the whole prefix. Flips are shown as runs of swaps. Among equal keys it takes the last, so one already at the end costs no flip. | No |
| Bitonic | A sorting network: halves sorted in opposite directions, then merged by half-cleaners. Works for any number of items. | No |
| Odd-even merge | Batcher's sorting network, in its iterative form for any number of items. | No |
| Radix LSD (base 10) | Reads every key to find the range, then per decimal digit, least significant first: reads each digit, copies each item into its bucket in the buffer, copies the buffer back. No comparisons. Digits are the key's own decimal digits, so the readout matches the value; keys are shifted up only when some are negative. | Yes |
| Radix MSD (base 10) | The same distribution, most significant digit first, then again inside each bucket of two or more items on the next digit. | Yes |
| Gravity (bead) | Reads every key to find the range, lays each row's beads on an abacus, then drops each rod in turn so its beads fall to the end of the array. Each row's value is rebuilt from its beads. | Does not apply |

These are original incremental implementations. The classic seven follow the methods described in [Sedgewick and Wayne's sorting chapter](https://algs4.cs.princeton.edu/20sorting/) and [NIST's bubble sort entry](https://xlinux.nist.gov/dads/HTML/bubblesort.html). Bitonic follows H. W. Lang's construction for arbitrary n, and odd-even merge follows K. E. Batcher's network.

Bubble, Selection, Insertion, Cocktail, Odd-even, Gnome, Cycle and Pancake can take quadratic work. Bottom-up Merge and Heap take O(n log n) worst-case work, and the two networks O(n log² n) comparisons whatever the data. Quick can take quadratic work on unfavorable inputs. Shell's and Comb's bounds depend on their gap sequences. Radix takes a pass per decimal digit of the key range. Gravity takes one rod drop per unit of key range: its work follows the size of the values, not the number of items.

Stability means retaining the input order of equal keys; it is tested using each item's original identity, although identity labels are not drawn. Gravity does not move items at all, so its result carries no identities. Sorting descending, radix fills the buckets in reverse digit order and Gravity drops beads toward the front.

## Compare fairly

Keep **Items**, **Starting data**, **Data seed** and **Sort order** the same when changing algorithms. Data generation is independent of the algorithm. Shuffled, Reversed, Nearly sorted, Few unique and Sorted are defined against low-to-high order, so Sorted input is deliberately reverse input for a descending sort.

Counters record actual sorting work, excluding dataset generation, rendering, checks and the displayed home positions:

- **Comparisons:** comparisons between two keys. Quick's three-way comparison counts once, including an equal result. Cycle sort's comparisons are against the held item, including the equality checks that place duplicates side by side.
- **Swaps:** exchanges of two distinct array positions. A swap adds two **Array writes**; self-swaps are omitted.
- **Array writes:** writes to the main array. These include Merge's and radix's copies back from the buffer, cycle sort's placements, and Gravity's row changes: one per row whose bead count changes when a rod drops.
- **Buffer writes:** copies into Merge's or radix's working buffer, and Gravity's abacus: every bead laid, plus one per bead cell a drop changes. They are counted separately from main-array writes.
- **Key reads:** a key examined without comparing it to another, as when radix and Gravity find the key range, radix reads a digit, or Gravity lays a row's beads.
- **Events:** comparisons + swaps + single-item copies + key reads + rod drops. A swap is one event and two writes; a rod drop is one event, however many beads fall.
- **Adjacent order:** the fraction of neighbouring pairs already in the requested order. This can rise or fall and is **not** a completion percentage.
- **Mean displacement:** the average distance of an item from home, as a fraction of the array. It is 0 exactly when every item is home, and about 0.5 for reversed input. The disparity circle measures the same distance the shorter way round the circle, which is why reversed input leaves both the ends and the middle at the rim.

The Metrics tab plots these values and exports CSV. Counts are useful for comparing algorithm work; playback time includes drawing and is not a sorting benchmark. **Events per step** batches the same event stream and does not change the final ordering or counts. The sorted values remain on screen until replay or reset.

## Implementation and limits

The engine supports empty and singleton inputs and negative keys for verification; the UI offers 16–512 items. It stores current algorithm state, not a prerecorded event tape. Space is O(n) for every algorithm but Gravity: Merge's and radix's buffers, Quick's range stack, Bitonic's O(log n) frame stack and MSD's span stack. Gravity's abacus holds n rows × key range cells, at most 512 × 511 in the UI. The engine refuses an abacus past 16,777,216 cells rather than allocating one. The 3D views use real geometry, face shading, perspective or parallel projection and a pixel depth buffer. Rendering is deferred until the host requests a picture.

`bench_sorting` checks all eighteen algorithms in both directions on:
- five input distributions at sizes through 512
- equal, negative and widely spread keys, and both extremes of `int`
- all 720 permutations of six items
- every 0-1 input up to ten items

For each run it checks item preservation, stability where claimed, that each call makes exactly one comparison, swap, copy, read or drop, event and write accounting, completion, and zero final displacement.

The two networks are checked on every 0-1 input up to sixteen items, together with a check that their comparisons do not depend on the data. By the 0-1 principle, those two checks prove each network correct at those sizes.

Exact fixtures cover bubble, insertion, selection, merge, gnome, pancake's tie-break, both radix sorts (including the readout's digit and a negative key) and Gravity. Bitonic and odd-even merge are checked against their published comparator counts for 2 to 512 items. Cycle sort's single write per misplaced item is checked on every distinct-key input.

The simulation checks cover:
- repeatable seeds
- all four views drawing the array mid-sort for every algorithm, each differently
- view switching that preserves progress
- orbiting that moves the sphere's items, and projection
- the sphere entry
- the displacement metric, including a Gravity sort that is not yet home
- beads laid row by row, hanging above the rods that have dropped, and finishing sorted
- playback-rate independence

Picture checks compare the drawing area alone. The title, the view's name, the counters and the readout all change by themselves, so a whole-image comparison would pass even for a view that drew nothing.

The normal workspace checks also exercise the real completion, pause and replay controls, staged setup, the disparity sphere entry and a Gravity sort through the frame loop.

## Rendering quality

All four sorting views use four samples per output pixel. The circle's sectors,
3D silhouettes and guide rings are smoothed before the final 1120 × 680 picture
is produced. Every 3D sample has its own depth, so overlapping objects remain
correct at their edges. Sphere lighting has a soft highlight, and the readouts
use an original antialiased stroke alphabet.

The workspace now filters rendered images when resizing: interpolation when
enlarging, and pixel-area averaging when shrinking. This also benefits the other
simulations that supply rendered images. Discrete cell fields keep their existing
sharp magnification. Camera controls, sort events and counters are unchanged.

Drawing buffers are reused and rendering remains on demand. The sorting canvas
uses about 26 MiB for its picture, four-sample colour buffer and depth buffer;
opening the circle adds about 4.4 MiB for its cached polar coordinates. Hidden box
faces are skipped, and triangle edges advance incrementally. This is CPU rendering;
dense columns cost more to draw than bars or spheres.

`bench_rendering` checks reconstruction at native, larger and smaller sizes,
thin-line visibility, gamma, panning, discrete-cell preservation, partial edge
coverage, clipping, draw-order-independent depth in both projections, colour
resolution and render-only state invariance. Its optional output prefix writes
64- and 512-item previews of every view at full size and at a small window size.

The reconstruction approach follows the sampling and filtering principles in
[Physically Based Rendering](https://www.pbr-book.org/4ed/Sampling_and_Reconstruction/Image_Reconstruction).
