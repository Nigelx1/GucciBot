# The AI Pathfinder: the plan

**Written:** 2026-10-13, for GucciBot 2.0. **Status:** a plan; nothing here is built yet.

**The idea:**
- **The physics map draws every real path** through the level using GD's own physics.
- **Our own small AI learns which of those paths get through** and paints them, so the pathfinder follows the good ones first.
- **GD's physics is the restriction:** nothing the AI picks can be physically wrong.

**How we got here (2026-10-13):**
1. Nigel wanted an AI that holds the controller inside the simulator, built by us and shipped inside the mod with no Claude needed.
2. He then asked about using a general AI with GD's physics as the restriction.
3. The research found the general AIs that could do it are 2–3.3 GB downloads, take about 5–10 seconds per decision, and have never seen GD (Part 2 §C).
4. Self-trained became the main plan (Nigel: "sure"). The general AI stays as an optional comparison test.

Part 1 is for Nigel. Part 2 is for whoever builds it.

---

# Part 1: In plain English

## How it works

**The physics map draws the paths. No training needed.** From where the player is, it works out every way they can still be alive, tick by tick, using GD's own player code on copies of the player. Paths that pass the obstacles the same way are grouped into routes. You'll see it as dots on screen. This is the first upgrade, and it comes before any AI exists.

**Our AI paints them.** It looks at the level data around each dot: the blocks, spikes, orbs, pads and portals nearby, and how the player is moving. It paints the dot green if a way through continues from there, and red if the player is already doomed. The pathfinder follows the green first, so it finds the way through much sooner and can look further ahead.

**GD's physics does the rest.** The simulator turns the path into exact clicks, the real game plays every tick for real, and the finished macro is replayed from a cold start before it counts.

**The AI can make the pathfinder slower, never wrong.** A bad paint job just means more searching. With the AI off, you get today's pathfinder exactly.

## What "training" means here: mostly your PC working on its own

- **Drawing the paths needs no training.** That's the physics map, straight from GD.
- **The AI only learns which paths are good, and the physics map makes its examples.** Run it on a list of levels overnight, and for every dot GD gives the exact answer: "a way through continued from here" or "doomed". A night makes a huge number of these. They come from GD itself, so nobody else's macros are involved.
- **Teaching:** a small program on your PC shows the AI those examples over and over until its paint matches the answers. Minutes to an hour, with nothing for you to do.
- **Rounds:** one round is a night of GD running plus about an hour of teaching. The first round should already help. A few more rounds, on harder levels, make it better.

Your part is leaving GD running overnight a few times.

## Size and speed

- **The AI is about 40–140 KB,** maybe 1–2 MB if it needs more room. It ships inside the mod, so there's nothing to download.
- **It answers in about 0.00001 seconds per dot,** measured on your PC in an Oct 5 test. That's fast enough to guide every decision, not just a few.
- **The teaching program never ships.** It stays on your PC.

## What you'll see, step by step

Each step is a build you can try in-game.

| Step | What it adds | What you test |
|---|---|---|
| **M0: measure** | Logs how fast the simulator really is, plus a speed-test button. | Run the pathfinder on Zafari 2 for a minute and press the speed test. Try opening GD twice. |
| **M1: the level as data** | An overlay drawing the level the way the AI reads it: boxes by kind, moving ones included. | Do the boxes sit on the real hitboxes in Zafari 2, Xmas Challenge and Bloodbath? |
| **M2: the physics map** | The dots, grouped into routes, as a new search when the pathfinder is stuck. The first upgrade, with no AI yet. | Bloodbath's Michigun UFO: does it find a way past? Zafari 2 and Xmas must not get worse. |
| **M3: proof runs** | The two "fix later" bugs fixed, the cold replay check, and an overnight mode that runs a list of levels. It reports what it solved and saves the physics map's answers as examples. | One night unattended, then a solved/failed list. |
| **M4: the first AI** | Teaching program, AI v1, and the paint overlay: dots green or red by what the AI thinks. | Does green show up where there's really a way through? |
| **M5: the AI guides** | The pathfinder follows the green first; smart going-back after a death. Pathfinder switch: **AI**. | Zafari 2 and Xmas, AI on vs off. Bloodbath from the start. |
| **M6: more rounds** | More overnight rounds on harder levels, plus fixes. | Overnight runs on RobTop's levels and your 20 online levels. |
| **M7: release** | Docs and credits. | The 2.0 release test. |

