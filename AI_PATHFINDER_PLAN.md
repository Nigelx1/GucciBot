# The AI Pathfinder: the plan

**Written:** 2026-10-13, for GucciBot 2.0. **Status:** a plan; nothing here is built yet.

**Sources:**
- Nigel's design (2026-10-03 and 10-05): the AI holds the controller inside the simulator, it reads the level as data, it's built by us and ships inside the mod with no Claude needed, and the physics map is its teacher.
- Two design drafts written on 2026-10-05/06: a search-first one and a learning-first one. This plan merges them and replaces both.

Part 1 is for Nigel. Part 2 is for whoever builds it.

---

# Part 1: In plain English

## What we're building

**The AI holds the controller. It never draws a line.** At every moment it decides what the button does: press, hold or let go, and for how many ticks. It plays those moves on a copy of the player inside the simulator, which runs GD's own player code against the real level. So any path the AI finds is a path GD's physics actually produced. You preferred this over an AI that sketches a rough line for the steering to follow.

**It reads the level as data, not pictures.** Every object near the player goes in as numbers:
- where it is;
- its hitbox;
- what it does (spike, block, orb, pad, portal);
- how it's moving.

You'll also be able to export a whole level that way and see what the AI sees, drawn over the game.

**It learns by trying, millions of times.** It doesn't play millions of full levels; the simulator isn't fast enough for that. It makes millions of short tries of a few seconds each, at the parts that matter. Today's pathfinder already makes 20,000–115,000 of those per level and throws every one away. From now on each try becomes a lesson: what was tried, how long it lasted, and what killed it. Over a few nights of training runs on your PC, that adds up to tens of millions.

**The physics map is its teacher and its safety net.** The sweep finds every way the player can still be alive at every tick from a given spot. It shows as dots on screen.
- **As a teacher,** it gives the AI exact answers to learn from: "from here, something survives" or "nothing does".
- **As a safety net,** when the AI is stuck, the sweep searches everything.

**It ships inside the mod.** That means a file of numbers, well under 1 MB, plus a few hundred lines of C++. No Python, no internet, no Claude. Training happens once on your PC, and the result goes into the mod's files.

**It can make the pathfinder slower, never wrong.** The AI only decides what to try first and where to look. The checks that decide whether something works don't change:
- the exact simulator judges every idea;
- every idea is also tried a tick late;
- the real game plays every tick for real.

New for 2.0: a finished macro also gets replayed from a cold start before it counts. With the AI switched off, you get today's pathfinder exactly.

## How it plays a level

```
level loads -> exported as data (objects, hitboxes, what moves)
   |
   v
at each moment:  the AI suggests moves ("hold 12 ticks", "let go 4 ticks", ...)
                 -> the simulator plays them on a copy of the player
                 -> survivors are checked (does it still work a tick late?)
                 -> the real game plays the next ticks for real
   |
stuck at a death?  the AI judges how far back the real mistake was;
                   the physics map searches everything from there
   |
level complete -> replay the macro from a cold start -> saved only if it completes
```

## What you'll see, step by step

Each step is a build you can try in-game.

