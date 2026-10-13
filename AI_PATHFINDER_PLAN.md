# The AI Pathfinder: the plan

**Written:** 2026-10-13, for GucciBot 2.0. **Status:** a plan; nothing here is built yet.

**The idea (Nigel, 2026-10-13):** a general AI, with GD's physics as the restrictions.
- A free, open AI that already knows how things like games work chooses the route.
- GD's own physics decides what's possible.

**Sources:**
- Nigel's design (2026-10-03/05): it holds the controller inside the simulator, reads the level as data, is built by us, and ships with no Claude needed.
- Two 2026-10-05 design drafts.
- A read-only research pass on 2026-10-13 over open game-playing, vision and "physical AI" models (workflow wf_09d33fe0-e2a). Every license was checked on the official model card.

Part 1 is for Nigel. Part 2 is for whoever builds it.

---

# Part 1: In plain English

## How it works

**GD's physics is the restriction.** The physics map works out every way the player can physically get through the next stretch, using GD's own player code on copies of the player. The ways that pass the same obstacles the same way are grouped into a handful of routes, like "over the block, then under the spike" or "under the block, then through the top gap". Every route on that list is physically real.

**The general AI picks the route.** It's a free, open vision AI: it looks at pictures and knows what platforms, spikes, gaps and bouncing are, the way a person does.
- It never trained on Geometry Dash.
- It gets a picture of the stretch ahead, drawn from the level's data, with the possible routes drawn on as numbered lines.
- Its job is to pick the route that gets through (or draw one).
- Picking from a few real options is far easier for it than inventing a path from nothing.

**GD's physics does the rest.** The simulator turns the picked route into exact clicks, frame by frame; the real game plays every tick for real; and the finished macro is replayed from a cold start before it counts.

**The AI can be wrong; physics can't.** If its pick dies, the pathfinder takes the next route. A bad pick costs a few seconds, never a wrong macro. With the AI switched off, you get today's pathfinder exactly.

## Which AI (the research, 2026-10-13)

No free AI knows Geometry Dash. The useful kind is a general vision AI that can point and draw on pictures.

| | What | Why |
|---|---|---|
| **Pick: Qwen3-VL 4B** (Alibaba) | A free, open vision AI | Apache-2.0 license, which is clean for GucciBot. It outputs any number of points on a picture. Its makers publish ready-to-run files (about 3.3 GB) that run on your AMD card. Estimated 5–10 seconds per pick on your PC, not measured yet. |
| To compare: **Gemma 4** (Google) | Same kind | Apache-2.0 since April 2026, and runs on your card too. |
| To compare: **RoboBrain 2.5** (BAAI) | Trained to draw a route as points | Apache-2.0. It learned on robot-arm photos, so it may or may not carry over to GD. |
| Later: **Qwen3.5** | Newer and stronger | Apache-2.0. Waiting on the tool that runs it to support pictures. |

**Ruled out:**
- **NitroGen** (NVIDIA's game-playing AI): its license is non-commercial research only. It also needs real gameplay frames and can only press buttons, not draw.
- **Genie 3 and other "world models":** closed, NVIDIA-only, or one game each.
- **Several others, over license traps:**
  - Qwen2.5-VL 3B, Llama 3.2 Vision, Gemma 3 and Moondream 3 carry non-commercial, EU or extra-terms licenses.
  - Molmo 2 can't run on your card yet.

**How it runs:**
- The tool that runs it is **llama.cpp**: free (MIT), works on Windows with your AMD card, and also on Mac, iPhone and Android.
- **The AI file isn't in the mod download** (it's 1–3 GB). GucciBot downloads it the first time you turn the AI on, and checks it the same way the renderer files are checked.
- **No internet or Claude after that.**

## The first test, before building anything in

The open question is how good a general AI is at picking GD routes. Nobody knows, so we test it first:
1. On your PC, run the AI on pictures of 10–20 spots we know: Bloodbath's Michigun UFO, Zafari 2's 14% wall, and others, with the real routes drawn on.
2. Score how often it picks one that gets through.
3. Try the other two AIs the same way.