## The bar for 2.0 (agreed 2026-10-13)

1. **The floor:** from the start, unattended, it solves Zafari 2, Xmas Challenge and Bloodbath. Each solve passes the cold replay.
2. **The real goal:**
   - every official RobTop level that isn't a platformer level;
   - at least 16 of 20 online levels covering every game mode, which you pick or approve. They're never used for teaching, so it's an honest test.
3. **It's never slower than today's pathfinder** on the floor levels.

## What could go wrong

- **The simulator's speed and memory cost are unknown.** They set how wide the physics map can search. M0 measures them.
- **The physics map can blow up** on long open sections, like wave spam. It then keeps only the best-looking dots. Before the AI exists, "best-looking" means furthest along. Afterwards, it means greenest. If that finds nothing, it retries wider.
- **The AI may paint wrong at first,** or only be good at level styles it has seen. Wrong paint only costs time, and the unseen test levels keep us honest.
- **Bloodbath may not be a search problem.** If the simulator is wrong at the Michigun UFO, no AI fixes it. M2 checks first.
- **Mac, iPhone and Android** need the trigger model ported first, which is on your "port everything" list. The AI itself is plain C++ and runs everywhere.

## How long

I'm not sure until M0. Rough shape:
- **M0–M2:** a day or two each, plus your test.
- **M3–M6:** a few overnight runs. Those nights are your PC's time, not yours.

Expect a few weeks, paced by usage.

---

# Part 2: Technical design

## 1. What it builds on

All in `src/absense/` unless noted. Absense's pathfinder was ported in whole; none of it changed after 2026-10-05.

| Piece | Where | Use |
|---|---|---|
| Exact simulator: copies of the real `PlayerObject` stepped with GD's `update()` + `checkCollisions()`, Silicate's collisions and the World trigger model | `trajectory/trajectory.hpp`: `run()`, `steer()`, `beginSearch()`/`stepSearch()`, `captureStart()`/`useStart()`, `TickInput`, `StartState` | Every path is played here. |
| Legal moves per mode | `trajectory.cpp` `alternativesFor()` | The physics map's branches. |
| Branch and rewind | `SimSnapshot`; the DFS keeps one every `kSnapEvery` = 4 ticks | Frontier states (cost: §2). |
| Same input, same result | `pathfinder.hpp` `RunMemo`; `Stats::unrepeatable` | Labels and results can be trusted, and violations get counted. |
| Decision loop and trust chain | `pathfinder/pathfinder.cpp`: `decide()`, `evaluate()`, `robust()` (1 tick late), trim, tabu, `kDivergence` = 1.0 re-plan, confirmed killers, phantoms, `escalate()`; `kSearchBudget` {40k, 100k, 160k} | Unchanged. New paths enter as `Candidate`s. |
| Hand-made priors | `pathfinder/memory.hpp` (`Priors`; "Nothing here learns physics") | The AI takes over their job; they stay as a tie-breaker. |
| Level -> typed boxes | `gpu/client.cpp` `classify()`, `changesEverything()`; `gpu/protocol.hpp` `Object{box, radius, power, kind (18 kinds), flags, vx, vy}` | The AI's inputs. |
| Judge, sim-vs-real, automation | `judge.*`; `glue.hpp`; MCP `gucci_sim_vs_real`, `gucci_abs_simulate`, `gucci_tick_probe`, `gucci_open_level`, `gucci_restart_game` | Simulator checks and unattended runs. |

## 2. Hard constraints

- **The simulator runs only on GD's main thread, one per GD process.** The materializer writes run positions into the real objects and collision buckets. Searches run in deadline-bounded slices. (Network calls are pure and cheap, so they run inline.)
- **Time units:** `PlayerObject::update(dt)` takes sixtieths of a second (a 240 TPS tick = `0.25`; CLAUDE.md §4).
- **Determinism:** a run is a pure function of its start and script. All orderings and tie-breaks are deterministic; the network is too.
- **The World is Windows-only** (commit 2c30247).
- **Snapshot cost is unmeasured** and sets the frontier width. The fallback is sparse snapshots plus re-running, as in the DFS.

## 3. The level as data (M1)

