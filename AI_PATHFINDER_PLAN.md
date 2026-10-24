# The AI Pathfinder: the plan

**Written:** 2026-10-13, for GucciBot 2.0. **Status:** a plan; nothing here is built yet.

**Superseded 2026-10-24** by [docs/AI_KNOWS_THE_GAME_PLAN.md](docs/AI_KNOWS_THE_GAME_PLAN.md): an AI that knows what the game is (Nigel: "rather than patching one thing at a time"). The route map and planner described here (built through step 5) stay as parts of it.

**The idea (Nigel):** "the AI plays through the level with knowledge and context of the whole level". It makes the decisions; GD's physics makes the path from those decisions.

**The detail behind this plan** is in [docs/AI_PATHFINDER_RESEARCH.md](docs/AI_PATHFINDER_RESEARCH.md): the Bloodbath evidence and two full designs. Earlier versions of this plan are in git history.

---

# Part 1: In plain English

## What we found out first (the Bloodbath evidence)

I'm only fairly sure of this, not certain. Today's pathfinder gets stuck at Bloodbath's Michigun UFO because of **how it decides**:
- It keeps whichever idea survives longest. Often the only way through is a plan that looks worse for a second but is in the right place.
- Its new ideas are mostly the failed run with **one** click nudged a few ticks, so it never builds the many-flap combination the UFO part needs.
- Once it's stuck, it repeats the same cached searches forever.

The decisions that matter seem to be within about 10 seconds of the death. Nothing in the logs shows a choice from much further back.

One other possibility gets checked first: **the simulator could be wrong there.** Its trigger model was switched off on Bloodbath, and at one point the plan was 42 units off the real player. If the simulator is wrong, no planner fixes that.

## How the new pathfinder works

1. **It reads the whole level at once** and draws its own map: where the gaps are, where the level forks into an upper and a lower way, and where every orb, pad, portal and mode change sits.
2. **It plans a whole route from start to finish,** like a sat-nav. A route is a list of decisions: "go under this block", "use this orb", "skip that pad", "be low and falling when the UFO part starts". Tight gaps and fast clicking cost more, so it picks the route that looks easiest. No training needed.
3. **It plays through it.** For each stretch between two decisions (a leg), GD's own physics tries every click combination and finds the exact clicks. The real game plays them, like today.
4. **If physics proves a decision impossible,** meaning it tried every combination, that decision is crossed off for good and the route is re-planned. It jumps straight back to whichever decision caused the problem, even if that was 30 seconds earlier. It never retries a dead choice, so it can't spin.
5. **Each finished macro is replayed from a cold start** before it counts.

With it switched off, you get today's pathfinder exactly.

## Why this should fix stuck spots

| Today | New |
|---|---|
| No plan; keeps whatever survives longest | An explicit plan of decisions for the whole level |
| Changes one click at a time | Each leg tries every click combination |
| Goes back at most 10 seconds, a few ticks at a time | Jumps back to the decision that caused it, at any distance |
| Repeats the same searches when stuck | Crosses off what's proven dead; never retries it |

**What no planner fixes:** the simulator being wrong. That's checked in R0.

## Training, size, downloads

- **No training needed.** It works from the level's data and GD's physics alone.
- **Optional later:** a small trained "decider" (about 0.5 MB) that picks the right route the first time more often. It would learn from levels GucciBot solved itself, overnight on your PC. Only worth doing if planning turns out slow.
- **What ships is code only,** a few hundred KB. Nothing to download, no Claude, no internet.

## What you'll see, step by step

| Step | What it adds | What you test |
|---|---|---|
| **R0: measure, plus simulator checks** | Simulator speed and memory cost. Two checks: run a known-good Bloodbath macro through the simulator, and find out why the trigger model switches off on Bloodbath. | A minute of pathfinder on Zafari 2, the speed-test button, and opening GD twice. Have a Bloodbath macro handy if you have one. |
| **R1: the level map** | The whole-level map drawn over the game: gaps, forks, orbs, pads, portals, mode sections. | Does the map match Zafari 2, Xmas Challenge and Bloodbath? |
| **R2: the planned route** | The planner draws its whole-level route over the level. It doesn't drive yet. | Does the route look like how you'd play it? |
| **R3: physics plays the legs** | Each leg is found by trying every combination (the physics map). | Bloodbath: does a leg get past the Michigun UFO? |
| **R4: it drives** | The route engine plays levels through, re-planning when a leg is proven impossible. | Zafari 2, Xmas, Bloodbath from the start. |
| **R5: long jumps back and route memory** | Re-plans any distance back, and remembers routes that worked. | Levels with an early mistake that only bites later. |
| **R6: proof runs** | The two "fix later" bugs fixed, the cold replay check, and an overnight test mode over a list of levels. | One night unattended, then a solved/failed list. |
| **R7: breadth and release** | Fixes from the overnight runs; docs and credits. | RobTop's levels and your 20 online levels, then the release test. |

