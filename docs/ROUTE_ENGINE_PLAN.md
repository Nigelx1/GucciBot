# Route drives: the build plan

**Written:** 2026-10-22, from a read-only workflow: two diagnoses (ToE2's orb, Deadlocked's movers), three independent designs (A: the plan; B: whole-level search; C: smallest change) and a judge who checked their claims against the code. Line numbers are as of master 3881613 (2.0.0-beta.5). This is the plan for AI_PATHFINDER_PLAN.md's real R4 onward; the 10-21 baseline it starts from is in docs/ROAD_TO_2_0.md.

## The choice

Synthesis, "Route drives": Design A's route driver logic (explicit gate decisions including orb use/skip, the two-leg rule, a ledger of soft and hard dead ends, backjumps to the responsible gate, budgets counted in simulated steps), built in Design C's order inside decide()'s existing stages and plumbing, with Design B's richer merge key and per-class frontier quotas.

Design A is the plan Nigel agreed to (AI_PATHFINDER_PLAN.md:26-30, 105-106): the AI makes explicit decisions across the whole level, physics finds each leg's clicks, dead ends get crossed off, and the run jumps back to the cause.

It is also the only design whose dead-end handling gets back to a ToE2-type cause quickly. The blue orbs sit about 10 gates before the diamond deaths, so neither a one-gate tie-break nor C's gate-by-gate chronological backjump reaches them fast. A's re-plan diff with a gravity/mode/size shortcut goes there first.

A also fixes determinism properly, with budgets counted in simulated steps. Today two things are nondeterministic:
- sweepLeg stops on the wall clock (trajectory.cpp:3836).
- The random ideas are seeded from random_device (pathfinder.cpp:134).

A's weakness is its step 4. In one build it lands PLAN/CHEAP/LEG1/LEG2/VERIFY/COMMIT/AHEAD, gate anchors and the handoff, and it moves decide()'s commit block and the restore code into new shared functions in a 4,606-line file. That is the half-port pattern this project keeps hitting.

Design C gets the same behavior in small, testable builds. It keeps the trust chain that already works in game:
- evaluate (1816-1932), robust (1775-1814) and the trim (1594-1736);
- the ahead, live and probe paths (3447-3572, 4354-4379).