| Step | What it adds | What you test |
|---|---|---|
| **M0: measure** | Logs how fast the simulator really is, plus a speed-test button. | Run the pathfinder on Zafari 2 for a minute and press the speed test once in a ship part and once in a cube part. Also try opening GD twice. I read the logs. |
| **M1: the level as data** | Export a level as data, and an "AI view" overlay that draws the boxes the AI sees, coloured by kind. | Open Zafari 2, Xmas Challenge and Bloodbath with Show Hitboxes on. The AI's boxes should sit on the real hitboxes, moving ones included. |
| **M2: the physics map** | The sweep as dots on screen, and as a new search when the pathfinder is stuck. | Bloodbath's Michigun UFO: does the sweep find a way past? Zafari 2 and Xmas Challenge must not get worse. |
| **M3: the AI's first brain** | A first network trained on the sweep's answers and on what the pathfinder already tries. You can watch "the AI's line" (the AI driving a copy, drawn like the path preview) and a danger colour that turns red ahead of deaths. | Play and watch: does red show up *before* deaths? Does the AI's line look sensible? |
| **M4: the AI drives** | The AI's own runs become ideas in the pathfinder, and it picks which ideas to try first. Pathfinder tab switch: **AI**. | Zafari 2 and Xmas Challenge, 3 runs with the AI on and 3 with it off. It should be no slower. |
| **M5: the AI picks where to go back** | After a death, the AI judges how far back the real mistake was. On the hardest spots, it searches many moves ahead. | Bloodbath from the start, plus the two regression levels. |
| **M6: it trains itself** | Overnight training runs: the pathfinder solves a list of levels on its own, the AI learns from them, repeat. | Leave GD running overnight a few times; next day, try each new version on the test levels. |
| **M7: ship it** | The trained AI in the mod; docs and credits updated. | The 2.0 release test. |

## What I need from you

**Four decisions** (my recommendation first):
1. **What the shipped AI learns from.** Recommended: only the pathfinder's own runs plus your own macros. Your PC has 37 Silicate macros and 29 .gdr macros. If other people made them, the AI shouldn't learn from them without their permission; the ToastyReplay lesson applies. Are they yours?
2. **What "done" means for 2.0.** Proposal:
   - From the start of the level, unattended, it solves Zafari 2, Xmas Challenge and Bloodbath.
   - Every solve passes the cold replay.
   - With the AI on, it's never worse than with it off.
   - Change any of it.
3. **The trainer.** It's the program that teaches the AI on your PC, and it never ships. Recommended: Python, which is quicker to write. The alternative is C++ only.
4. **Two "fix later" bugs have to be fixed before the overnight runs.** One: restarting right after a finished replay skips the next attempt's level end. Two: the pathfinder's checker leaves a step flag on. The overnight runs restart right after replays all night, so these come first, before M6.

**For running it:**
- In M0, try opening GD twice. If two copies can run at once, training runs go about twice as fast.
- During M6, leave GD running overnight a few nights. The training itself needs no Claude.

## What could go wrong

- **The simulator's speed is unknown.** Every time estimate here waits on M0. If it's slow, the AI learns only from what the pathfinder already does, plus the physics map, and skips the extra self-training. That still speeds up the search.
- **Only one simulator per GD, on one CPU core.** Your other 11 threads sit idle unless GD can run twice.
- **Bloodbath may not be a search problem.** If the simulator is wrong at the Michigun UFO, no AI fixes it. M2 checks that first.
- **Networks are fuzzy; GD is frame-perfect.** That's why the AI only proposes. The search still tries each idea a tick early and a tick late, and the simulator decides.
- **It may only be good at level styles it has seen.** The test levels are kept out of training so we find out honestly.
- **Mac, iPhone and Android** don't have the trigger model yet, so the AI starts on Windows. It comes to the others once the trigger model is ported there, which is on your "port everything before 2.0" list, and someone tests it.

## How long

I'm not sure, and M0 is what tells us.
- **M0–M3:** normal builds, each a day or two of work plus your test.
- **M4–M6:** a few nights of training runs on your PC, plus a round of fixes after each.

Expect several weeks in total, paced by usage.

---

# Part 2: Technical design

## 1. What it builds on

All in `src/absense/` unless noted. Absense's pathfinder was ported in whole; none of it changed after 2026-10-05.

