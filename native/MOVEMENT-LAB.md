# Creature evolution: movement lab

An original creature movement sandbox inspired by [Evolution by Keiwan](https://keiwando.com/evolution/). It is one of the workbench's selectable simulations (`locomotion`), so the rest of the collection and custom plugins remain available.

## Build, evolve, replay

1. Open the library, search **evolution**, and open **Creature evolution: movement lab**.
2. Press **Run** to watch the default walker population. **Train → Next generation** evaluates one complete generation. Larger batches and continuous training are in the same menu.
3. Open **Edit body** to change the creature. **Move** drags a joint; **Joint** adds one. **Bone** and **Muscle** connect two joints, selected with two clicks. **Erase** removes a link or a joint and its incident links. Keys **1–5** select these tools; **Ctrl+Z** undoes.
4. **Starter bodies** offers a walker, triangle hopper, crawler, and empty canvas. Up to 40 edits can be undone, including clearing the canvas and loading another body.
5. **Return to experiment**, **Run**, or Space validates the body. Disconnected or unfinished bodies stay in the workshop with an explanation. A changed body starts fresh training. Merely opening and closing the editor preserves the current experiment.
6. Once a generation finishes, **Replay champion** reruns the best controller from its original starting state. **Back to population** resumes the experiment's view. Replay does not breed, consume training randomness or change the population.

The canvas follows the current best mover by default. Click one of the first eight population cards to follow that individual; **Follow the best creature** restores automatic tracking. A generation boundary displays the best completed body until the next trial advances. Bone links are pale; muscles are coral when shortened and blue-green when lengthened; joints turn green on ground contact. The muscle bars show commanded length, with a center mark for the original length.

## Objectives and settings

| Task | Fitness, in meters |
|---|---|
| Walk forward | Final center-of-mass x minus settled starting x |
| Jump high | Greatest center-of-mass height minus settled starting height |
| Climb stairs | Final center-of-mass x minus settled starting x, on the stair course |

All trials start from the same geometry at rest. A half-second passive settling interval precedes 3–20 seconds of scored simulation. Negative walking results mean backward travel. A failed numerical trial receives -1000 and cannot outrank a stable mover. Rolling, hopping and flopping may score well; no humanoid gait is imposed.

The population supports 8–128 independent controllers. A body supports 24 joints, 64 links and 16 muscles. Positions snap to a 0.1 m grid inside the build area. Overlapping joints, duplicate links and invalid endpoints are rejected. Bones and muscles may not occupy the same joint pair.

Task, population, trial duration, mutation, muscle stiffness, ground grip and seed are experiment settings; the host stages them until **Apply & restart**. Playback speed changes how much simulated time is shown per visual step. It does not change full-generation results. The Train path yields between short batches of physics ticks, including with large bodies; a single tick and its rendering still finish before the next input is processed.

## Save and load

**Save body** and **Load body** use native Windows file dialogs. The `.creature` format stores geometry, including unfinished bodies. Loading validates a candidate before replacing the current body and opens it in the workshop. Invalid or oversized files leave the current body intact.

Body files do **not** contain trained brains, population state or settings. For a reproducible new experiment, retain the body and the same seed and settings. Use the host's Metrics tab to export fitness curves. Closing the simulation releases the experiment; save custom geometry before switching to another simulation.

## Model and sources

The implementation is original; it does not use the reference game's source or assets. Each joint has equal mass and a 0.1 m contact radius. Bones use nearly rigid distance constraints; compliant muscle constraints vary their target length between 65% and 135% of the designed length, at no more than two original lengths per second.

Distance solving follows [XPBD, Algorithm 1 and equations 17–18](https://matthias-research.github.io/pages/publications/XPBD.pdf), with fixed 1/240-second substeps and six solver iterations. Contact is inelastic, with a Coulomb tangential friction bound. The [Box2D simulation documentation](https://box2d.org/documentation/md_simulation.html) is a reference for fixed stepping and contact friction; Box2D is not a dependency.

The existing workbench MLP supplies eight tanh hidden neurons and sigmoid muscle outputs. Inputs include relative joint positions, contact flags, center-of-mass velocity and height, and sine/cosine clock signals. Networks update at 30 Hz. Three-way tournament selection, uniform crossover, and bounded weight mutation create offspring. The two best controllers survive unchanged. Bodies remain user-designed throughout a run.

This is a 2D approximation: muscles connect joints directly, only joints collide with the ground and stairs, and links can cross. It does not implement solid bone collisions, self-collision, flight, wings or fluid dynamics. Results describe this model rather than a biological or engineering prediction.

## Verification

`bench_locomotion` exercises genome round trips, body validation and malformed imports, analytical free fall, passive and frictionless controls, floor/riser/tread contact, elite retention, seeded evolution, champion replay, all three courses, stiffness extremes, building from an empty canvas, undo, and equivalence between full generations and interruptible training. It also exercises the maximum population and body configuration.

`bench_ui_shot --verify-ui` drives the actual window procedure and checks the body toolbar, letterboxed canvas clicks, validation, undo and replay at both tested workspace sizes. The existing registry, workflow, plugin and other simulation suites remain part of CTest.

Release verification on 2026-09-12: **14 suites passed**, including 239 movement checks and 645 UI checks. All 34 library entries render. The full maximum-size 20-second stair trial completed with 0 of 128 creatures failing. [Verification record](UI-UX-REVIEW.md).
