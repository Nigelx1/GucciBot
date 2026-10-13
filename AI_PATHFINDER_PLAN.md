# The AI Pathfinder: the plan

**Written:** 2026-10-13, for GucciBot 2.0. **Status:** a plan; nothing here is built yet.

**Sources:**
- Nigel's design (2026-10-03 and 10-05): it holds the controller inside the simulator, reads the level as data, is built by us and ships inside the mod with no Claude needed, and the physics map is its teacher.
- Two design drafts from 2026-10-05/06, one search-first and one learning-first, which this replaces.
- Nigel's answers on 2026-10-13.

## The decision (2026-10-13): no machine learning first

Nigel asked whether this could be done without machine learning, or with a free AI program that already knows how to get through a gap. **No such program exists** as far as a search finds. The closest is [DashBot 3.0](https://github.com/MCJack123/DashBot-3.0), which evolves click timings level by level by trial and error and has been archived since 2023. Chat AIs are far too slow for 240 decisions a second, and they guess at physics instead of running it.

**One program already knows GD's physics perfectly: GD.** We already run it as the simulator. What's missing is a smart way to search it, and that doesn't need learning. Chess engines beat grandmasters for decades by searching alone.

So the plan is in two phases:
- **Phase A, exact search, is the plan.** It builds the "physics map" and a "gap finder" that reads the level data.
- **Phase B, a learned network on top, happens only if Phase A turns out too slow on hard levels.** It's kept at the end of Part 2 for that case. It would plug into the same places, so nothing in Phase A is wasted.

**What Phase A settles:** no training data, so whose macros it learns from no longer matters. No trainer, so Python vs C++ no longer matters. No overnight training runs.

Part 1 is for Nigel. Part 2 is for whoever builds it.

---

# Part 1: In plain English

## What we're building

**It holds the controller, inside the simulator.** At every moment it tries the button moves that could matter: press, hold or let go. Each one is played on a copy of the player in the simulator, which runs GD's own player code against the real level. Any path it finds is one GD's physics actually produced. Nothing is drawn as a line for the bot to follow.

**The physics map is the engine.** From any spot, it keeps every way the player can still be alive, tick by tick, merging the ones that are basically the same. You'll see it as dots on screen. If a way through a hard part exists, the physics map finds it. If none exists from that spot, it proves that too, and the pathfinder knows to go further back.

**The gap finder is the knowledge.** It reads the level as data: every block, spike, orb, pad and portal, with their hitboxes and how they move. From that it works out where the openings are ahead, meaning the gaps the player has to fit through. It doesn't steer the player. It tells the physics map which ways to try first, the ones heading for the gaps, so the search finds the way through much sooner. You'll see the gaps it finds drawn over the game.

**Nothing learned, nothing guessed:**
- The simulator decides every death.
- Every idea is also tried a tick late, so it isn't a lucky frame.
- The real game plays every tick for real.

New for 2.0: a finished macro is replayed from a cold start before it counts. With the new parts switched off, you get today's pathfinder exactly.

**It ships inside the mod as plain C++.** No extra files to download, no Python, no internet, no Claude.

## How it plays a level

```
level loads -> read as data (blocks, spikes, orbs, pads, portals, what moves)
   |
   v
at each moment:  the gap finder marks the openings ahead
                 -> the pathfinder tries moves, those heading for the gaps first
                 -> the simulator plays them on a copy of the player
                 -> survivors are checked (does it still work a tick late?)
                 -> the real game plays the next ticks for real
   |
stuck at a death?  the physics map checks each spot further back:
                   "is there any way past from here?"
                   the first spot with a way -> that way is the fix
   |
level complete -> replay the macro from a cold start -> saved only if it completes
```

## What you'll see, step by step

Each step is a build you can try in-game.

| Step | What it adds | What you test |
|---|---|---|
| **M0: measure** | Logs how fast the simulator really is, plus a speed-test button. | Run the pathfinder on Zafari 2 for a minute and press the speed test once in a ship part and once in a cube part. Also try opening GD twice. I read the logs. |
| **M1: the level as data** | An overlay that draws the level the way the pathfinder reads it: boxes coloured by kind, moving ones included. | Open Zafari 2, Xmas Challenge and Bloodbath with Show Hitboxes on. The boxes should sit on the real hitboxes. |
| **M2: the physics map** | The dots: every way to still be alive. When the pathfinder is stuck, the physics map searches. | Bloodbath's Michigun UFO: does it find a way past? Zafari 2 and Xmas Challenge must not get worse. |
| **M3: the gap finder** | The openings ahead drawn over the game. The physics map tries moves toward them first, and the existing height-hold steering aims at them. | Do the drawn gaps match the level? Zafari 2 and Xmas Challenge should solve faster. |
| **M4: smart going-back** | After a death, the physics map checks spots further back until it finds one with a way through. | Bloodbath from the start, plus the two regression levels. |
| **M5: proof runs** | The two "fix later" bugs fixed, the cold replay check, and an overnight test mode that runs the pathfinder through a list of levels and reports what it solved. | Leave GD running overnight once; next day there's a solved/failed list. |
| **M6: breadth** | Fixes for whatever failed in the overnight runs, on RobTop's levels and a test list of online levels. | Overnight runs on the full list until it meets the bar below. |
| **M7: ship it** | Docs and credits updated. | The 2.0 release test. |

## The bar for 2.0 (your answer 2)

You agreed with the three levels, and said that should be the floor, with it already knowing way more. So:
1. **The floor:** from the start of the level, unattended, it solves Zafari 2, Xmas Challenge and Bloodbath. Each solve passes the cold replay.
2. **The real goal:**
   - it solves every official RobTop level that isn't a platformer level, from the start, unattended;
   - it solves at least 16 of a list of 20 online levels covering every game mode, levels it was never tuned on. You pick the list, or approve mine.
3. **It's never slower than today's pathfinder** on the floor levels.

## What I need from you

- **In M0,** try opening GD twice. If two can run at once, the overnight test runs go faster.
- **For M6,** pick or approve the list of 20 online levels. Mix the modes: ship, wave, UFO, ball, robot, spider, swing, plus some dual parts.
- **During M5 and M6,** leave GD running overnight a few times.

Your answer 4 is covered: the two "fix later" bugs get fixed in M5, before the overnight runs.

## What could go wrong

- **The simulator's speed is unknown.** M0 measures it, along with how costly it is to save and restore the simulator's state. The physics map keeps many copies of that state at once, so this sets how wide it can search.
- **The physics map can blow up** on long, open sections where many paths stay alive, like long wave spam. It then has to keep only the best-looking ways by the gap finder's judgement. That can cut a real route, so when a capped search finds nothing, it retries wider before giving up.
- **Bloodbath may not be a search problem.** If the simulator is wrong at the Michigun UFO, no search fixes it. M2 checks that first.
- **If Phase A is too slow on the hardest levels,** that's when Phase B, the learned network, comes in.
- **Mac, iPhone and Android** don't have the trigger model yet, so this starts on Windows. It comes to the others once the trigger model is ported there, which is on your "port everything before 2.0" list.

## How long

I'm not sure until M0. Phase A has no training nights, so it's quicker than the machine-learning plan.
- **M0–M4:** builds of a day or two each, plus your test.
- **M5–M6:** a few overnight test runs and a round of fixes after each.

Expect a few weeks, paced by usage.

---

# Part 2: Technical design

## 1. What it builds on

All in `src/absense/` unless noted. Absense's pathfinder was ported in whole; none of it changed after 2026-10-05.

| Piece | Where | Use |
|---|---|---|
| Exact simulator: copies of the real `PlayerObject` stepped with GD's `update()` + `checkCollisions()`, Silicate's collisions and the World trigger model | `trajectory/trajectory.hpp`: `run()`, `steer()`, `beginSearch()`/`stepSearch()`, `captureStart()`/`useStart()`, `TickInput`, `StartState` | Every move is played here. |
| Legal moves per mode | `trajectory.cpp` `alternativesFor()` | The move set per tick: UFO flap, ship/wave/swing toggle, cube/robot/ball/spider tap/hold/release, orb re-press and spam cadences. |
| Branch and rewind | `SimSnapshot`; the DFS keeps one every `kSnapEvery` = 4 ticks and re-runs the rest | The physics map's frontier states are snapshots (§2, cost). |
| Same input, same result | `pathfinder.hpp` `RunMemo`; `Stats::unrepeatable` | Search results can be trusted, and violations get counted. |
| Decision loop and trust chain | `pathfinder/pathfinder.cpp`: `decide()`, `evaluate()`, `robust()` (1 tick late), trim, tabu, `kDivergence` = 1.0 re-plan, confirmed killers, phantoms, `escalate()`; `kSearchBudget` {40k, 100k, 160k} simulated ticks | Unchanged. New ideas enter as `Candidate`s and face all of it. |
| Height-hold steering (GucciBot's addition) | the Steers stage in `decide()` | The gap finder gives it target heights instead of a fixed spread. |
| Level -> typed boxes | `gpu/client.cpp` `classify()`, `changesEverything()`; `gpu/protocol.hpp` `Object{box, radius, power, kind (18 kinds), flags, vx, vy}` | The level as data, already written. The GPU scorer that used it is off in GucciBot. |
| Real-game judge, sim-vs-real, automation | `judge.*`; `glue.hpp`; MCP `gucci_sim_vs_real`, `gucci_abs_simulate`, `gucci_tick_probe`, `gucci_open_level`, `gucci_restart_game` | Checking the simulator against the game, and running levels unattended. |

## 2. Hard constraints

- **The simulator runs only on GD's main thread, one per GD process.** `Sim::begin` edits the live `m_gameState`. The copies live in the real object layer, and the World's materializer writes run positions into the real objects and collision buckets, then puts them back. No worker threads. Every search runs in deadline-bounded slices (`kFrameBudgetMs`) so the game keeps drawing.
- **Time units.** `PlayerObject::update(dt)` takes sixtieths of a second, so a 240 TPS tick is `0.25`. Anything stepped by hand uses `60 / tps` (CLAUDE.md §4).
- **Determinism.** A run is a pure function of its start and script. Every ordering and tie-break must be deterministic, or tabu and memory break.
- **The World is Windows-only** (commit 2c30247). Elsewhere the copies run without triggers.
- **Snapshot cost is unmeasured, and it decides the physics map's width.** A `SimSnapshot` carries hash maps plus World state. M0 measures save/restore time and bytes, and the frontier cap follows from that. The fallback is the DFS's trick: keep sparse snapshots along each state's history and re-run from the nearest one.

## 3. The level as data (M1)

- **Live view:** during a sim tick, after the World has placed objects where this run has them, collect the objects near the copy from GD's collision sections, the same query the hitbox drawer uses, and `classify()` them. A static export would be wrong for anything a move trigger has shifted.
- **Export:** a Pathfinder-tab button plus an MCP tool `gucci_export_level` write `<save>/levels/<levelKey>.gblevel`. It contains every object's classified box, the triggers by `world::Kind` from the `WorldDef`, the start state (mode, speed, gravity, mini, dual) and the level length, sorted by x. It's for debugging and for the M6 reports. It has to run in-game, because hitbox sizes come from the live objects (`getObjectRect`).
- **Code:** `classify()` and `changesEverything()` move out of `gpu/client.cpp` into a shared `src/absense/levelmap.*` (or `src/ai/level_data.*`).
- **Overlay:** the classified boxes coloured by kind, moving ones included.

## 4. Phase A design

### 4a. The physics map (M2)

- **Breadth-first over ticks, from a start:** a kept `SimStart` at a repair stop, or the current state.
  - Each frontier state expands by its legal moves for the next tick (`alternativesFor`; 2–4 branches) and steps one tick.
  - States that die are dropped. That's exact.
  - Survivors with the same merge key are merged.
- **Merge key:** y and yVelocity quantised (start around 0.05 units and 0.01 per tick, tuned in M2), mode, gravity, size, speed, held, onGround, orb-contact flags. In dual sections, both players' keys together.
  - Merging can rarely hide a distinct survivor. It costs search time, never correctness: the result is still judged exactly.
- **Each state keeps its input history** as a parent pointer plus a move. Any state that reaches the target tick yields a witness script.
- **Frontier cap:** set from M0's snapshot cost. Over the cap, keep the best states by the gap score (§4b).
- **On failure:** if a capped search fails, it retries with a wider cap (×2, up to a limit) before reporting "nothing here". Only an uncapped failure counts as a proof.
- **Output:** the witness script becomes a `Candidate` named "sweep", judged by `evaluate()` / `robust()` like every other idea.
- **Overlay:** alive states per tick as dots (Nigel's physics map). Dot colour = how many merged states it holds.

### 4b. The gap finder (M3)

- **Free space ahead:** for columns ahead of the player, say every 7.5 units in x up to about 2 s of travel, compute the vertical intervals free for the player's hitbox in its current mode and size. That means subtracting solids and hazards (saws by radius) from the live view. Orbs, pads and portals are marked where they sit.
- **Gaps and corridors:** linking free intervals across columns gives corridors. A gap is a corridor's narrowest stretch. Objects flagged by `changesEverything()` mark the column "unknown", so it guides nothing there.
- **Gap score for a simulated state:** progress, plus how well its height and velocity line up with the next gap's opening, minus distance to the nearest hazard edge. Pure geometry, no learning.
- **What uses it:**
  1. **The physics map's order and cap:** best-first instead of plain breadth-first when the frontier is over the cap.
  2. **Height-hold steering:** aims at the centres of upcoming gaps instead of a fixed spread of heights.
  3. **Idea ordering in `decide()`:** the score sits next to `memory::Store::score()`.
- **It never kills a state.** It only orders, caps and suggests; the simulator decides deaths.
- **Overlay:** the free space ahead as bands, gaps highlighted.

### 4c. Smart going-back (M4)

Today a repair probes stops at fixed distances back from the death (1, 2, 3, 4, 6, 8, 12, 16, 24 … ticks).

Instead, for each kept start from the latest backwards, run a budgeted physics map that asks "does any state get past the death tick + clearance?"
- The first stop that answers yes is the repair point, and its witness is the fix.
- Stops that answer no (uncapped) are proven dead and skipped next time.
- `escalate()` stays as the fallback past the kept window (`kMaxWidenSeconds`, 10 s).

### 4d. The confirm run and the two bug fixes (M5)

- **Fix first:**
  - restarting right after a completed replay skips the next attempt's level end;
  - the judge leaves the one-step flag armed (find the re-armer rather than clearing it after the fact).

  Both are in CLAUDE.md §6.
- **Confirm run:**
  1. When the level completes, do a cold full restart.
  2. Play the saved macro through normal playback.
  3. Run Check Macro.

  Only then is the level solved. Absense never had this; Zafari 2's replay was checked by hand, which is how the dropped tap releases (-bq) went unseen.

### 4e. The overnight test harness (M5–M6)

- **An in-mod "test run" mode walks a level list:** open, start from the beginning, pathfind, confirm run, quit, next. Per level it writes solved/failed, time, simulated ticks, and where it stalled.
- **A PowerShell watchdog** relaunches GD after a crash and resumes the list.
- **`gucci_open_level`** opens local created levels only today; extend it to saved online levels and main levels.
- **Testing only.** Nothing is trained. A level whose `Stats::unrepeatable` is non-zero is flagged, because its results can't be trusted.

## 5. Settings and platforms

- **Pathfinder tab switches:** **Physics map**, **Gap finder**, and the overlays (level view, dots, gaps). All on once M4 shows they're no worse; with them off, the pathfinder is today's exactly.
- **Platforms:** Windows first. Elsewhere it ships off by default until the World runs there and someone tests on a device.

## 6. Milestones and exit checks

| # | Delivers | Exit check |
|---|---|---|
| M0 | `stopped:` line logs simulated ticks, sim/restore/real time and ticks/s; `gucci_abs_benchmark` and a speed-test button (µs per tick, µs and bytes per snapshot save/restore); a second-GD-instance check | Real numbers set the physics map's frontier cap and replace every estimate here. |
| M1 | Shared `classify()`, live view, `.gblevel` export, level overlay | Nigel finds no wrong or missing gameplay objects on the 3 test levels. |
| M2 | Physics map as a repair search, dots overlay | Bloodbath either gets past tick ~8676, or there's an uncapped proof that nothing alive in the window does. The second sends the work to the window size or to sim accuracy (`gucci_sim_vs_real` there). No regression on Zafari 2 or Xmas. |
| M3 | Gap finder: free-space overlay, best-first ordering and cap, aimed height-hold, idea ordering | Simulated ticks per decision down on Zafari 2 and Xmas (3 runs each, on vs off); Nigel confirms the drawn gaps match the levels. |
| M4 | Smart going-back | Bloodbath solved from the start; no regression. |
| M5 | The two bug fixes, confirm run, test harness, watchdog | One unattended night completes with a per-level report. |
| M6 | Fixes from the reports, on the official non-platformer levels and the 20-level online list | The bar in §7 is met. |
| M7 | about.md, README, in-game credits and site | Nigel's release test. |

**After M6:** if solve times on the breadth set are too slow, start Phase B (§9). Otherwise, ship without it.

CLAUDE.md rules hold throughout:
- one commit per logical change;
- `GB_BUILD_LABEL` bumped every build;
- no build while GD runs;
- Nigel's in-game test before any release.

## 7. "Done" for 2.0 (Nigel, 2026-10-13: the three levels are the floor)

1. **Floor:** from the start of the level, unattended, it solves Zafari 2, Xmas Challenge and Bloodbath. Every solve passes the confirm run and Check Macro.
2. **Breadth:**
   - every official RobTop level that isn't a platformer level, solved from the start, unattended;
   - at least 16 of a 20-level online list covering every game mode, never tuned on. Nigel picks or approves the list.
3. **Speed:** never slower than today's pathfinder on the floor levels.
4. **Shipping:** plain C++ in the mod; no Python, internet or Claude. With the new parts off, it behaves exactly as today.
5. **Platforms:** Windows supported. Elsewhere it's off by default until tested on a device.
6. **Release:** Nigel's release test passes, and credits match across about.md, README, in-game and the site.

## 8. Risks

1. **Throughput and snapshot cost** (M0 decides): they set how wide the physics map can go.
2. **Frontier blow-up on open sections** (long wave spam, swing): the capped best-first search with widening retries handles it. A cap can still cut the only route, and only an uncapped failure is a proof.
3. **The gap finder can mislead:** a gap that looks open but needs an orb or pad to reach, or a trigger that changes things later. It only orders, so the worst case is slower. `changesEverything()` columns are left unguided.
4. **Bloodbath may not be a search problem:** M2 and `gucci_sim_vs_real` separate a search problem from a sim-accuracy one first.
5. **Dual and two-player sections** multiply the state space: the merge key covers both players. Expect these to be the slowest parts.
6. **Off Windows:** no World, so results there are unverified; off by default.
7. **Open bugs that would fake failures:** the restart-after-replay skip and the judge's step flag, fixed in M5 before any unattended run.

## 9. Phase B (only if Phase A is too slow): a learned guide

Kept from the learning-first draft, for later. It plugs into the same places Phase A uses, so no Phase A code is thrown away.

- **What it learns:** a small network (10k–35k numbers, 40–140 KB) with two outputs:
  - a **policy**, which move to try first;
  - a **"doomed" value**, the chance that no input survives the next 2 s.

  It would replace the gap score in §4b's ordering and §4c's stop choice. The simulator still decides everything.
- **Inputs:** the player vector (`StartState` fields, mode one-hot, held, ticks since the last change, orb contact, dual) plus the nearest 32–48 classified objects, as a set. Sorted, padded, masked, and flipped with gravity.
- **Actions:** (`alternativesFor` move, duration ∈ {1, 2, 3, 4, 6, 8, 11, 16, 24, 32, 48} ticks), scaled by tps/240.
- **Cost:** measured in a 2026-10-05 scratch benchmark (`/O2 /MD`, one thread): 9.2 µs per evaluation with AVX2 and 16.6 µs without for the 10k version.
- **Training data:**
  - physics-map labels, which are exact;
  - the pathfinder's own judged ideas (20k–115k per solve, all discarded today);
  - self-play rounds over a curriculum, holding out the test levels.

  Others' macros only with their permission.
- **Runtime:** `src/ai/features.*` and `src/ai/net.*` in plain C++. `resources/ai/policy.gbnet` carries format and feature versions; a mismatch turns it off. A golden test against the trainer goes in `run_tests.bat`. The trainer (`tools/ai_train/`) never ships.
- **Not used even then:** Absense's approximate GPU physics. It can't model triggers, dual or teleports.