| Piece | Where | Use |
|---|---|---|
| Exact simulator: copies of the real `PlayerObject` stepped with GD's `update()` + `checkCollisions()`, Silicate's collisions and the World trigger model | `trajectory/trajectory.hpp`: `run()`, `steer()`, `beginSearch()`/`stepSearch()`, `captureStart()`/`useStart()`, `TickInput`, `StartState` | The AI's moves are played here. `steer()` already drives a copy tick by tick; `drive()` (§5a) copies it. |
| Legal moves per mode | `trajectory.cpp` `alternativesFor()` | The action space per tick: UFO flap, ship/wave/swing toggle, cube/robot/ball/spider tap/hold/release, orb re-press and spam cadences. |
| Branch and rewind | `SimSnapshot`; the DFS keeps one every `kSnapEvery` = 4 ticks and re-runs the rest | Tree nodes rebuild the same way (§5c). |
| Same input, same result | `pathfinder.hpp` `RunMemo`; `Stats::unrepeatable` | Labels can be trusted. A level with `unrepeatable > 0` is dropped from training. |
| Decision loop and trust chain | `pathfinder/pathfinder.cpp`: `decide()`, `evaluate()`, `robust()` (1 tick late), trim, tabu, `kDivergence` = 1.0 re-plan, confirmed killers, phantoms, `escalate()`; `kSearchBudget` {40k, 100k, 160k} simulated ticks | Unchanged. The AI's ideas pass through it like any other. |
| Hand-made "learning" | `pathfinder/memory.hpp` (`Priors`: situation -> idea kind -> wins; its header says "Nothing here learns physics"), `human.hpp` | The policy takes over the priors' job and keeps them as a tie-breaker. |
| Level -> typed boxes | `gpu/client.cpp` `classify()`, `changesEverything()`; `gpu/protocol.hpp` `Object{box, radius, power, kind (18 kinds), flags, vx, vy}` | Most of the observation, already written. The GPU scorer that used it is off in GucciBot. |
| Real-game judge, sim-vs-real | `judge.*`; `glue.hpp`; MCP `gucci_sim_vs_real`, `gucci_abs_simulate`, `gucci_tick_probe`, `gucci_open_level`, `gucci_restart_game` | Checking the simulator against the game, and running levels unattended. |

## 2. Hard constraints

- **The simulator runs only on GD's main thread, one per GD process.** `Sim::begin` edits the live `m_gameState`. The copies live in the real object layer, and the World's materializer writes run positions into the real objects and collision buckets, then puts them back. No worker threads. The search runs in deadline-bounded slices so the game keeps drawing.
- **Time units.** `PlayerObject::update(dt)` takes sixtieths of a second, so a 240 TPS tick is `0.25`. Anything stepped by hand uses `60 / tps` (CLAUDE.md §4).
- **Determinism.** A run is a pure function of its start and script. AI rollouts that sample must use a fixed seed, or tabu and memory break.
- **The World is Windows-only** (commit 2c30247). Elsewhere the copies run without triggers, so labels made with the World don't match there.
- **Snapshot cost is unmeasured.** A `SimSnapshot` carries hash maps plus World state, so don't keep one per tree node.

## 3. The level as data

- **Export (offline and debugging):** a Pathfinder-tab button plus an MCP tool `gucci_export_level`. It walks every object, calls `classify()`, adds the triggers by `world::Kind` from the `WorldDef`, the start state (mode, speed, gravity, mini, dual) and the level length, and writes `<save>/levels/<levelKey>.gblevel` (binary, sorted by x). It has to run in-game, because hitbox sizes come from the live objects (`getObjectRect`), not the level string.
- **Live view (what the AI actually sees):** during a sim tick, after the World has placed objects where this run has them, collect the objects near the copy from GD's collision sections, the same query the hitbox drawer uses. A static export would be wrong for anything a move trigger has shifted.
- **One feature builder, in C++:** `src/ai/features.{hpp,cpp}` turns player + nearby objects into numbers. Training shards store its output, so the trainer never reimplements features. A feature-version number goes into every shard and into the weights file.
- **Overlay (M1):** draws the AI's view boxes coloured by kind, flipped with gravity, moving objects included.

## 4. Observation, action, network