- **Live view:** during a sim tick, after the World has placed objects where this run has them, collect nearby objects from GD's collision sections and `classify()` them (shared `src/ai/level_data.*`).
- **The AI's input (`src/ai/features.*`, the one feature builder used by both the mod and the trainer):**
  - **Player vector (~28 values):** the `StartState` fields, mode one-hot, held, ticks since the last change, orb contact, dual, and TPS normalised to 240.
  - **The nearest 32–48 objects** from about 2 blocks behind to about 2 s ahead, within about ±8 blocks vertically. Each has dx, dy, w, h, radius, a kind one-hot (18 + "changes everything"), power, flags, vx and vy. Sorted, padded and masked, and flipped when gravity is upside down.
  - A feature-version number goes into every example file and every weights file.
- **Overlay:** classified boxes by kind. **Export:** `gucci_export_level` / a button writes `.gblevel`, for debugging.

## 4. The physics map and its routes (M2)

- **Breadth-first over ticks:**
  - each state expands by `alternativesFor()`;
  - deaths are dropped (exact);
  - survivors are merged by a key: y and yVelocity quantised (start around 0.05 units and 0.01 per tick), mode, gravity, size, speed, held, onGround, orb contact, and both players in dual;
  - each state keeps its input history.
- **Frontier cap** from M0's snapshot cost. Over the cap, keep the best states: the furthest along before the AI exists, the greenest after. Widening retries on failure; only an uncapped failure is a proof.
- **Routes:** histories that pass every obstacle column on the same side and use the same orbs and pads form one route, kept with its witness script.
- **Output:** a witness becomes a `Candidate` named "sweep" through the unchanged trust chain.
- **Uses:**
  - as a repair search at stops;
  - "smart going-back" (M5): a budgeted map per kept start, latest first, that asks "any way past the death?".
- **Overlays:** dots coloured by count, plus routes.

## 5. The AI (M4–M5)

- **Network (`src/ai/net.*`, plain C++, no libraries):**
  - a shared per-object MLP and a max-pool (a set encoder), joined with a player MLP, then 128 units, then the heads;
  - **output:** P(a way through continues 2 s from this state);
  - optional second head: expected ticks to death;
  - optional later head: which move first.
- **Size and speed:** 10k–35k parameters, a 40–140 KB file. Measured in the 2026-10-05 scratch benchmark (`/O2 /MD`, one thread): **9.2 µs** per evaluation with AVX2 and 16.6 µs without (10k version); 30 / 78 µs for the 34k version. `net.cpp` builds with `/arch:AVX2`, picked at runtime by CPUID with the plain build as fallback.
- **Uses in the search:**
  1. ordering and capping the physics map's frontier (greenest first);
  2. picking the repair stop (the latest kept start not painted red goes first);
  3. ordering `decide()`'s ideas (next to `memory::Store::score()`);
  4. the paint overlay.

  It never kills a state; the simulator decides deaths.
- **Weights:** `resources/ai/policy.gbnet`, with a header carrying the format version, feature version and layer shapes. A mismatch or a missing file turns the AI off, and the pathfinder behaves exactly as today.

## 6. Training

- **Labels (exact, GD-generated):** in physics-map runs, a state gets 1 if any descendant is alive 2 s (480 ticks at 240 TPS) later, with a witness, and 0 if all descendants die. Merging can rarely hide a survivor; that costs search time, never correctness. Examples go to `<save>/ai_examples/*.gbex` as feature-builder output plus labels (about 250 B each).
- **Data sources:**
  1. physics-map runs in the overnight harness (M3);
  2. the pathfinder's own judged ideas, logged per decision;
  3. later rounds, run with the current AI.

  No one else's macros.
- **Curriculum:**
  1. RobTop's levels;
  2. easy online levels, one mode each;
  3. mixed modes;
  4. demons.

  Also a reverse curriculum from kept starts just before each level's remembered spots.
- **Held out, never taught on:** Zafari 2, Xmas Challenge, Bloodbath, and the 20-level online list.
- **Trainer:** `tools/ai_train/`, in **C++**, built with the repo's existing MSVC toolchain the way `run_tests.bat` builds its tools. That means no Python and no installs. It's hand-written backprop and Adam over the same `features.cpp`, and minutes per epoch on 12 threads for a net this size (estimate). It never ships. A README covers retraining after a GD update without Claude.
- **Golden test** in `run_tests.bat`: the mod's inference must match the trainer's forward pass on a fixed batch to 1e-5.

## 7. Checking a found path for real