The optional trained decider would come after R6, only if needed.

## The bar for 2.0 (agreed 2026-10-13)

1. **The floor:** from the start, unattended, it solves Zafari 2, Xmas Challenge and Bloodbath. Each passes the cold replay.
2. **The real goal:**
   - every official RobTop level that isn't a platformer level;
   - at least 16 of 20 online levels covering every game mode (you pick or approve the list).
3. **It's never slower than today's pathfinder** on the floor levels.

## What could go wrong

- **The simulator may be wrong at Bloodbath.** R0 checks before anything is built on it.
- **Trying every combination can blow up** in flying modes (ship, wave, UFO), where states rarely repeat. That shows up as running out of budget instead of a proof.
- **The level map is approximate on some 2.2 content:** rotated hazards, moving objects, teleports. The map only guides; physics decides.
- **Speed and memory** are unknown until R0.
- **Mac, iPhone and Android** need the trigger model ported first, which is on your "port everything" list.

## How long

Not sure until R0. Probably a few weeks, paced by usage.

---

# Part 2: Technical design (for whoever builds it)

The full design is in [docs/AI_PATHFINDER_RESEARCH.md](docs/AI_PATHFINDER_RESEARCH.md) §2 (Design A, chosen). The trained option is §3. The Bloodbath evidence and the cheapest tests are §1. The essentials:

- **The atlas (R1):** built once per level from `classify()` boxes (`src/absense/gpu/client.cpp`), portals, orbs, pads and mode/speed/size sections. It contains:
  - **gates:** columns where the free space splits or an interaction happens;
  - **options per gate as WAYPOINTS:** x, a y-band, an optional vertical-speed band, expected mode/size/gravity, and required and forbidden interactions since the previous gate.
  - Rotation is baked into axis-aligned boxes, so rotated hazards over-block. Moving objects come from the World where it can predict them.
- **Movement limits:** measured from GD itself at level start. The exact sim flies a copy in open air per (mode, speed, size, gravity) to get climb and dive slopes, flap and jump arcs, and so on. Cached per level, a few KB.
- **Route search (R2):** A*/beam over gates. Edge costs come from the movement limits (tightness, click rate), and impossible edges are cut. The route is a list of waypoints. A "two-leg rule" refuses entries already doomed one leg on.
- **The leg executor (R3):** the physics map from the entry set, constrained to the next waypoint's band.
  - **Search mode:** merged and capped.
  - **Proof mode:** exact keys, no pruning, no cap reached. A proof covers whole-tick inputs at this TPS from that entry set only; sub-tick/CBF inputs and quantized merging are outside it.
- **On "no":**
  - a **nogood** is recorded for that option, given its entry;
  - the search **backjumps** to the gate responsible, at any distance, replaying the committed macro to an earlier tick for jumps past the kept starts (about 10 s);
  - **budget-outs are soft** (re-tried with a larger budget), **proofs are permanent**.
- **Integration:**
  - The route engine replaces `decide()`/repair as the driver behind a switch. Today's engine stays as the fallback and the "off" behaviour.
  - The trust chain is unchanged (`evaluate()`, `robust()`, real-tick commit, `kDivergence` re-plan, `judge::run`), plus the cold confirm run (R6).
  - `memory::Store` keeps route choices that worked.
- **Hard constraints:**
  - The simulator is main-thread only; work runs in deadline-bounded slices.
  - Determinism: same inputs, same answers, with stable orderings.
  - The World is Windows-only.
  - Snapshot cost is unmeasured (R0).
- **First tests (R0, cheapest first), from the evidence:**
  1. Replay a known-good Bloodbath route in the simulator from about tick 7100–7300 and compare with `gucci_sim_vs_real` / `gucci_tick_probe` through 8700. If the sim kills a route the game passes, fix the sim first.
  2. Find why the World switches off on Bloodbath ("the World state after branches is different").
  3. A physics-map sweep from stops between about 7100 and 8600.
- **The trained decider (optional, research §3):** a masked decision in-painter over the whole-level tape (dilated 1-D conv, about 200–250k params, about 0.4–0.8 MB). It's trained on GucciBot-generated labels with a C++ trainer, ships as `resources/ai/decider.gbnet`, and only orders the planner's choices; physics still decides.
- **CLAUDE.md rules hold:**
  - one commit per logical change;
  - `GB_BUILD_LABEL` bumped every build;
  - no build while GD runs;
  - Nigel's test before any release.