**If it's good,** we build it in. **If it isn't,** the backup is our own small AI, taught on the physics map's answers (Part 2, §B). It plugs into the same place, so nothing else changes.

This test needs downloads: the llama.cpp tool from its official GitHub, and the AI files from Hugging Face. I'll ask you before downloading anything.

## What you'll see, step by step

Each step is a build you can try in-game.

| Step | What it adds | What you test |
|---|---|---|
| **M0: measure, plus the AI test** | Logs how fast the simulator is, plus a speed-test button. The AI test runs on your PC, outside the mod. | Run the pathfinder on Zafari 2 for a minute and press the speed test. Try opening GD twice. Look at the AI test's results with me. |
| **M1: the level as a picture** | Draws the level the way the AI sees it, as a clean picture: blocks, spikes, orbs, pads, portals. Also an overlay in-game. | Does the picture match the level on Zafari 2, Xmas Challenge and Bloodbath? |
| **M2: the physics map** | The dots (every way to still be alive), grouped into numbered routes. | Bloodbath's Michigun UFO: is there a route past it? Zafari 2 and Xmas must not get worse. |
| **M3: the AI picks** | When there's more than one route, the pathfinder asks the AI and follows its pick first. Pathfinder switch: **AI**. | Zafari 2 and Xmas with the AI on and off. Watch which routes it picks. |
| **M4: smart going-back** | After a death, the physics map checks spots further back for a way through, and the AI picks among them. | Bloodbath from the start. |
| **M5: proof runs** | The two "fix later" bugs fixed, the cold replay check, and an overnight test mode that runs a list of levels. | One night unattended, then a solved/failed list. |
| **M6: ship the AI in the mod** | The AI runs inside GucciBot, downloaded the first time you switch it on, plus fixes from the overnight runs. | Overnight runs on RobTop's levels and your 20 online levels. |
| **M7: release** | Docs and credits. | The 2.0 release test. |

## The bar for 2.0

Agreed 2026-10-13: the three levels are the floor.
1. **The floor:** from the start, unattended, it solves Zafari 2, Xmas Challenge and Bloodbath. Each solve passes the cold replay.
2. **The real goal:**
   - every official RobTop level that isn't a platformer level;
   - at least 16 of 20 online levels covering every game mode, which you pick or approve.
3. **It's never slower than today's pathfinder** on the floor levels.

## What could go wrong

- **The general AI may just not be good at GD routes.** That's what the first test is for, and the backup (our own small AI) is ready in the plan.
- **Speed:**
  - The AI takes seconds per pick, so it's only asked where there's a real choice, not every tick.
  - The simulator's speed is unknown until M0.
- **Your graphics card is shared with GD.** A 4B AI fits alongside it in 8 GB, but we'll watch for that.
- **Bloodbath may not be a search problem.** If the simulator is wrong at the Michigun UFO, no AI fixes that. M2 checks first.
- **Mac, iPhone and Android** need the trigger model ported first, which is on your "port everything" list. The AI tool itself runs on all of them.

## How long

I'm not sure until M0 and the AI test. Rough shape:
- **M0 and the AI test:** a few days.
- **M1–M4:** a day or two each, plus your test.
- **M5–M6:** a few overnight runs and fixes.

Expect a few weeks, paced by usage.

---

# Part 2: Technical design

## 1. What it builds on

All in `src/absense/` unless noted. Absense's pathfinder was ported in whole; none of it changed after 2026-10-05.