It changes only three things:
- what a decision aims at;
- what counts as passing (crossing the next gate in the chosen option, instead of better()'s "survives 4 s, then fewest clicks", 2295-2316);
- how far it commits (routeCap generalized, 3296-3300).

C's weak points are its chronological backjump and its millisecond budgets. A's logic replaces both.

Design B has the most thorough look-ahead, but I did not choose it:
- It replaces the agreed whole-level route with a heuristic field built from the rough per-mode envelopes (planner.cpp:28-42).
- It needs a snapshot for every node on every tick over a 4-20 s lead; its own estimate is 150-420 MB. Snapshot cost has never been measured: no "[AbsPF] speed test" line exists in any log, though bot.cpp:360 writes one.
- It is the biggest new engine.
- LIMBO's cost per tick may rule it out.

From B, the plan takes the merge key and the class quotas. Without them a capped frontier can merge or cull "orb fired" and "orb skipped" states (today's key, trajectory.cpp:3763-3778, has no ring or fired bits).

Hard constraints:
- No freeze: every heavy piece is sliced. The synchronous 200 ms leg inside each decision (pathfinder.cpp:2207-2260) is removed early.
- Determinism: step budgets and a seeded RNG.
- No spam: a minimum gap between input changes, an exact dominance merge, and calmest-passer ranking.

Today's engine stays available as Off and Route ideas, and as the fallback for any stretch the route engine hands back.

### Taken from the other designs

- A: budgets counted in simulated steps, never milliseconds, for anything that changes a choice. A wall-clock limit may only stop the run and report.
- A: the two-leg rule. Leg 1 is committed only when the exit has a way through the following gate. Out of budget does not block; not found does.
- A: a feasibility ledger keyed by (gate, option, entry bucket) with budget rungs, soft fails and hard nogoods. A soft result comes back only on a higher rung.
- A: backjump to the responsible gate at any distance. First try the last gate whose other option changes gravity, mode or size, then the newest gate with an untried option. Inside the kept starts this is looked at in the simulation; further back it goes through anchors and the sliced restep, with anchors kept at committed gates.
- A: a minimum gap between input changes, an exact dominance merge (keep the state that is free to switch soonest), a relaxation ladder, and clicks per second in the stop line.
- A: FREE and OBSERVED gate options, so a wrong map costs time, not the route.
- A: three real disagreements at one gate hand that stretch to today's engine, with a 'the simulation disagrees with the game at x' report.
- B: a richer merge key: ring used/touched in the current contact, the goal object's fired bit, speed, and a World toggle/item hash.
- B: per-class quotas in a capped frontier (gravity, mode, size, goal fired), so both orb choices survive the cap.
- B: trust by certainty. Static deaths count toward proofs. Modelled deaths count only while world::drifts() is unchanged and a straight re-run agrees. Uncertain deaths never count.
- B: a look-only tool. 'Look ahead' and an MCP call plan and search the next few decisions from a paused spot without playing them.
- C: the build order. Leg bar and commit-to-gate go inside decide()'s existing stages, so the live, decided-ahead and probe paths share one code path.
- C: a three-way setting: Off / Route ideas (today) / Route drives.
- C: GateWatch in Trajectory::run, legPass, and the verdict order: passers first, then fewest fast changes, then fewest changes, then most room.
- C: a backward cost-to-go pass, so each decision ranks the next gate's options from the player's real y. Gates that share a column are merged instead of dropped.
- C: a refutation valve (a filler that passes a hard-dead option erases it), widening when the map is wrong, m_rng seeded per decision, and gucci_route_ledger.
- Diagnoses: a fix for restores re-arming GD's anticheat spike, a bound on R5, gates at narrow waists (ToE2 hypothesis 2), and columns with touch-triggered movers kept 'moving' (Deadlocked hypothesis 6).

## What the diagnoses found

**Fixes to do first (step 1):** restores losing the slope launch; a bound on R5; determinism; logging the leg's error.

**Deadlocked**

- **H1 holds (likely).**
  - On the first pass the ball left the ramp at f=5250 with ret=0, postCalled=1 and ys 6.266 (log 15082-15084).
  - After the rewind to 5248 (log 15087), the re-stepped 5250 returned 1 and postCollision never ran (log 15088).
  - ToE2 shows the same thing 1-3 ticks after restores: at 11081 (log 10688 vs 10692) and at 12283 (log 12614 vs 12617).
  - Under Absense the spike reaches GD: hook_playlayer.cpp:660 and 691 don't divert it, and 776 hands it to GD. The copies skip it (collisions.cpp:410). Memory measured ret=1/postCalled=0 as the spike's signature.
  - Not proven for checkpoint loads. Step 1's log line settles it.
- **H2: the R5 half holds.**
  - beginBacktrack's loop (pathfinder.cpp:4215-4227) has no floor.
  - forceRoute takes any route idea that reaches its gate (2814-2831, routeReached 2319-2327).
  - Jumps in the log: 3137 ticks on Deadlocked (log 15113); 3096 and 3753 on ToE2 (log 12931, 13253).
  - The "Modelled counts as cannot move" half is real in code (1888-1889 and 1906-1907 feed 3098-3119), but no such verdict is shown wrong in the log. Left alone in the old engine; Route drives never uses that verdict.
- **H3 is plausible, with no cheap fix.** Only the removed height steers got past the crushers at about tick 4407. That becomes step 6's test.
- **H4 holds as a measured simulator problem.** The World's answers at the group-9 crushers differ by path: unrepeatable steers at log 15287-15291, flagged by evaluate at 1925-1930. Measure in step 2; no blind fix.
- **H5 holds; cause unknown.** Slope exits drift even without a restore: ToE2 was 26 units off four ticks after a clean exit (log 10688-10689). Measure in step 2.
- **H6 holds; step 7.** The ghost never presses (trajectory.cpp:4106), and timed columns lose their "moving" flag (atlas.cpp:315-319).

**ToE2**

- **H1: the mechanics are verified.**
  - A steer that lasts the horizon sends the decision straight to the verdict (2331-2335, 2406-2409). That happened at the orb: "tick 16026: steer 32 (lasts >=960), committing 640 [30 of 403]" (log 12765).
  - better() then prefers fewer clicks (2313-2314).
  - Realtime commits 2H/3 = 640 ticks (3268-3273).
  - Single-change ideas stop at 240 ticks (983).
  - steer() only switches when its input dies inside its look (2703-2747).
  - Not proven: that pressing the orb is what opens the diamond.
- **H2 fits equally.** The big ship can't be flown through the diamond. The gucci_route_leg check in step 2 decides between H1 and H2.
- **H3 holds structurally.**
  - Orb gate options are just the column's openings (atlas.cpp:339, 347).
  - The planner has no gravity (planner.cpp:28-42).
  - A leg ends on the first band crossing (trajectory.cpp:3835, 3888-3893).
  - A second gate in the same column is dropped (planner.cpp:206).
- **H4 is rejected for now.** The game died one tick after the predicted 17127, with no divergence line (log 12926-12928).
- **No patch to the old engine.** The diagnosis's fixes (a)-(e) are what steps 3-6 build.

**Also: Clubstep's "6057 could not run" is not a slot conflict.** Design A's worry about that (log 8581) doesn't hold.
- It equals exactly the 6057 decisions made after tick 20758, past the last gate ("no gate ahead", trajectory.cpp:3794-3799).
- Those decisions came during a level-end loop: "continue lasts 1, committing 1", then "committing 120" up to tick 73105.
- That loop adds about 23 s to the 231.9 s baseline.
- It is likely the known level-end bug, and not an engine blocker.

## Steps

### 1. Cheap fixes from the diagnoses (one build, one commit each, GB_BUILD_LABEL bumped)

**Files:** src/hooks/hook_playlayer.cpp, src/hooks/hook_gjbasegamelayer.cpp, src/absense/pathfinder/pathfinder.cpp, src/absense/trajectory/trajectory.hpp/.cpp

(a) Restores must not change the real game's physics.
- Log a line when GD's destroyPlayer gets the anticheat spike while Absense drives. Today it reaches GD at hook_playlayer.cpp:776, because 660 and 691 only divert it for onWouldDie and the classic Pathfinder.
- In the checkCollisions hook (hook_gjbasegamelayer.cpp:348-378), log every real-player pass that returns 1 without a death, not only slope exits.
- Fix: at the start of finishRestore (pathfinder.cpp:4490-4492), before the re-step loop (4506-4528), hand the spike to GD's own destroyPlayer once, so re-stepped ticks run like a cold replay.
- If the log shows it is still hit, keep it out of the real player's pass instead (m_isDisabled around the call at hook_gjbasegamelayer.cpp:362), from the first pathfinder restore until the next full restart. That matches the copies (collisions.cpp:410).
- Not sure which of the two GD accepts. One logged run settles it.

(b) Bound R5.
- In beginBacktrack's loop (4215-4227), skip route choices older than m_repair.floor.
- In the forced verdict (2816), take a route idea only if it also clears d.need.

(c) Determinism.
- Seed m_rng per decision from the level, the tick and the visit count, instead of random_device (134; used at 1182-1191).
- Give LegGoal a step budget and drop the wall-clock break (trajectory.cpp:3836) for decisions: 2216 passes about 20k steps instead of 200 ms.

(d) Log why a route search could not run (2221).

**Test:** Nigel runs ToE2 and Deadlocked from the start with 'Follow the planned route (AI)' on, like the 10-21 baseline. Claude reads the log for three things:
- After each 'respawn@' line, no slope exit shows 'ret=1 ... postCalled=0' (baseline: log 15088, 10692, 12617), and the new line says whether the spike was hit.
- No 'route: back to tick' lands further back than that dead end's 'no further than tick'.
- ToE2 still reaches 78% or better. Deadlocked passes 22%, or stops at a new place.

### 2. Measure before building (R0): speed, memory, and three simulator checks (no new code)

**Files:** None. Runs on the step-1 build; Claude reads the logs.

Speed test > Measure (shell.cpp:1231-1247, logged at bot.cpp:360) gives three numbers that have never been logged: ticks a second, snapshot save/restore cost, and snapshot size. They set every leg budget and the memory cap.

With Assistant Access on, a session runs three checks.
(1) ToE2's orb, the diagnosis's live test: gucci_route_leg from about frame 16030 to the normal-gravity platform band, and from about 17010 to the diamond's exit.
- 'Found with a press at x 21840-21906' plus 'not found from 17010' means the orb choice is the cause (hypothesis 1).
- 'Found from 17010' means the ship can't be flown through the diamond (hypothesis 2).
(2) gucci_sim_vs_real across a slope exit at ToE2 ~11075-11090. The real path was 26 units off there after a clean exit (log 10688-10689).
(3) Deadlocked's group-9 crushers at about tick 4490, using Nigel's loaded macro and gucci_run_to: live-import run vs carried start vs the game (log 15285-15295).

Also note why the World switches off on ToE2 ('eventStamps size 8 vs 10', log 8614) and on Bloodbath.

**Test:** Nigel opens Clubstep, ToE2 near 75%, Deadlocked near 20%, LIMBO and Bloodbath, and presses Speed test > Measure once in each. No run needed. Then he turns on Assistant Access in ToE2 so a session can run the orb check and the two comparisons. Claude reports which ToE2 hypothesis holds, and the numbers.

### 3. A sliced, repeatable, calm leg search (the physics map with a goal)

**Files:** src/absense/trajectory/trajectory.hpp/.cpp; src/absense/pathfinder/pathfinder.cpp (route stage); src/absense/compat/bot.cpp, src/mcp/mcp_tools.cpp, src/ui/shell.cpp (Find a way)

Structure:
- Turn sweepLeg's loop (trajectory.cpp:3835-3918) into LegState with beginLeg/stepLeg/finishLeg.
- Pause and resume it like stepSearch (3120-3166) and GhostState::pause (3974-3980). Keep m_killer and m_teleportRand as sweepLeg does (3711-3712, 3946-3947).
- The budget is counted in simulated steps only.

Goal: a list of waypoints {x, band (lo > hi: any height), an object that must or must not fire, expected gravity}. A state that passes a waypoint the wrong way is dropped.

Merging:
- Key: today's key (3763-3778), plus ring used/touched in the current contact (trajectory.hpp:419-420), the goal object's fired bit (copyFired, trajectory.hpp:862-866), speed, and a World toggle/item hash.
- Dominance merge: keep the state that is free to switch soonest, then the one with fewer input changes. Today the first one found wins (3878).

Calm inputs:
- An input change is offered only G ticks after the last one. G is Nigel's number, by default 33 ms worth of ticks.
- Multi-activate orb spam is exempt (2542-2551).
- G halves, then drops to 1, only on later rungs.

Capped frontier:
- Keep a quota per class (gravity, mode, size, goal fired) first.
- Then rank by: can still reach the band, fewest fast changes, fewest changes, distance to the band, index. Sort by index, never the snapshots themselves (Android rule, 3898-3901).

Results:
- Crossings are collected for 4 ticks after the first. Up to 8 exits in distinct buckets, each with its witness.
- 'Impossible' only for exact keys, an uncapped frontier, all six whole-tick inputs, and Static deaths.

sweepLeg stays as a wrapper. decide()'s route idea runs as a sliced stage instead of the synchronous call (2207-2260). That removes a stall of up to 200 ms at every decision.

**Test:** In ToE2 practice a little before 75%, press 'Find a way' twice:
- The game keeps drawing.
- Both presses give the same answer.
- The drawn path looks like a person's: long diagonals, no buzzing.

Then run ToE2 with 'Follow the planned route' on:
- No stutter at each decision.
- The HUD's CPS readout never shows the 120-a-second straight-line wave.

### 4. Decisions the map can name: orb use/skip, pads and portals, and a route planned from where the player is

**Files:** src/absense/route/atlas.hpp/.cpp, src/absense/route/planner.hpp/.cpp, src/mcp/mcp_tools.cpp (gucci_route_atlas, gucci_route_plan)

Gate options become waypoints. Today an orb gate's options are just the column's openings (atlas.cpp:339, 347).
- Orbs: USE (band = the contact region, must fire; gravity flips for gravity orbs) or SKIP (must not fire).
- Pads and gravity/size/mode portals: TOUCH or AVOID, wherever an opening reaches past the object. ToE2's one 84% pass avoided the cube portal at x 20385.
- Toggle orbs and touch triggers: FIRE or SKIP.
- Every gate also gets a FREE option, tried last.

Gate changes:
- Gates that share a column are merged instead of dropped (planner.cpp:206).
- Add a gate wherever an opening narrows under about 3 player heights. The Tight rule fires only under 2 halves (atlas.cpp:355-363), so ToE2's 79-unit diamond waists are not gates today.
- Stable gate keys: (column, kind, object uid).

Planning:
- Add a backward cost-to-go pass over the layers, reusing stepCost (63-121), so each decision ranks the next gate's options from the player's own y.
- Today decisions follow a route planned once from x 0 (guidePlan, 303), while the drawn route starts at the player (shownPlan, 266-268).
- Ties: USE before SKIP, then the lower band.

The overlay marks each option.

**Test:** 'Show the planned route' on ToE2 at about 75%:
- The route starts at the player.
- The blue orbs at x ~21825, 21873, 22125 and 22185 carry USE/SKIP marks.

On Deadlocked, the route runs through the gaps the map timed. On Clubstep, the route looks as before.

### 5. Route drives: each decision is one leg to the next gate, committed only to that gate

**Files:** src/absense/pathfinder/pathfinder.hpp/.cpp; new src/absense/route/driver.hpp/.cpp; src/absense/trajectory/trajectory.cpp (GateWatch in run, 3234-3282); src/absense/compat/settings_types.hpp; src/ui/shell.cpp; src/mcp/mcp_tools.cpp

The switch becomes Off / Route ideas (today) / Route drives.

The target and the bar:
- A decision's target is the next gate. Its option is the best by cost-to-go.
- GateWatch in Trajectory::run records, for every idea judged: the crossing tick, y, and whether the object fired.
- legPass: crosses inside the band, required object fired, forbidden object not fired, and alive for `clear` ticks past (clear as at pathfinder.cpp:1974).

What gets tried, in order:
- Fillers first, judged over leg 1 + leg 2 instead of 4 s: continue (the last leg-2 way), steers 2-64, the human macro, remembered ways.
- Then the sliced leg search.
- Two-leg rule: commit only if the exit has a way through the following gate. That can be its own continuation, another passer's, or a capped leg-2 search from the exit.

The verdict:
- Passers first, then fewest fast changes, then fewest changes, then most room. This replaces better() (2295-2316) in this mode.
- robust() runs over the leg (1775-1814).
- The trim keeps only variants that still pass (1617, 1687).
- The memo salt includes the gate (1746-1762).

Committing:
- Commit exactly to the gate crossing (routeCap generalized, 3296-3300).
- The next decision is made ahead from the crossing state (3447-3495), or from the real state after a mode portal (3480).
- If no option of this gate passes, that one decision is made by today's verdict and logged as a handoff. Step 6 replaces this.
- R5 and the old verdict branches (2838-3135) don't run in this mode.

Visible output:
- One log line per decision, e.g. 'route: gate x 21873 (blue orb) USE: leg by steer 32, next gate ok; playing 64 ticks'.
- A 'Look ahead' button and an MCP call plan the next few decisions from a paused spot without playing them.
- The stop line shows clicks per second: the average and the busiest second.

**Test:** Clubstep from the start with Route drives:
- It must finish.
- Compare the time with about 209 s. The 231.9 s in log 8579 includes about 23 s of a level-end loop.
- The CPS readout stays human.

ToE2 and Deadlocked from the start:
- The log shows one decision per gate with its option, and where it handed over.
- Note how far each gets, against 78% and 22%.

At ToE2 around 75%, press Look ahead and note which orb option it picks.

### 6. Dead ends: crossed off, never retried, and the jump back to the gate that caused them

**Files:** new src/absense/route/ledger.hpp/.cpp (plus an offline test run by run_tests.bat); src/absense/pathfinder/pathfinder.cpp (beginBacktrack, afterTick, finishRestore); src/mcp/mcp_tools.cpp (gucci_route_ledger)

The ledger stores a result per (gate key, option, entry bucket). The entry bucket is y in steps of 15, the sign of vertical speed, mode, gravity, size, speed and held. Each result is Found, Soft (with its rung and fail count) or Hard (an exact proof).

Budget rungs are in simulated steps, set from step 2. A's starting points:
- 128 states, 30k steps, gap G;
- 256 states, 100k steps, gap G;
- 512 states, 300k steps, gap G/2;
- 1024 states, 1M steps, no gap;
- then a proof search in ground modes.

On a failure:
1. Next rung.
2. Then the next option at the same gate, on the same tick.
3. Then a backjump: first the last committed gate whose other option changes gravity, mode or size; else the newest committed gate with an untried option or exit.

How far back:
- Inside the kept starts (2464 ticks, 649), it is looked at in the simulation first (the probe path, 4354-4379).
- Further back, it goes through an anchor plus the sliced restep (4414-4474, 4506-4528), with anchors kept at committed gates (3985-3988).

What counts as hard:
- Only exact, uncapped, full-move searches whose deaths are Static, or Modelled with world::drifts() unchanged and a straight re-run that agrees (1925-1930).
- A filler passing a hard-dead option erases it.
- Real deaths are never nogoods. Three at one gate hand that stretch to today's engine with 'the simulation disagrees with the game at x'.

Limits count steps and backjumps, not seconds. R5 (4211-4228) is retired in this mode.

**Test:** ToE2 from the start, unattended:
- The log shows 'crossed off' and 'back to gate x N (S s back)' lines.
- The same gate/option/entry is never tried twice.
- It gets past 80%, or stops naming where the simulation disagrees.

Deadlocked from the start: past the crushers at about x 6342 without the removed height steers, and past 22%.

### 7. Blocks the player's own actions move: keep the map honest

**Files:** src/absense/route/atlas.cpp, src/absense/trajectory/trajectory.cpp (ghost run), src/absense/route/ledger.cpp

Columns whose movers come from touch triggers, toggle orbs or count/collision triggers stay 'moving' even when the ghost timed them. Today atlas.cpp:315-319 clears that flag, and the ghost never presses (trajectory.cpp:4106).

Where legs see live states cross a gate's x outside every band, those heights become an OBSERVED option for that entry.

Later: a ghost run for each FIRE option, with the press forced at the trigger.

**Test:** Deadlocked's ball section (x 7700-7950), with 'Show the level map' on:
- The monster's box sits where it really is when the ball takes the orb.
- The route stops aiming at the platform it rises from.

LIMBO from the start: note how far it gets.

### 8. Remember, then run the bar

**Files:** src/absense/pathfinder/memory.hpp/.cpp; the overnight level list and cold replay check (plan R6)

memory::Store keeps the chosen options and hard nogoods per level. They are keyed by object uid, because today's ring sets are keyed by pointer (trajectory.hpp:419-420).

Each finished macro is replayed from a cold start before it counts.

**Test:** ToE2 twice: the second run is faster, and the log shows remembered choices.

Then the 8 bar levels unattended (Clubstep, ToE2, Deadlocked, The Nightmare, Bloodbath, LIMBO, Orbit, Heliopolis). Each finished macro is replayed cold.

## Risks

- The simulator, not the planner, may be the limit. Slope exits drift even without a restore (ToE2: 26 units, log 10688-10689; Deadlocked: 1.05 at 5251, log 15085). Deadlocked's group-9 crushers give different answers depending on how the run got there (log 15285-15295). The World is off on ToE2 (log 8613-8614) and on Bloodbath. Route drives then stops and names the spot; fixing it is simulator work, found in step 2.
- Not sure how GD handles the anticheat spike on a checkpoint load. If neither handing it to GD nor hiding it works cleanly, it needs a disassembly look like the one for -au (memory: project_congregation_slope_bug). Hiding it means GD's anti-noclip check never fires again in that attempt.
- Proofs will be rare in ship, wave, UFO and swing legs, because states almost never merge exactly. Most of what it learns will be soft, so Bloodbath's UFO may end in a handoff rather than a solve.
- Snapshot cost and size are unmeasured, so every budget waits for step 2. Anchors are full checkpoints, megabytes each, capped at 64 (pathfinder.cpp:583-586).
- Half-port risk: the leg bar has to reach the live, decided-ahead and probe paths, the slack check, the trim, the memo key and the commit. If any one is missed, the route silently stops driving.
- The two-leg rule only looks one gate ahead. ToE2's orbs sit about 10 gates before the diamond, so that case depends on the backjump's gravity/mode/size shortcut, which is a heuristic, not a proof.
- Committing only to the next gate means a decision every 60-90 ticks. In realtime the game will wait at hard spots and at every mode portal, because those are never decided ahead (3480).
- It may be slower than today on easy levels. Clubstep should be compared against about 209 s, not 231.9 s: the baseline includes a level-end loop.
- The map's bands are approximate. Rotated hazards over-block, the corridor bounds are estimates (atlas.cpp:56-72), and so is the envelope (planner.cpp:28-42). The FREE and OBSERVED options limit the cost, but wrong bands still waste legs.
- Some nondeterminism stays in realtime: whether a decision is made ahead or from the real state depends on timing (3447-3495), and world drift can switch the World off mid-run.
- It is a large job against a capped usage week. One commit per logical change and one build per step keep a broken batch bisectable (CLAUDE.md section 0).

## Questions for Nigel

- Calm inputs: what is the shortest gap between two input changes the AI may use before it counts as fast clicking? Suggested: 33 ms, which is at most about 15 presses a second. It would go faster only where nothing calmer gets through, and say so in the log. Black-orb spam is always allowed, because the game needs it.
- Speed: if Route drives gets further on ToE2 and Deadlocked but is slower on Clubstep (about 209 s today), is that OK? Or must it never be slower, as the plan said?
- When it is stuck on one spot, how long should it keep trying before it stops and tells you where? Today it never stops on its own. For example: 5 minutes per spot, or a total time per level for overnight runs.
- Should the bar be measured with your own macro for the level loaded as a hint (the baseline ToE2 and Deadlocked runs had yours loaded), or with no macro at all?
- Once Route drives beats today's engine on Clubstep and ToE2, should it become the default, with today's engine kept as a choice? Or should it stay a switch until the whole bar passes?
- If handing GD's anti-cheat spike to the game after each rewind doesn't fix the lost slope launches, may the pathfinder hide that spike from the player after its own rewinds? This would only apply during pathfinder runs; normal play and the start of the level stay untouched.