**Observation:**
- **Player vector (~28 values):** the `StartState` fields (y, yVelocity, gravity, gravityMod, yStart, speed, speed multiplier, size, mode one-hot, upside-down, on ground, held), plus ticks since the last button change, touching an unused orb, dual flags, and TPS normalised to 240.
- **Object set (32–48 entries):** the nearest objects from about 2 blocks behind to about 2 s of travel ahead (`unitsPerSecond(speed) × 2`), within about ±8 blocks vertically. Per entry:
  - dx, dy, w, h, radius;
  - kind one-hot (18 `ObjectKind` + "changes everything");
  - power, flags, vx, vy.

  Sorted by distance, padded, masked. Flipped vertically when the player is upside down, so "up" always means the same to the network.
- **Why a set, not a grid:** GD is decided to a fraction of a block. A set keeps exact coordinates, and a grid that fine would be huge.

**Action: the AI holds the controller, asked only at decision points.**
- An action is a button move from `alternativesFor()`: let go, hold, tap inside the tick, re-press on an orb, or orb-spam cadence. Each comes with a duration from {1, 2, 3, 4, 6, 8, 11, 16, 24, 32, 48} ticks, scaled by tps/240.
- Illegal pairs are masked off.
- The AI is asked again when the duration runs out, or sooner if a new object enters the view.
- Every tick is still one `TickInput`. A ship hold is one call instead of 48.

**Outputs:**
- **Policy:** which (action, duration) to try first.
- **Value:** the probability the state is *doomed*, meaning no input from here survives the next 2 s. Plus expected ticks to death as an auxiliary head.

**Network:**
- A shared per-object MLP and a max-pool (set encoder), joined with a player MLP, then 128 units, then the two heads.
- About 10k–35k parameters; the weights file is 40–140 KB.
- Measured in the 2026-10-05 scratch benchmark (`/O2 /MD`, one thread): **9.2 µs** per evaluation with AVX2 and 16.6 µs without for the 10k version; 30 / 78 µs for the 34k version.
- That's about the cost of a few simulator ticks, so the network is never asked every tick of every rollout. That's why actions carry a duration.

## 5. How the AI drives (in build order)

**a. `Trajectory::drive(net, ticks, script&)` (M3–M4).**
- A copy of `steer()`, with the network choosing instead of the look-ahead rule. This is the literal "AI holds the controller".
- In `decide()`, next to the steered candidates, `buildNetCandidates()` makes K rollouts: greedy, plus a few samples with a fixed seed. They go in through `addCandidate(... "ai ...")`, and `evaluate()` and `robust()` judge them like every other idea.
- The idea ordering (today `memory::Store::score()` from `priors.json`) uses the policy's score where it has one.

**b. Value-guided repairs (M5).**
- Today a repair probes stops at fixed distances back (1, 2, 3, 4, 6, 8, 12, 16, 24 … ticks).
- Instead, evaluate V on the kept starts between the floor and the death, and probe the latest one the net doesn't call doomed first.
- `escalate()` stays as the fallback.