| Piece | Where | Use |
|---|---|---|
| Exact simulator: copies of the real `PlayerObject` stepped with GD's `update()` + `checkCollisions()`, Silicate's collisions and the World trigger model | `trajectory/trajectory.hpp`: `run()`, `steer()`, `beginSearch()`/`stepSearch()`, `captureStart()`/`useStart()`, `TickInput`, `StartState` | Every route is played here. |
| Legal moves per mode | `trajectory.cpp` `alternativesFor()` | The physics map's branches. |
| Branch and rewind | `SimSnapshot`; the DFS keeps one every `kSnapEvery` = 4 ticks | The physics map's frontier states (the cost is §2). |
| Same input, same result | `pathfinder.hpp` `RunMemo`; `Stats::unrepeatable` | Results can be trusted, and violations get counted. |
| Decision loop and trust chain | `pathfinder/pathfinder.cpp`: `decide()`, `evaluate()`, `robust()` (1 tick late), trim, tabu, `kDivergence` = 1.0 re-plan, confirmed killers, phantoms, `escalate()`; `kSearchBudget` {40k, 100k, 160k} | Unchanged. AI-picked routes enter as `Candidate`s. |
| Level -> typed boxes | `gpu/client.cpp` `classify()`, `changesEverything()`; `gpu/protocol.hpp` `Object{box, radius, power, kind (18 kinds), flags, vx, vy}` | The level as data, and the source of the AI's picture. |
| Judge, sim-vs-real, automation | `judge.*`; `glue.hpp`; MCP `gucci_sim_vs_real`, `gucci_abs_simulate`, `gucci_tick_probe`, `gucci_open_level`, `gucci_restart_game` | Simulator checks and unattended runs. |
| A local server pattern | Assistant Access (MCP, 127.0.0.1:8790) | Precedent for talking to `llama-server` on 127.0.0.1 during prototyping. |

## 2. Hard constraints

- **The simulator runs only on GD's main thread, one per GD process.** The materializer writes run positions into the real objects and collision buckets. Searches run in deadline-bounded slices.
- **The AI does not run on the main thread.** Its calls are pure: a picture and a prompt go in, JSON comes out. They run on a worker thread or in the `llama-server` process while the pathfinder waits or works elsewhere.
- **Time units:** `PlayerObject::update(dt)` takes sixtieths of a second (a 240 TPS tick = `0.25`, CLAUDE.md §4).
- **Determinism:**
  - The search side: a run is a pure function of its start and script.
  - The AI side: decoding is greedy (temperature 0, fixed seed), and every answer is cached per (level, spot, route set, prompt version, model hash). A rerun therefore makes the same picks, and tabu and memory keep working.
- **The World is Windows-only** (commit 2c30247).
- **Snapshot cost** is unmeasured and sets the physics map's width.
- **VRAM is shared with GD.** Budget for a 2–4B model at Q4 plus its mmproj (about 3–4 GB).

## 3. The level as a picture (M1)

- **Live view:** during a sim tick, after the World has placed objects where this run has them, collect nearby objects from GD's collision sections and `classify()` them (shared `src/ai/level_data.*`).
- **Schematic renderer** (`src/ai/schematic.*`): a fixed-scale image of the stretch ahead. About 2–3 s of travel at roughly 768×512, drawn from level data, not a screenshot, and in plain C++ so it works off-screen and on every platform. It includes:
  - solids in grey, hazards in red (saws as circles), slopes;
  - orbs, pads and portals in their GD colours, plus a legend glyph per kind;
  - the player, with an arrow for velocity;
  - an "unknown" hatch where `changesEverything()` flags something.

  It's flipped when gravity is upside down, so up always means up.
- **Export:** `gucci_export_level` / a Pathfinder-tab button writes `.gblevel` (all classified boxes, triggers by `world::Kind`, start state, length) plus PNG schematics, for the AI test and debugging.

## 4. The physics map and its routes (M2)

- **Breadth-first over ticks** from a start (a kept `SimStart` or the current state).
  - Each state expands by `alternativesFor()`; deaths are dropped (exact).
  - Survivors are merged by a key: y and yVelocity quantised (start around 0.05 units and 0.01 per tick), mode, gravity, size, speed, held, onGround, orb contact, and both players in dual.
  - Each state keeps its input history.
- **Frontier cap** from M0's snapshot cost. Widening retries on failure; only an uncapped failure is a proof.
- **Grouping into routes.** Two surviving histories are the same route if, at every obstacle column they cross, they pass on the same side of it: above or below each solid or hazard, and which orbs and pads they use. That gives a handful of distinct routes, each with a representative witness script and the band of y it occupies over x.
- **Overlays:**
  - the dots, coloured by count;
  - the routes as numbered coloured lines (the same drawing the AI gets).

## 5. The general AI in the loop (M3–M4)