Unchanged:
- the exact sim judges every idea, plus the tick-late check;
- committed ticks are real;
- a drift over 1.0 unit re-plans;
- disagreements go to `judge::run`.

**New (M3): an automatic cold confirm run** (full restart, normal playback, then Check Macro) before a level counts as solved. Before it, fix the restart-after-replay level-end skip and the judge's armed step flag (Nigel: yes).

## 8. Milestones and exit checks

| # | Delivers | Exit check |
|---|---|---|
| M0 | Ticks/s and snapshot save/restore cost logged; `gucci_abs_benchmark` and a speed-test button; a second-GD check | Real numbers set the frontier cap. |
| M1 | Shared `classify()`, live view, `features.*`, overlay, `.gblevel` export | Nigel: the boxes match the 3 test levels. |
| M2 | Physics map, routes, repair search, dots and route overlays | Bloodbath has a route past ~8676, or there's an uncapped proof that none exists in the window. No regression. |
| M3 | The two bug fixes, confirm run, overnight harness and watchdog, example writing | One unattended night completes with a report and example files. |
| M4 | Trainer, golden test, `policy.gbnet` v1, paint overlay | On held-out levels, the paint predicts "doomed within 2 s" with AUC ≥ 0.9, measured offline. Nigel: the green looks right. |
| M5 | AI-guided frontier, repair stops and idea ordering; **AI** switch | A/B on Zafari 2 and Xmas (3 runs each): no slower, fewer simulated ticks per decision. Bloodbath solved from the start. |
| M6 | Rounds v2–v3; fixes from the reports | The bar in Part 1 is met. |
| M7 | about.md, README, in-game credits and site | Nigel's release test. |

CLAUDE.md rules hold throughout:
- one commit per logical change;
- `GB_BUILD_LABEL` bumped every build;
- no build while GD runs;
- Nigel's test before any release.

## 9. Risks

1. **Throughput and snapshot cost** (M0): they set the physics map's width and how many examples a night makes.
2. **Frontier blow-up** on open sections: a capped best-first search plus widening. Only an uncapped failure is a proof.
3. **Wrong paint:** it only orders and caps. The worst case is slower, never wrong.
4. **Generalisation:** the curriculum and the held-out sets.
5. **Train/run feature drift:** one C++ feature builder, version numbers, the golden test.
6. **Bloodbath may be a sim-accuracy problem:** M2 plus `gucci_sim_vs_real` decide.
7. **Off Windows:** no World, so it's off by default until ported and tested.
8. **Open bugs that would fake failures:** fixed in M3, before any unattended run.

## C. The general-AI option (research 2026-10-13, kept as an optional comparison)

A read-only research pass (workflow wf_09d33fe0-e2a, licenses read on the official cards) found:
- **No model knows GD.**
- **NitroGen** (NVIDIA's game-playing AI) is non-commercial only, needs real gameplay frames, and outputs button presses, not routes.
- **World models** (Genie 3, DIAMOND, Oasis, V-JEPA 2-AC, Cosmos 3) are closed, CUDA-only, or one game each.
- **Usable:** open vision AIs that point and draw, run through **llama.cpp** (MIT) on the RX 6600 via Vulkan:
  - **Qwen3-VL** (Apache-2.0 at every size; points as JSON on a 0–1000 grid; official GGUF). The 4B is about 3.3 GB with its vision part; the 2B is about 2 GB (estimate).
  - **Gemma 4** (Apache-2.0).
  - **RoboBrain 2.5** (Apache-2.0, trained to draw ordered paths).
- **License traps:**
  - Qwen2.5-VL-3B (non-commercial);
  - Gemma 3 (Terms of Use);
  - Llama 3.2 Vision (EU carve-out);
  - Moondream 3 (BUSL);
  - Cosmos Reason 2 (NVIDIA Open Model License terms);
  - Molmo 2 (no llama.cpp vision yet).
- **Speed:** estimated 5–10 s per decision on the RX 6600.

**How it would plug in:** the physics map's routes are drawn as numbered lines on a schematic of the stretch ahead, and the AI picks one. The pick's witness enters the same trust chain. Weights would be an optional first-use download with a hash check, Apache/MIT models only.

**Optional comparison test:** after M2, run Qwen3-VL-2B/4B through `llama-server` on schematics of known spots and compare its route picks with the trained AI's. The downloads need Nigel's OK: the llama.cpp release from GitHub, and the model files from Hugging Face.