**c. Guided tree search for the hardest spots (M5).** This is AlphaZero-style PUCT over the simulator, run only when nothing cheap survives the horizon.
- **Node and branch:** a node is a sim state at a tick; a branch is an (action, duration).
- **Expanding a branch:**
  - a branch that dies is dead, exactly;
  - children with bit-identical states merge (a press that changes nothing isn't a decision);
  - each live child gets one network call for V and its children's priors.
- **Selection and pruning:** select by Q + c·P·√N/(1+N) with c ≈ 1.5. A node is dead when all its children are dead, which is exact.
- **Cost and output:**
  - Budget: the same `kSearchBudget`, so A/B comparisons are fair.
  - Snapshots only at visited depths, every k; other nodes are rebuilt by re-running.
  - The subtree is reused after commits.
  - The best reached path becomes one "guided" candidate.

**d. The sweep, Nigel's physics map (M2).**
- Breadth-first over ticks. It keeps every alive state, merges near-identical ones (quantised y, yVelocity, mode, held), and keeps each state's input history.
- Before there's a net, it's a repair search at stops. Once there is one, the frontier is capped to the top-N by V.
- It's also the label source in §6.
- The overlay draws the alive states as dots.

**Not used:** pre-training on Absense's approximate GPU physics (`src/gpuapp` in the Absense zip, not in this repo). It can't model triggers, dual or teleports, so it might teach wrong habits. Revisit only if exact-sim training is too slow.

## 6. How it learns

**Data, in order of exactness and cost:**
1. **Sweep labels: exact.** From many states on training levels, a state gets V = 1 if the sweep finds a descendant alive 2 s later, with a witness path. Otherwise V = 0, which is near-exact, since merging can hide a rare survivor.
2. **Pathfinder decision logs: free.** `decide()` already judges 20k–115k ideas per solve. A logging switch writes, per decision:
   - the features;
   - each judged idea mapped to its first (action, duration);
   - the ticks it survived, its killer, and the verdict.
3. **Self-play rounds (expert iteration).**
   1. The AI-guided pathfinder solves curriculum levels, writing shards.
   2. Retrain.
   3. Ship the new weights into resources.
   4. Repeat.

   Policy target: the visit spread at decision roots, or the ideas that cleared `need`. Value target: exact outcomes, plus sweep labels where available.
4. **Macro imitation: optional, waits on Nigel's decision 1.** Play each macro on the real level with a tick logger and record (features, button) per tick. This needs a `.gdr` reader and `gucci_open_level` extended to saved online levels (today it only opens local created levels).
5. **Reinforcement learning (PPO from the round-3 weights): last and optional.** Only if M0's speed allows it.

**Curriculum:**
1. RobTop's main levels.
2. Easy online levels, one mode each.
3. Mixed modes.
4. Demons.

Order by "simulations needed to solve" from the logs. Add a reverse curriculum: start runs from kept starts just before each level's remembered spots.

**Held out, never trained on:** Zafari 2, Xmas Challenge, Bloodbath.

**Harness (M6):**
- An in-mod "training run" mode walks a level list: open, start from the beginning, pathfind with logging on, cold-replay confirm, quit, next.
- A PowerShell watchdog relaunches GD after a crash and resumes the list.
- It writes only to the mod's save folder.
- **Prerequisite:** fix the restart-after-replay level-end skip and the judge's armed step flag (Nigel's decision 4).