**When it's asked:** only where the physics map finds two or more distinct routes that look different. Typical places:
- forks in the level;
- stuck spots after a death;
- the first stretch after a portal.

Not every tick. The answer is cached (§2).

**What it's given:**
- the schematic of the stretch ahead with the candidate routes drawn as numbered coloured polylines (Set-of-Mark style);
- a short text: the game mode, what each colour means, and "pick the route most likely to get the player through, considering the obstacles after it too".

Two modes, A/B tested in the AI test:
1. **Pick** (default). The output is forced by llama.cpp's JSON-schema constraint: `{"route": <n>, "backup": <n>}`.
2. **Draw.** `{"points": [[x, y], ...]}` on a 0–1000 grid (Qwen3-VL's `point_2d` convention). The physics map then picks the real route closest to the drawing.

**What happens with the answer:** the picked route's witness script becomes a `Candidate` named "ai"; ordering puts it first; `evaluate()` / `robust()` / trim / tabu judge it as usual. If it fails, the backup, then the remaining routes, then today's ideas, then `escalate()`.

**Smart going-back (M4):** after a death, a budgeted physics map per kept start (latest first) asks "is any route past the death tick?". When several routes pass at a stop, the AI picks.

## 6. Runtime and shipping

- **Engine: llama.cpp** (MIT), with vision through libmtmd and an mmproj file. Qwen3-VL has been supported since PR #16780 (Oct 2025), and Gemma 4 since launch. Backends: Vulkan on Windows (the RX 6600 works with just the AMD driver), CPU (AVX2), Metal on macOS and iOS, CPU on Android.
  - **Prototype (M0 test, M3):** the official `llama-server` Windows Vulkan build on 127.0.0.1. It takes OpenAI-style requests with a base64 image and a JSON-schema constraint.
  - **Ship (M6):** link `libllama` + `libmtmd` into the mod (static or shared). It's the only option that works on every platform GucciBot builds for (iOS can't spawn a helper process) and the mod ships no extra executable.
- **Measured speed (llama.cpp Vulkan scoreboard, RX 6600):** Llama-2 7B Q4_0 at about 760–800 tokens/s prompt processing and about 51–53 tokens/s generation. Image-encode time on AMD isn't published; M0 measures it.
- **Estimates:** a 4B VLM at Q4 takes about 5–10 s per call on the RX 6600; CPU is several times slower.
- **Model download on first use:**
  - From a pinned Hugging Face commit, plus a SHA-256 check (the same pattern as the FFmpeg DLLs in guccibot-build-files).
  - Qwen3-VL-4B-Instruct Q4_K_M is about 2.5 GB, plus the mmproj (about 3.3 GB total); a 2B model is about 1.1 GB plus the mmproj.
  - Weights stay outside the .geode, and outside the GPL-3 code: they're data loaded at runtime.
  - The Apache-2.0 NOTICE is shown in Credits and the download dialog.
- **Licenses:**
  - **Ship only Apache-2.0 or MIT models.** That covers Qwen3-VL (every size), Qwen3.5, Gemma 4, RoboBrain 2.5-4B and Embodied-R1.5.
  - **Avoid:**
    - Qwen2.5-VL-3B (Qwen Research License, non-commercial);
    - Gemma 3 / 3n / PaliGemma 2 (Gemma Terms of Use);
    - Llama 3.2 Vision (EU carve-out);
    - Moondream 3 (BUSL);
    - NitroGen and GR00T (NVIDIA non-commercial);
    - Cosmos Reason 2 (NVIDIA Open Model License). It could be an optional separate download with its notices, but it's not the default.
- **Settings:** a Pathfinder-tab **AI** switch with model choice, download size, a progress bar and "remove model". A missing model, a failed hash check or a load error mean the AI is off and the pathfinder runs exactly as today.

## 7. The AI test (M0, offline)

