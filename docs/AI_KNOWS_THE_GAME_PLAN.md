# An AI pathfinder that knows the game: the plan

2026-10-24. Part 1 is for Nigel. Part 2 is the build plan. Nothing in it is built yet, apart from the Remember solutions switch (-ht) that step 0 uses.

## Part 1: For Nigel

**What "knows the game" means.** Today's pathfinder only learns what Geometry Dash is by dying. It tries a fixed menu of click patterns and keeps whatever survives longest over the next 4 seconds. After a death, it nudges the clicks near that spot. That's why every new kind of wall has needed its own patch.

The new one reads the level before it plays, the way you would. It splits the level into parts by what you are in each one: cube, ship, UFO and so on, which way up, what size, what speed. For each part it knows what can't change once you're inside (a UFO can't flip itself) and which earlier choice decided that. Then it plans how to go into each part. GD's own physics still finds every exact click and has the last word, so a wrong idea costs time, never a bad macro. It never uses a macro or anything from earlier runs, and nothing in it is written for one level.

**What it knows.**
- What a click does in each of the 8 modes, and when a click does nothing. What every orb, pad and portal does, what kills you, and what a checkpoint puts back.
- Each of these rules is tested against the game itself. If the game disagrees with a rule, that rule is switched off and named in the log.
- Movement, like how high a cube jumps or how fast a UFO climbs, is measured from the game, not guessed.
- The level as parts, the few ways into each part, and the earlier choices that decide which way in you get.
- Everything it has tried in this run, so it never repeats itself.
- Every spot where its simulation and the game disagree.

**How it plays.**
- Before going into a new part, it uses GD's physics to check that there is a way through from the way in it's about to take.
- When it dies, it explains why in game terms, for example "died in the UFO; went in right side up; the ball's last click decided that". Then it goes back and changes that choice instead of the timing.
- Where the game and its simulation disagree, it stops trusting the simulation there. It tries the clicks on the real game and says what differs.
- Finished runs are still replayed from a cold start, and it still stops after 5 minutes stuck.

**Why this fixes tonight's walls.** On Bloodbath, a ball click sets your gravity just before the UFO portal, about two seconds before the ceiling spike that kills the run. Today's repairs only shift clicks by a few ticks, and they only accept a fix that outlives that spike, so they keep picking right-side-up paths. The only run that got through without a macro went in upside down. The new pathfinder treats "which way up at the portal" as the choice and checks both ways before going in. If the UFO kills it anyway, it goes back to that ball click.

Two more things came out of tonight's logs:
- **It can't fly a UFO.** The part of the pathfinder that searches for exact clicks can't fly a UFO at all. At both Bloodbath's and Clubstep's UFO walls, no click it tried changed anything. Most likely it presses and lets go within the same 1/240 of a second, and a UFO only flaps if the button is still down when the game moves it. I'm not sure yet. One test settles it, and that test is step 1. Catching facts like this is what the rule tests are for.
- **It trusts the simulation over the game.** In the cold runs going right now, The Nightmare stopped at 90%. The game and the simulation disagreed at the same spot 100 times in a row, and the pathfinder believed the simulation every time.

**What it builds on.** These all stay: GD's physics on hidden copies of the player, the trigger model, today's click finder, the level map, and the cold-replay check. The pile of patches (click nudges, escalation levels, rescue searches, "trying anyway") becomes one mechanism.

| Step | What it adds | What you'll see |
|---|---|---|
| 0 | An honest yardstick: the 8 levels with no memory (running tonight) | So far: The Nightmare 90%, Bloodbath 31% (the UFO). Clubstep is at its 70% UFO wall as I write |
| 1 | It knows what a click does in each mode, and the click search can fly | Bloodbath in practice, just before the UFO: "find a way through" draws a way past the spike |
| 2 | It notices when the game and the simulation disagree | The Nightmare's 90% spot named within 3 tries, not 100 |
| 3 | The level brief | An overlay of the parts and the gravity choices, to check against levels you know |
| 4 | It looks before going into a part | Bloodbath from the start: "UFO: upside down gets through", then it gets past the spike |
| 5 | Dead ends go back to their cause and are never repeated | Clubstep's, ToE2's and Bloodbath's walls each passed within 5 minutes |
| 6 | Only clicks that do something | Fewer clicks, same passes |
| 7 | About 10 levels it has never seen | A solved/failed list, each with a reason |
| 8 | All 8 bar levels, unattended | The 2.0 bar |

**Honest risks.**
- Everything after step 1 depends on the click search being able to fly. If that takes more than one fix, steps 4 and 5 slip.
- At some walls, "which way in" may be too rough a choice. It refines itself when that happens, but that costs time.
- LIMBO, ORBIT and Heliopolis need the simulator itself made faster and more exact. This plan stops them spinning and makes them say why they're stuck. It doesn't get them to 100%.
- Timing: steps 0 to 4 take about two weeks. Step 5 lands around the Nov 15 checkpoint, and steps 6 to 8 run close to Nov 26.

**What I need from you.**
1. A yes on this direction. Two bigger alternatives wait until after 2.0: a full rulebook that drives every click, and a trained "instinct". The instinct needs training, which you ruled out as the core.
2. LIMBO, ORBIT and Heliopolis will probably be the holdouts at Nov 15. If you want them at 100% for 2.0, say so now and their simulator work starts alongside. That work is open-ended and would slow the rest a little.
3. Before about Nov 10: around 10 non-platformer levels the pathfinder has never played, covering every mode.

Nothing else changes for now. Per your rule, the new pathfinder stays a switch, with today's engine as the default, until all eight levels pass.

---

## Part 2: Build plan (for the engineer)

Line numbers are at HEAD e3573f9 (build -ht). The designs and judges cited 49eb76d or c98a7dd, and pathfinder.cpp has shifted 10-40 lines since then, so search by function name.

### 2.1 Constraints (Nigel's, all standing)

- **No macros, no memory.** No macro ever steers a search (-hs). Bar runs use Remember solutions off (-ht), so "cold" means no macro and no memory.
- **Training.** No training as the core. An optional trained decider can come later.
- **Speed and stopping.** Slower is fine if the hard levels reach 100%. After 5 minutes stuck (kStuckSeconds), stop and say where.
- **Cold replay.** A result counts only after it replays from a cold start.
- **No level-specific fixes.** Never add a per-level or per-spot rule. A failure on a bar level is fixed as the general weakness it shows (memory feedback_pathfinder_generality).
- **Calm inputs.** A gap between input changes is a preference, never a rule or a label ("nothing should count as spam").
- **Platform and safety.** A Windows-only engine is fine. Automated runs only happen with safe mode on.
- **Default.** The new engine stays a switch, with today's engine as the default, until all 8 bar levels pass.

### 2.2 Where things stand

**Builds that already landed.**
- -hs removed the human-macro idea.
- -ht added Remember solutions (`pf_remember_solutions`, default off, compat/bot.cpp:193).
  - With it off, `start()` never opens `memory::Store` (pathfinder.cpp 489-490), so every `learning` gate is false. That means no remembered ideas, no priors reordering (finishIdeas 2702), no spot fails and no lessons.
  - `learned/` still holds solutions recorded during macro-seeded runs. Back it up and empty it before anyone turns Remember on again.
- Exact start flags is a test switch, off by default (settings_types.hpp:84), using the -hq rule (gdKeepsFlags, trajectory.cpp 1925).
- Route mode is Off by default.

**The cold yardstick, being measured now.** Log `Geode 2026-10-23 23.38.11.log`; settings: -ht, Route Off, Remember off, Exact flags off.

- **The Nightmare: stuck at 90.5%** (x 25740, tick 19821) after 501 s.
  - 213 dead ends at death tick 19832, on object 186 at (25755, 225), each verdict "cannot move".
  - 100 identical partings at tick 19698. Real: y 502.1, vy 5.7, normal gravity. Simulated: y 507.0, vy 13.3, upside down.
  - This has the same shape as the old 75% wall: a gravity object counted as used in one and fresh in the other after a rewind. I'm not sure that's the cause here.
- **Bloodbath: stuck at x 12167, tick 8675, on the UFO wall.**
  - Every normal-engine run without a macro has stopped there: 4 of 4 now.
  - This settles the open -hp question: Exact start flags did not cause the stall, because it stalls the same way with them off.
  - Earlier in the run there were 75 + 52 identical partings at ticks 1119 (x off by 1.81) and 1127 (x off by 4.52). y was equal, and the x gap grows about 0.9 units a tick. That looks like the two disagreeing about a speed portal. Not checked.
- **Clubstep:** running; at its 70% UFO wall (tick ~14600) at 00:02.

**The leg search cannot flap a UFO.** The evidence is strong; the cause is not confirmed.

- Every rescue leg that started inside a UFO used exactly 4n−2 simulated steps over n ticks:
  - Bloodbath -hp, stops 8321-8675 (8470: 114 ticks, 454 steps; 8602: 74 ticks, 294 steps);
  - Bloodbath -ht: 624 legs;
  - Clubstep -ht, stops 14315-14612 (14464: 161 ticks, 642 steps).
- 4n−2 means one state on the first tick, then exactly two live states a tick (held and released), each with two moves. No input ever produced a third distinct state.
- Bloodbath's legs from ball stops (7638-8238) all emptied at tick 8359, about 31 ticks after the UFO portal (around tick 8328).
- The code path:
  - stepLeg's moves are carry-on `{0, down}` plus alternativesFor's moves (trajectory.cpp 4679-4691).
  - For the UFO, alternativesFor offers only `{1,false}` (3405-3409), plus `{2,true}` while on a ring.
  - Sim::step's `apply()` (2769-2794) plays `{1,false}` as pushButton, then releaseButton, both before the physics update.
  - If GD flaps the UFO only when the button is still down at the update, a same-tick tap does nothing. The merge key then folds every tapped child into the carry-on child.
- The ideas that do fly the UFO keep the button down through the tick: "orb spam" (`{2,true}` then `{0,false}`), "hold", and steer's toggle `{1,true}`.
- The branching search has the same `{1,false}` UFO alternative (3604), so in a UFO it explores flap timings only on rings.
- Two things can't be settled from the logs alone:
  - The alternative explanation (every tapped child dies on the spot) can't be ruled out.
  - Whether the real game flaps on a same-tick tap is unknown.

  Step 1's first test answers both.
- **Checked and ruled out:**
  - The frontier cap: these legs peaked at 2 states, far below 128.
  - `m_touchingRings`: SavedPlayerCheckpoint restores it (core/checkpoint_player.cpp 188-190 and 449-450).

**Other code facts confirmed at HEAD.**
- **Movement limits** are typed in: planner.cpp envelope(), lines 29-43.
- **Gravity on the route:**
  - Rise and fall swap only in cube and robot columns (asym, 246-281).
  - Gravity changes only through options (gravityAfter, atlas.cpp 832).
  - So ball, spider and swing flips are invisible to the route.
- **The atlas's "already moved" test** reads sprite positions (atlas.cpp 336-337; also 318-319, 355 and 382). GD's enter effects slide sprites.
- **The divergence check is muted after ahead commits.**
  - decide() sets `m_lastReplanTick = tick` at 4658. For an ahead decision, that tick is the plan's future end.
  - afterTick only reports a parting once `now >= m_lastReplanTick + gap` (5428).
- **materialize.cpp startPending** (1391-1399) takes the full write-back whenever `m_fy > 0`. ORBIT and Heliopolis log a y section factor of 0.01.
- **gucci_abs_simulate and gucci_sim_vs_real can't express a same-tick tap.** In their script language (parseAbsScript, mcp_tools.cpp 136-156), `p` keeps the button down through the tick.
- **judge::run** plays from wherever the real game is. Nothing in the pathfinder calls it yet; only gucci_sim_vs_real does (compat/bot.cpp 862).

### 2.3 What gets built

The core is Design 2 (Level Brief + Choice Backtracking), as both judges recommended. Pieces taken from the other two designs:

**From Design 1:**
- rule checks, adapted to run from real states instead of constructed copies;
- measured movement;
- gravity in every mode;
- the atlas fixes: read `m_positionX/Y`, and follow teleport portals to their exit;
- the whole-level situation plan with a reason for each step, used to order probes and shown on screen;
- provenance of gravity, mode and size;
- splitting a ledger bucket when exact answers disagree inside it;
- never going back past the furthest good path;
- the batch of levels it has never seen;
- one narrow interface header.

**From Design 3:**
- leg diagnostics: peak frontier, the capped flag, cap and exact comparisons, and a protected lineage;
- TraceSample gains mode, mini and speed;
- the overnight harness.

**Deferred until after 2.0:**
- Design 1's fourth setting, where the model drives every step;
- Design 3's trained net and its hooks H1-H6;
- heavy-level World work, unless Nigel asks for it (side track B).

**Two changes to the judges' order,** because of evidence that came in after they wrote:
1. The leg fix becomes "inputs, measured", the rulebook's first family, because the leading suspect is input semantics, not ranking.
2. Trust moves up to step 2, because tonight's cold runs show repeated identical partings as the first wall on two levels.

### 2.4 Architecture

```
level opens
  route/atlas.cpp (fixed positions, teleports) --> route/brief.cpp: sections, locks, setters,
                                                   entry classes, dynamics, killer movers
  model/rules.cpp + calibrate.cpp ------------> input table, object rules, measured movement (checked)
  model/situate.cpp --------------------------> per boundary: entry classes in order, with a reason

decide()  --[model/engine.hpp: Contract stage]--> today's ideas / steers / search / legs (fixed menus)
                                                  --> verdict: better(), robust(), trim, commit
dead end  --[choice repair]--> explain (choices.cpp) --> ledger (model/ledger.cpp) --> attempts in cause order
                                --> probing from kept starts, or anchors + sliced re-step --> restore
afterTick --[model/trust.cpp]--> partings, unseen deaths, unrepeatable answers --> ask the game (judge::run)
```

Principles:
- **Physics decides.** The model picks targets, orders the work and assigns blame. The trust chain is unchanged: evaluate, robust, real-tick commit, divergence re-plan, cold replay. So a wrong model costs time, never a wrong macro.
- **A switch.** `pf_engine = today | understanding`. On the Pathfinder page it reads "Engine: Today / Understanding (test)", and gucci_pathfinder takes `engine`. Understanding ignores the route switch and logs that it does.
- **Shared-code fixes land for both engines.** These are step 1's input menus, step 2's divergence fix and the Exact start flags default. Each one is gated by a cold regression batch on Clubstep, ToE2 and Deadlocked.
- **One interface.** pathfinder.cpp includes only `model/engine.hpp`.
  - Every contract or repair decision logs one line starting `model:`, so a path the model never reached shows up as a missing line. This is the half-port check.
- **Determinism.** Budgets are counted in simulated steps, orderings are stable, and the clock is only a safety stop.
- **Handoffs.** These go to today's engine, are counted, and are logged ("model: today's engine decides this one: two players"):
  - two-player decisions;
  - platformer levels;
  - a player going left;
  - dual stretches.

### 2.5 New components and data structures

**1. model/rules.hpp/.cpp (~700 lines)**
- **The input table.** Keyed by (mode, mini, gravity, where ∈ {ground, air, on orb}).
  - It records which acts change the next 8 ticks compared with doing nothing.
  - It is measured lazily: the first time a decision starts from a real kept start in a combination not yet measured, it runs 6 copies of 8 ticks from that start.
  - It is cached per launch and TPS, and logged once, e.g. "model: UFO in the air: a tap inside the tick does nothing; a one-tick press flaps".
- **`menuFor(p, down, onRing)`** replaces alternativesFor's hand-written menu.
  - It offers every act that does something and nothing that doesn't.
  - For a combination not yet measured, it falls back to today's menu.
- **Object rules.** One row per `gpu::ObjectKind` (with power and flags).
  - Rows are written from the binary transcriptions in physics/collisions.cpp and physics/player.cpp, and each row cites its source.
  - They are checked from exact traces: when a copy fires an object (noteRingUsed 823, and the portal and pad hooks), the state before and after is compared with the row.
  - A row that disagrees is switched off for the launch and logged, and nothing downstream relies on it.
- **Death and checkpoint rows:** hazard, the side of a solid, the void (inVoid 770), and the flag rule (gdKeepsFlags 1925).

**2. model/calibrate.cpp (~400)**
- Per-tick vertical response per (mode, mini, speed, gravity, TPS), taken from the same 8-tick runs.
- A 1-D integrator turns these into rise and fall per 30-unit column. This replaces envelope() and feeds "Show reach".
- **Check:** one exact free-air run per mode against the integrator, within 1 unit. If it fails, the envelope stays generous and the log says so. I'm not sure every mode passes this check.

**3. route/brief.hpp/.cpp (~900)**
- Built whenever atlasFor builds the atlas.
- **Sections** are cut at:
  - mode, size, speed, dual, solo and teleport portals;
  - the player-state triggers 2066, 2900, 3022 and 1932.
- **Locked state:**
  - Gravity is locked in cube, robot, ship, UFO and wave sections, except across a gravity object inside the section. It is free in ball, spider and swing.
  - Mini, mode and speed are locked except across their own portals.
- **Setters** come from a backward pass. For gravity:
  - the previous ball, spider or swing section's flip region (its last surface contacts before the boundary);
  - any gravity portal, orb or pad in between;
  - further back, transitively, if the previous section is locked too.
- **Entry classes** are gravity (only where a setter can change it) × band × rising or falling. The band is the openings at the section's first tight point or fork, or else thirds of the room.
- **Dynamics:** each section is labeled Static, Modeled or Unknown, naming the trigger kinds the World doesn't run (world/def.cpp kindShipped, world/certainty.cpp).
- **Killer movers:** gates whose choice targets a group that holds a killer inside the section (WorldDef group targeting).
- **Room:** the atlas estimate, replaced by the exact flying bounds the first time a copy crosses the portal.

**4. model/situate.cpp (~700, beside route/planner.cpp)**
- The layered best-K search over situations, with:
  - measured reach;
  - gravity for every mode, with ball, spider and swing flips as transitions;
  - a vertical label;
  - ledger facts as removed or priced edges.
- **Output:** for each boundary, the entry classes in preferred order, each with a reason, e.g. "flip in the ball before x 11715: the UFO is planned upside down". "Show the planned route" draws this.

**5. pathfinder/choices.hpp/.cpp (~450)**
- **ChoiceTrace** of the committed path, written in afterTick: the section entered and its class, the orbs, pads and portals fired, and which side of each fork. It is cut back on every rewind, the way dropLessonsAfter is.
- **`entryClassOf(trace, boundaryX)`** reads an idea's class at a boundary.
- **`explain(dead end)`** returns the section, the killer's uid and certainty, the class entered, the setters, the choices inside the section and the killer movers. It returns these as text and as the ordered list of repair attempts.

**6. model/ledger.hpp/.cpp (~600), plus tools/ledger_tests.cpp in run_tests.bat**
- **Rungs** follow ROUTE_ENGINE_PLAN step 6, scaled by TPS/240:
  - 128 states, 30k steps;
  - 256 states, 100k steps;
  - 512 states, 300k steps, half the calm gap;
  - 1024 states, 1M steps, no calm gap;
  - then an exact search in ground modes.
- **Hard facts** come only from exact, uncapped, full-move searches whose deaths are Static (or Modeled with `world::drifts()` unchanged), confirmed by a straight re-run.
- **Real deaths** never become hard facts.
- **Splitting:** when exact answers disagree inside a bucket, it splits by the sign of vertical speed, then a finer y, then held.
- **MCP:** gucci_route_ledger.

**7. model/trust.hpp/.cpp (~450)**
- **Detection.** A parting key is (tick, dx and dy to 0.1). Three identical partings mark the ±32×scale ticks around them as an untrusted span.
- **What differs.** Real against the plan trace:
  - the gravity, mode, size and speed flags;
  - the used flags of portals, orbs and pads within two columns, read through `m_positionX` and compared with the copy's fired set.
- **Inside an untrusted span:**
  - the simulation's verdicts make no facts and no "cannot move" dead ends;
  - nothing is decided ahead, so the game waits at the decision;
  - the top 3 ideas that cross the span are played on the real game with judge::run before one is committed, ranked by the real outcome.
- **Also here:**
  - the divergence fix;
  - never going back past the furthest good path;
  - stop lines that name the span.

**8. model/engine.hpp/.cpp (~800)**
- The interface, the Contract stage, the choice-repair driver, the handoffs and the `model:` lines.

**9. Changes to trajectory/ (~400)**
- **LegGoal** gains:
  - `wantGravity`, `wantMode` and `wantMini`, checked at the crossing next to fireObject and avoidObject (stepLeg 4702-4706);
  - `lineage`, a protected path that is never merged or dropped.
- **Live** gains `spamPeriod`, `spamPhase` and `releaseNext`, so stepLeg honors `Alternative::thenRelease` and `spamPeriod`. `ringHidden` comes from the previous tick's press.
- **keyOf (4478)** gains:
  - the buffered-press flags (`m_jumpBuffered`, `m_stateRingJump`; check the Geode names);
  - a hash of the rings used since the leg began (check that SimSnapshot carries `m_ringUsed`).
- **TraceSample** gains mode, mini, speed, and the uid fired on that tick.

**10. UI and MCP (~400)**
- The Engine switch.
- "Show the level brief".
- "Find a way through the next section" (practice).
- New MCP tools: gucci_level_brief, gucci_route_ledger, gucci_model_rules, gucci_model_probe.
- gucci_abs_simulate and gucci_sim_vs_real gain a `t` step (a tap inside the tick).
- gucci_route_leg gains lineage and class options.
- Rescue and drive leg log lines gain peak states, "dropped for room" and the relaxed count. compat/bot.cpp 805 already returns the first two to the MCP.

In all, about 6k new lines and about 1k changed.

```cpp
// model/rules.hpp
enum class Act : uint8_t { TapInTick, PressOneTick, Hold, Release, Repress, SpamUnit };
enum class Where : uint8_t { Ground, Air, OnOrb };
struct InputRule { uint8_t mode; bool mini, upsideDown; Where where; uint8_t acts; bool measured; };
struct ObjectRule { int kind; bool onPress, multi; int8_t gravitySet; bool gravityFlip;
                    int8_t mode, mini; float speed, impulse; const char* source; bool checked, agrees; };
// route/brief.hpp
struct EntryClass { int8_t gravity /* -1: not choosable */; uint8_t band; int8_t vertical; uint8_t split; };
struct Setter { enum Kind : uint8_t { Flips, GravityObject, ModePortal, SizePortal, SpeedPortal } kind;
                float x0, x1; int uid; };
struct Section { float x0, x1; uint8_t mode; bool mini; float speed; bool dual;
                 float roomLo, roomHi; bool roomExact; uint8_t locked;  // Gravity|Mini|Mode|Speed
                 std::vector<Setter> setters; std::vector<int> gates; std::vector<EntryClass> classes;
                 enum class Dyn : uint8_t { Static, Modeled, Unknown } dyn;
                 std::vector<int> unfollowedKinds, killerMoverGates; };
// pathfinder/choices.hpp
struct ChoiceEvent { uint64_t tick; float x; uint8_t kind; int section, uid; EntryClass cls; };
struct Explanation { int section, killerUid; uint8_t certainty; EntryClass entered;
                     std::vector<Setter> setters; std::vector<ChoiceEvent> inside; std::string text; };
// model/ledger.hpp
struct Bucket { int16_t yBin15; int8_t vSign; uint8_t mode, gravity, speedBin; bool mini, held; uint8_t split; };
struct LedgerKey { uint8_t kind; int section; route::GateKey gate; int option; EntryClass cls; Bucket b; };
struct Fact { enum : uint8_t { Unknown, Found, Soft, Hard, Untrusted } state; int rung, tries;
              uint64_t fromTick; std::vector<TickInput> way; };
struct Rung { int cap, steps240, calm; bool exact; };
// model/trust.hpp
struct Parting { uint64_t tick; int16_t dx10, dy10; int count; std::string what; };
struct Span { uint64_t from, to; int partings, unseenDeaths, unrepeatable; bool untrusted; };
```

### 2.6 Existing code: reused, changed, replaced

| Existing | Role in the new engine |
|---|---|
| Trajectory::run, steer (3161), beginSearch/stepSearch, advanceStart (5094), captureStart (1357), useStart (1970) / useStartExact (1929), inVoid, phantoms, confirmed killers | Reused unchanged: the executor and the physics |
| alternativesFor (3393) | Takes its menu from the measured input table (both engines) |
| beginLeg / stepLeg / finishLeg (4546-4881) | Reused for probes and contract legs. Gets the menu fix, spam units, class goals, lineage and key bits |
| decide() (3028), better() (3379), evaluate (2498), robust (2457), trimStep (1868) | Reused. The Contract stage sits after the judging loop and before the verdict. Probes ride the decision's existing leg slot (the Route and rescue plumbing, ~3356-3504) |
| beginDriveLeg / takeDriveLeg / drivePasses (2868-2969) | Their plumbing becomes the contract legs |
| commitTick (5261), afterTick (5283), keepStart (851), keepAnchor (775), rewindTo (4830), finishRestore (5964), saveFurthest (710), settleAnticheatSpike (5937), AheadWait at mode portals | Reused. afterTick feeds the ChoiceTrace and the trust record. Anchors are also kept at committed boundaries. Probes run where the game already waits at portals |
| beginRepair (5474), beginBacktrack (5607), escalate (5003), moveStartBack (5072), rescueWanted / beginRescueLeg / takeRescueLeg (2747-2853) | Replaced by the choice repair when Engine = Understanding. They stay in today's engine |
| atlas.cpp build | Positions read through m_positionX/Y; teleports followed to their exit |
| planner.cpp envelope (29-43), stepCost (66), noteLeg's +5000 | Measured movement, gravity in every mode, the ledger |
| materialize.cpp startPending (1391-1399) | Side track A |
| judge::run | "Ask the game" in untrusted spans |
| memory.cpp | Untouched; off for the bar |
| GPU stage (dead: gpu=false) | Untouched. Optional cleanup commit later |

### 2.7 Order of work

Dependencies: step 0, then steps 1, 2 and 3 in parallel worktrees, then 4, 5, 6, 7 and 8 in order. Side track A can happen at any time.

Rough calendar: step 0 by about Oct 25, steps 1-3 by about Nov 3, step 4 by about Nov 7, step 5 by about Nov 15-18, step 6 two days after that, and steps 7-8 run over nights to about Nov 24-26. If step 1's cause turns out to be a stepping bug rather than the input menu, add up to a week.

**Step 0: The honest yardstick (no engine change)**
- **Build:**
  - Let tonight's cold batch finish: 8 levels, Route Off, Remember off, flags off.
  - On the same build with Exact start flags on, run The Nightmare and Bloodbath, then Clubstep, ToE2 and Deadlocked as a regression.
  - If a night is free, run the 8 cold on Route drives, as the comparison for steps 4-5.
  - Code: peak states, "dropped for room" and the relaxed count in the rescue and drive leg lines (takeRescueLeg 2817-2848, takeDriveLeg).
- **Test** (Claude, unattended over Assistant Access when the PC is free; the scripts refuse without safe mode):
  - a table per level: %, time, clicks a second, stop reason, cold replay;
  - whether the partings at 19698 and 1119/1127 disappear with flags on.
- **Decision:** if flags on clears those partings and costs nothing elsewhere, it becomes the default. It is GD's own rule (-hq).
- **Retires:** nothing.

**Step 1: Inputs, measured; legs that can fly (gates steps 4-5)**
- **First, the test that names the cause:**
  - Add the `t` step to the script language.
  - In practice inside a UFO, from the live state, run gucci_abs_simulate with `t1 r20`, then `p1 r20`, then `r21`.
  - Run gucci_sim_vs_real with the same three scripts.
- **Reading the result:**

  | What happens with `t1` | Meaning | Fix |
  |---|---|---|
  | Neither the copy nor the game flaps | The menus are wrong | The measured table |
  | The game flaps, the copy doesn't | The copy's input path differs from GD's queued path (GJBaseGameLayer::handleButton) | The copy's input path. Check Silicate's version first (CLAUDE.md §5) |
  | Both flap | Something else causes the die-off | Find it with the lineage parity line |

- **Build:**
  - The input table and menuFor; alternativesFor uses them (both engines).
  - stepLeg honors thenRelease, spamPeriod and ringHidden.
  - The keyOf additions.
  - LegGoal.lineage and a parity line: "leg parity: the played path dies at tick N in the leg but lives in a straight run". That line means a stepping or restore bug.
  - "Tap at k" ideas become one-tick presses wherever a tap inside the tick does nothing.
- **Test:**
  - Nigel or Claude, in Bloodbath practice: checkpoint in the ball section around x 11300-11600, then press "Find a way through the next section" (or gucci_route_leg to x 12425, any height, cap 128 then 1024).
    - Pass: a way past x 12300 is drawn, or "out of budget" names the class.
    - Two presses give the same answer, and the game keeps drawing.
  - The same at Clubstep's ship-to-UFO portal (~68%).
  - Log: no UFO leg with 4n−2 steps, and peak states above 2.
  - Regression, cold and unattended: Clubstep, ToE2 and Deadlocked complete, and their cold replays complete.
  - If Clubstep's 70% wall now passes on the first visit, credit the general menu fix, not a level patch.
- **Retires:** nothing. A rescue leg's "nothing alive gets there" only counts when the parity line is clean.

**Step 2: Trust (parallel with step 3)**
- **Build:**
  - model/trust as described in 2.5.
  - **Divergence fix:** m_lastReplanTick only rate-limits rethinks after real decisions. After an ahead commit, every committed tick is compared with the plan trace (afterTick 5419-5431).
  - **Never lose the best path:** an attempt that dies before the dead end it was repairing restores the saved furthest path (saveFurthest) before the next attempt.
  - Exact flags become the default if step 0 agreed.
  - **Risky part:** judge::run inside a decision. Its `active()` checks in engine_updater.cpp 843-863 must not collide with the pathfinder's own stepping. Review this part, in parallel with Nigel's test.
- **Test:**
  - The Nightmare cold with Exact flags off on purpose, which reproduces tick 19698.
    - Pass: within 3 identical partings, the log reads "model: the simulation disagrees with the game at x … (tick 19698): …", naming the object or flag. The run then asks the game and gets through, or stops with that line, instead of repeating 100 times.
    - With flags on, it completes cold and the cold replay completes.
  - Bloodbath's 1119/1127 partings are named the same way.
  - LIMBO for 15 minutes:
    - it never falls more than one section behind its best tick (tonight it went from 7777 back to 5060);
    - the stop line lists the untrusted spans.
- **Retires:** in the Understanding engine, "trying anyway" inside untrusted spans; and re-running an identical rescue leg (memoized by start tick, goal and budget).

**Step 3: The level brief (read-only; parallel with steps 1-2)**
- **Build:**
  - **Position reads:** use m_positionX/Y at atlas.cpp 318-319, 336-337, 355 and 382, in noteKiller's x/y (796-809), and in gucci_used_objects.
  - Teleport portals are followed to their linked exit.
  - Object rules and their checks.
  - calibrate.cpp, replacing envelope().
  - Gravity for every mode in stepCost.
  - route/brief, the "Show the level brief" overlay, gucci_level_brief, and the TraceSample additions.
- **Test:**
  - Nigel, with the overlay on, at Bloodbath 25-35% (the ball, then the UFO), The Nightmare 70-76% and ToE2 80-90%: do the parts and the gravity marks match the level?
  - Claude, brief lines such as:
    - "UFO x 11715-12210: gravity locked; decided by the ball's flips x 10995-11715", or the line names a gravity object inside;
    - "Clubstep UFO ~70%: entered from a ship, gravity locked".
  - Claude: ToE2 and The Nightmare maps built at tick 1 and mid-level give the same moving-object and gate counts (ToE2 gave 2020 against 0).
  - Claude: "model: N rules checked, 0 disagree", or each disagreement named.
  - Regression: Engine Today plays as before, since the brief is read-only.
- **Retires:** the typed envelope.

**Step 4: Look before going into a part (needs steps 1 and 3)**
- **Build: the Contract stage.**
  1. Group the judged ideas by the class each crosses the next boundary in.
  2. Look each class up in the ledger.
  3. Probe classes that are still unknown, in slices: advanceStart to the crossing, then a leg through the section to its end (or to the known dead spot plus clearance) at rung 0.
  4. Contract legs ask for an untried class (gravity and band at the boundary).
  5. In the verdict, rank class first (Found, then Unknown, then Soft, then Hard), then better().
  6. Two-leg rule per section: a plan never crosses into a class without a Found way through. While a probe is still running, it commits up to the boundary minus the margin.
  7. situate.cpp orders the classes and supplies the reasons.
- **Test:**
  - Claude, unattended: Bloodbath cold, Engine Understanding.
    - Pass: the log has a line like "model: UFO x 11715: right side up - no way through (rung 0); upside down - a way through; going in upside down", and the run passes tick 8675 with no dead end there.
  - Regression: Clubstep, ToE2 and Deadlocked complete on Understanding with cold replays, each in no more than 1.5× its step 0 time.
  - Nigel: "Show the planned route" on Bloodbath and ToE2. Does the planned way into each part look like how he'd play it?
- **Retires** (Understanding engine):
  - Route ideas' legs and rescue legs before boundaries; contract legs replace them.
  - Route drives' per-gate legs. A gate becomes a target only when an explanation names it.

**Step 5: Dead ends go back to the cause and never repeat (needs step 4)**
- **Build:**
  - choices.*, the ledger with rungs and splitting, and offline tests.
  - With Understanding on, beginChoiceRepair/nextAttempt replace beginRepair, beginBacktrack and escalate. Attempts, in order:
    1. Timing inside the section: today's stops and ideas, bounded by the section's start and its last choice point, plus a contract leg to past the death.
    2. The newest untried option at a choice point inside the section.
    3. Another entry class at its setter: a backjump into the setter's kept starts, contract legs that demand the class at the boundary, then a section probe.
    4. The previous section, the same way.
    5. The next rung, starting again from attempt 1.
  - After the last rung: a report, then today's repair until the 5-minute stop.
  - Class tabu.
  - Backjumps past the ~10 s of kept starts go through anchors and the sliced re-step.
  - A backjump that finds nothing returns to the best path.
  - The stop line lists what the ledger ruled out.
- **Test:**
  - Bloodbath cold with the step 4 contract switched off, so the UFO has to be repaired.
    - Pass: the log explains the death ("died in the UFO, entered right side up, decided by the ball's flips at x …"), lists the attempts in order, and goes back to the ball.
    - gucci_route_ledger shows no (key, rung) tried twice.
  - Clubstep 70%, ToE2 85% and Bloodbath's UFO: each passed within 5 minutes in at least 2 of 3 cold runs.
  - run_tests.bat passes the ledger suite.
- **Retires:** see 2.8.

**Step 6: Only clicks that do something (needs step 1; best after step 5)**
- **Build:**
  - Open-loop "orb spam at k" and "orb spam ×N" (buildCandidates 1314-1366) become spam while on a multi-activate orb, at a cadence of 2, 3 or 4.
  - Steers and idea shapes never press where the input table says nothing happens.
  - The trim stays.
- **Test:** Claude, cold.
  - Clubstep and ToE2 still pass on Understanding, with cold replays.
  - A checker over each saved result counts presses on ticks where the table says nothing happens. Pass: 0.
  - Clicks a second and the busiest second are reported against step 5. No cap, because nothing counts as spam.
- **Retires:** the open-loop spam shapes. The spam trim pass becomes a no-op on Understanding results but stays as a net.

**Step 7: Levels it has never seen**
- **Build:** tools/selfplay, grown from the scratchpad batch_runs.py and pf_timed.py:
  - a level list;
  - safe mode required, Remember off;
  - a watchdog through gucci_restart_game;
  - a cold replay of every result;
  - a morning report.
- **Test:**
  - About 10 levels Nigel picks, covering all 8 modes, mini, dual, every speed, gravity portals and orbs. They run cold, overnight, on both engines.
  - Count the 100%s that pass cold replay, and read every stop reason.
  - A failure becomes a rule or brief fix only if it is a general GD fact.

**Step 8: The bar**
- All 8 levels on Understanding, unattended and cold, with every 100% replayed cold.
- Then Nigel decides the default. Only after that are retired patches deleted from today's engine, one commit each.

**Side track A** (any time, its own commit): a 2-D pending pass when `m_fy > 0`.
- Test on ORBIT and Heliopolis:
  - the "World upkeep" pending time drops from 44.9 s and 105.5 s;
  - simulated ticks a second rise;
  - the % reached is no worse.
- Expect a modest gain: pending passes were about 6% of ORBIT's deciding time and 14% of Heliopolis's.

**Side track B** (only if Nigel asks): heavy-level simulator work. Not estimated.
- start tables after rewinds (world/ledger.cpp coveredNow and lastUncovered);
- the trigger kinds LIMBO, ORBIT and Heliopolis use that the World doesn't run;
- flying bounds that follow the camera.

### 2.8 Retirement map (Understanding engine; deleted from code only after the default flips)

| Today's patch | Becomes | From step |
|---|---|---|
| "played, input N moved ±1/2/4" | Timing ideas, only at stops inside the dead end's section | 5 |
| Rescue legs (-hh/-hi) | Contract legs: to past the death inside the section, or to the boundary in a named class | 4-5 |
| Escalation levels; widening after 60 real deaths | Budget rungs per ledger entry | 5 |
| Floor "trying anyway" | "Ask the game" in untrusted spans; nothing elsewhere | 2 (untrusted), 5 (all) |
| Traps, retries, 1/3 and 2/3 stop shifts | Ledger entries plus the step (a) stop list | 5 |
| moveStartBack | A backjump to a setter through stored frames or anchors | 5 |
| R5 (jump back to the decision that left the route) | The general backjump to the setter, at any distance | 5 |
| Route ideas, Route drives | The Contract stage (sections and classes, not gates) | 4-5 |
| noteLeg's +5000 | Ledger facts | 5 |
| Typed envelope | Measured movement | 3 |
| Open-loop orb spam shapes | Spam while on a multi-activate orb | 6 |
| Divergence check muted after ahead commits | Fixed, both engines | 2 |
| Exact start flags as an off-by-default test switch | On by default, if step 0 agrees; the switch stays for tests | 0 |

These stay as they are:
- the exact-prefix tabu, with the class tabu added on top;
- forcedCommit and playing into a death, inside a section;
- AheadWait, which now hosts probes;
- the void rule;
- phantoms and confirmed killers, which now feed the trust record;
- the trim passes;
- memory, off for the bar. After 2.0 it could store the classes that worked, never inputs.

### 2.9 Working rules

- **Build and commit (CLAUDE.md §0):**
  - one commit per logical change;
  - `.\build_win.bat`, and look for "Build complete";
  - bump GB_BUILD_LABEL on every build, editing it by the line that ends in a backslash;
  - push to master and watch CI.
- **Running the game:**
  - Nothing in-game while Nigel uses the PC.
  - Batches run only with safe mode on (the scripts check gucci_get_state) and Remember solutions off.
- **Results:** a result counts only after a cold replay from a fresh open.
- **Half-port checklist for steps 4-5.** Each of these paths must log its `model:` line, and each step's test log is grepped for all of them:
  - live, ahead and probing decisions;
  - the two-player handoff;
  - the slack check (robust);
  - the trim;
  - the run memo key;
  - the commit length;
  - the drive paths.
- **Offline tests:** the pure parts (the brief from synthetic atlases, the ledger, the cause order) go in run_tests.bat.
- **Parallel worktrees.** Steps 1-3 touch mostly separate files:
  - step 1: trajectory/ and model/rules;
  - step 2: pathfinder.cpp afterTick and model/trust;
  - step 3: route/, model/brief and calibrate.
- **Reviews:** only for the risky parts (step 2's real-game asking, step 5's backjumps), run in parallel with Nigel's test and never gating it.
- **Docs:**
  - Tick docs/ROAD_TO_2_0.md step 4 as steps land.
  - AI_PATHFINDER_PLAN.md still says "nothing here is built yet", and ROUTE_ENGINE_PLAN.md's line numbers are stale. Point both at this plan.

### 2.10 Risks and open questions

- **The UFO hypothesis could be wrong.** Then the die-off is a stepping bug, for example state the snapshot doesn't carry across interleaved restores, such as the materializer's writes. The lineage parity line finds it, and fixing it is simulator work of unknown size.
- **Entry classes could be too coarse.** Splitting fixes that, at a cost in time.
- **Proofs will be rare in flying modes,** so most facts will be soft and progress comes from rungs. Switching back and forth between classes will need hysteresis, which is untested.
- **Probes cost time.** Expect seconds per boundary on normal levels and minutes on ORBIT, with more waiting at portals.
- **judge::run costs real ticks.** It is slow on heavy levels, and it must leave the game exactly as it was (it records the frame and x before and after).
- **Long backjumps** past about 10 s go through anchors, which are megabytes each, 8 to 64 of them kept.
- **Setters can be wrong** where toggles, items or counters couple choices that are far apart. The fallback is stepping back choice by choice through the ChoiceTrace, newest first.
- **Realtime timing is nondeterministic:** whether a decision is made ahead or from the real state depends on timing.
- **Exact flags as the default** could change levels that pass today. Step 0's A/B covers this.
- **Not sure:**
  - whether Bloodbath's UFO is purely an entry-gravity problem (the evidence is strong but indirect; step 1's practice test and step 4 will tell);
  - what causes ToE2's 85% wall;
  - what causes Bloodbath's parting at 1119/1127.

### 2.11 Not in this plan

- **Design 1's "model drives every step"** setting. It waits until legs pass step 1 and handoffs fall far below Route drives' 2015 of about 2240 decisions on cold ToE2.
- **Design 3's trained net,** its hooks H1-H6 and the self-play corpus: after 2.0. Its training-data log could arrive earlier as an optional switch.
- **Heavy-level World work** (side track B).
- **Platformer, sub-tick (CBF) inputs, and coins.** Coins means each coin becomes a gate, after step 5.