**Trainer:** `tools/ai_train/`, Python + PyTorch on the CPU (Nigel's PC: i5-12400, 6 cores; RX 6600, so no CUDA), or C++ if Nigel prefers. It never ships. A README covers how to retrain after a GD update without Claude.

**Estimates (unverified; M0 replaces them):**
- **Simulator speed:** 20k–200k ticks/s. One simulation has a measured fixed cost of about 0.12–0.13 ms.
- **Per self-play round:**
  - with GD on one instance: about 30–200 levels × a few minutes each, a night or two;
  - training the network: minutes to an hour on the CPU.
- **Shards:** about 250 B per sample; tens of millions of samples is a few GB.

## 7. In the mod

- **Code:** `src/ai/features.*` and `src/ai/net.*`: dense layers, ReLU and max-pool, about 200–300 lines, no libraries.
  - `net.cpp` is built with `/arch:AVX2` and picked at runtime by CPUID, with the plain build as fallback.
  - It builds on all five CI platforms.
- **Weights file:** `resources/ai/policy.gbnet`.
  - Header: format version, feature version, layer shapes. A version mismatch turns the AI off, and so does a missing or corrupt file, with a logged warning; the pathfinder then behaves exactly as today.
  - **Golden test** in `run_tests.bat`: the C++ outputs must match the trainer's reference vectors to 1e-5.
- **Settings:** a Pathfinder-tab **AI** switch, default on once M4 shows it's no worse. "AI view" and "AI line" overlays.
- **Platforms:** Windows first. Elsewhere it ships off by default until the World runs there and someone tests on a device.

## 8. Checking a found path for real

Unchanged:
- The exact sim judges every idea, each also tried a tick late.
- Committed ticks are real ticks.
- A drift over 1.0 unit forces a re-plan.
- Disagreements go to `judge::run` and the distrust list.

**New, for 2.0: an automatic confirm run.**
1. When the level completes, do a cold full restart.
2. Play the saved macro through normal playback.
3. Run Check Macro.

Only then does it count as solved. Absense has never had this; Zafari 2's replay was checked by hand, which is how the dropped tap releases (-bq) went unseen.

## 9. Milestones and exit checks

| # | Delivers | Exit check |
|---|---|---|
| M0 | `stopped:` line logs simulated ticks, sim/restore/real time and ticks/s; `gucci_abs_benchmark` (µs per tick, µs per snapshot save/restore); a second-GD-instance check | Real numbers replace §6's estimates. Below ~10k ticks/s, drop RL and shrink the self-play plan. |
| M1 | `classify()` moved to `src/ai/level_data.*`, `.gblevel` export, live view, AI-view overlay | Nigel finds no wrong or missing gameplay objects on the 3 test levels. |
| M2 | Sweep: repair search plus dots overlay | Bloodbath either gets past tick ~8676, or there's proof that nothing alive within the 10 s window does. The second sends the work to the window size or to sim accuracy (`gucci_sim_vs_real` there). No regression on Zafari 2 or Xmas. |
| M3 | Logging switch, trainer, `policy.gbnet` v0 (sweep labels plus decision logs), C++ inference with golden test, AI line and danger overlay | V predicts death 2 s ahead with AUC ≥ 0.9 on held-out levels, measured offline; per-call cost logged. |
| M4 | `drive()` candidates and policy ordering; **AI** switch | AI on vs off, 3 runs each on Zafari 2 and Xmas: median time and simulated ticks per decision no worse, ideas tried per decision down. |
| M5 | Value-guided repairs and the guided tree | Bloodbath past the Michigun UFO from the start; no regression. |
| M6 | Confirm run, the two bug fixes, harness, self-play rounds v1–v3 | Each version solves at least as many held-out levels as the last, at equal or lower median time. |
| M7 | Final weights in resources; about.md, README, in-game credits and site | Nigel's release test; §10 met. |

CLAUDE.md rules hold throughout:
- one commit per logical change;
- `GB_BUILD_LABEL` bumped every build;
- no build while GD runs;
- Nigel's in-game test before any release.

## 10. "Done" for 2.0 (a proposal for Nigel to accept or change)

1. From the start of the level, unattended, it solves Zafari 2, Xmas Challenge and Bloodbath. Every solve passes the cold-start confirm run and Check Macro.
2. **AI on is never worse than AI off.** On the regression set, median solve time stays within +10%. On ~10 held-out levels it solves strictly more.
3. **It all ships in the mod:**
   - `policy.gbnet` and the C++ inference;
   - the golden test;
   - a clean fallback when the file is missing.

   No Python or Claude at runtime. Training is documented and repeatable.
4. **Platforms:** Windows is supported. Elsewhere it's off by default until tested on a device.
5. Nigel's release test passes, and credits match across about.md, README, in-game and the site.

## 11. Risks

1. **Throughput** (M0 decides): RL and big self-play only if the sim is fast enough. Expert iteration on existing logs works regardless.
2. **One sim per process, on the main thread:** parallelism needs multiple GD instances (unknown).
3. **Bloodbath may not be a search problem:** M2 and `gucci_sim_vs_real` separate a search problem from a sim-accuracy one first.
4. **A confidently wrong V can starve the only route.** PUCT never zeroes a branch, every old idea source still runs, and `escalate()` is unchanged. The worst case is slower, never wrong.
5. **Fuzzy net vs frame-perfect game:** the net proposes, ±1-tick variants and the exact sim decide. Expect help on routing more than on timing.
6. **Generalisation and data skew:** the curriculum, the held-out set, and no training on the test levels.
7. **Train/run feature drift:** one C++ feature builder, version numbers, the golden test.
8. **Data provenance:** others' macros only with permission (Nigel's decision 1).
9. **Off Windows:** no World, so labels mismatch; off by default there.
10. **Open bugs that would fake failures:** the restart-after-replay level-end skip and the judge's step flag. Fixed before M6.