1. Build a set of 10–20 spots with known truth: the M2 physics map on known spots, or for a first look, the existing logs and solved macros. Include Bloodbath's Michigun UFO, Zafari 2's 14% wall, Xmas Challenge sections, and a few cube, ship, wave, ball, UFO and swing forks.
2. For each spot, render the schematic with the real routes drawn, and mark which ones get through.
3. Run Qwen3-VL-4B, Gemma 4 E4B and RoboBrain 2.5-4B through `llama-server` (Vulkan), in both pick and draw modes. Record accuracy, the backup pick's accuracy, and seconds per call.
4. **Go:** clearly above chance (well above 1/n on n-route spots) at under about 10 s per call. **No-go:** fall back to §B.

The downloads need Nigel's OK first: the llama.cpp release zip from GitHub, and the model files from Hugging Face.

## 8. Checking a found path for real

Unchanged:
- The exact sim judges every idea, plus the tick-late check.
- Committed ticks are real.
- A drift over 1.0 unit re-plans.
- Disagreements go to `judge::run`.

**New: an automatic cold confirm run** (full restart, normal playback, then Check Macro) before a level counts as solved. Before it, fix the restart-after-replay level-end skip and the judge's armed step flag (Nigel: yes).

## 9. Milestones and exit checks

| # | Delivers | Exit check |
|---|---|---|
| M0 | Simulator ticks/s and snapshot cost logged; `gucci_abs_benchmark`; second-GD check; **the AI test (§7)** | Real numbers. Go/no-go on the general AI. |
| M1 | Shared `classify()`, live view, schematic renderer, `.gblevel` + PNG export, overlay | Nigel: the pictures match the 3 test levels. |
| M2 | Physics map, route grouping, dots and route overlays | Bloodbath has a route past ~8676, or there's an uncapped proof that none exists in the window. No regression. |
| M3 | AI pick through `llama-server` at forks and stuck spots; cache; **AI** switch | A/B on Zafari 2 and Xmas (3 runs each): no slower; log how often the first pick works. |
| M4 | Smart going-back, with the AI choosing among stop routes | Bloodbath solved from the start. |
| M5 | The two bug fixes, confirm run, overnight test harness and watchdog | One unattended night completes with a per-level report. |
| M6 | llama.cpp linked into the mod; first-use download with hash; settings; fixes from the reports | The bar in Part 1 met on RobTop's levels and the 20-level list. |
| M7 | about.md, README, in-game credits (llama.cpp, the model and its NOTICE) and site | Nigel's release test. |

CLAUDE.md rules hold throughout:
- one commit per logical change;
- `GB_BUILD_LABEL` bumped every build;
- no build while GD runs;
- Nigel's test before any release.

## 10. Risks

1. **General-AI quality on GD schematics is unknown.** The §7 test gates everything; §B is the fallback.
2. **Latency:** seconds per call, so it's asked only at real forks, with answers cached.
3. **VRAM contention with GD** on an 8 GB card: 2–4B models only, and the CPU as a fallback.
4. **Determinism:** greedy decoding, a fixed seed and the cache. A different model or llama.cpp version changes picks, so the cache key includes both.
5. **Build cost of linking llama.cpp** into a Geode mod for 5 platforms (Vulkan on Windows). Prototype with `llama-server` first; link only at M6.
6. **Download size and hosting:** 1–3 GB per model, so it's opt-in, with a pinned commit and hash.
7. **Simulator-side risks are unchanged:** throughput and snapshot cost (M0), frontier blow-up (cap plus widening), Bloodbath possibly being a sim-accuracy problem (M2 checks), no World off Windows.

## B. Fallback: our own small AI (only if the §7 test says no)

It plugs into the same place: it ranks routes and physics-map states instead of the general AI.

- **What it is:** a small network, 10k–35k numbers (40–140 KB), in plain C++. Measured at 9.2 µs per evaluation with AVX2 in a 2026-10-05 scratch benchmark.
  - **Inputs:** the player vector plus the nearest 32–48 classified objects.
  - **Output:** the chance that a state still has a way through 2 s on.
- **How it learns:** taught on the physics map's own exact answers, from overnight runs on a level list. That's GD-generated data only, so nobody's macros. A trainer on Nigel's PC (`tools/ai_train/`) never ships.
- **Rounds:** self-improvement over a curriculum, holding out the test levels.
- **Shipping:** weights in `resources/ai/policy.gbnet` with format and feature versions; a golden test in `run_tests.bat`.
