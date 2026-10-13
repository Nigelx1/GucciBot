# AI pathfinder: research notes (2026-10-13)

Raw findings behind AI_PATHFINDER_PLAN.md, from a read-only research run (workflow wf_acd1cd9e-a60): an evidence pass over the Bloodbath logs and pathfinder code, and two independent designs of Nigel's whole-level decider. The plan is the decision; this file is the detail. Estimates are estimates until R0 measures them.

## 1. Evidence: what the Bloodbath stall is

**Verdict:** local_combination_problem (confidence: medium)

### What the logs show

The logs are under C:/Program Files (x86)/Steam/steamapps/common/Geode Geometry Dash/geode/logs/.

RUN 1: "Geode 2026-09-30 12.53.13.log", build -bv (before the -bw/-bx fixes), level "Bloodbath 2", started at tick 1.
- How it entered the section:
  - 6164 "steer 16 (lasts >=960)", then 6804 "steer 64".
  - 7124: "the simulation sees no way past tick 7133; playing steer 16, height +0 into it to see what the game says".
  - 7132: "real path left the simulated one at tick 7132 (off by 0.00, 42.28): thinking again from here". This was a plan decided ahead of the game, from a predicted state. The IDEATH/respawn@7128 line next to it is the pathfinder's own rewind, not an AbsPF death.
  - 7132 "search (lasts 396)", then 7454 "nothing gets past the death at tick 7481 (object 216 at (10635, 478)) - so steer 16 ... is played to 9 ticks short of it", and 7472 the same for 7574.
  - 7803 "orb spam at 40", 8103 "orb spam at 16", 8224.
  - Dead end at 8334 (object 216 at (11745, 404)), then 8334 "search (lasts 342)".
  - 8576 and 8643: "nothing gets past the death at tick 8676 (object 218 at (12185, 472))", played up to just short of it.
  - Dead end at 8665/8671: "nothing gets past object 218 ... and it cannot move".
- The repair:
  - Stops 8664 back to 8153 ("no further than 7945"), then "nothing works within the limit; looking back as far as tick 7236 now", then 7897, 7641, 7236.
  - At every stop, the best idea's "lasts" figure is exactly the distance to 8676. Examples: 8601 "best release at 48 lasts 75", 8153 "played, input 64 moved +1 lasts 523", 7641 "lasts 1035".
  - Floor 7236 approved "played, input 196 moved +1 (lasts >=1440 ticks)". That is the horizon-cap bug fixed in -bw.
  - "resuming at tick 7236" happened 8 times, each with a different nudge, each back at 8676, each "(1 here so far)".
  - Stopped at 12:57:35: "best tick 8671, 207.9s".

RUN 2: "Geode 2026-09-30 15.41.25.log", build -by (includes -bw/-bx), memory run 2.
- It took the same route: 7124 / 7132 (the same 42.28 divergence) / 7454 / 7472 / 7803 / 8103 / 8224 (now "remembered #30") / 8334 / 8576 / 8643, dead end 8665, "this spot bit 1 times in earlier runs".
- Level 1, floor 7236 to 6996: "tick 6996 is the limit going back; trying played, input 139 moved +4 anyway (lasts 1680, needed 1695)". It committed 1681 ticks and really "died at tick 8677". Then: "the game agreed: the object the plan died at (unique id 7382) is deadly". So simulator and game matched over 1680 ticks on that path.
- Levels 2 to 4: floors 6757, 6517, 6277 (about 10 s back). At every stop from 6800 to 8676 the best idea still dies at exactly 8676/8677:
  - 6800: "played, input 132 moved -1 lasts 1876"
  - 6971: "lasts 1705"
  - 7141: "lasts 1535"
- At the furthest stops nothing lasts at all:
  - 6629: "steer 2 lasts 73"
  - 6517: "random #23 lasts 71"
  - 6288: "random #215 lasts 70"
  - 6277: "random #185 lasts 81, needed 2415"
- Level 5 onward: "the floor stays at tick 6277". Levels 5 to 22 took about 0.4 s each (15:44:58 to 15:45:27), with the same stop ticks and the same best-idea lines repeating (for example 8421 "played, input 159 moved -4 lasts 255" in several walks). Stopped at 15:45:27: "best tick 8676, 219.1s".
- No idea in either run got one tick past 8676. Away from the death, the best idea at every stop was a ±1/2/4-tick nudge of the run that had just failed. The only fresh ideas that were best anywhere: taps and releases from 85 ticks or less before the death, and "orb spam at 48 lasts 156" / "random #122 lasts 265" at 8068/8080.
- Both Bloodbath runs logged at start: "World: off, every run uses the fallback paths (the World state after branches is different)". So the trigger model was off for Bloodbath; it was on for every Zafari 2 run.

What I can't tell from the logs:
- the game mode at 8676 (the memory says UFO; the orb-spam ideas suggest orbs);
- whether a way exists from the states it reached;
- whether the fallback simulator is right for routes at other heights.

### What the code does there

All references are in src/absense/. Constants (pathfinder.cpp L53-116): kLookahead 4 s, kBackSeconds 3, kMaxRepairSeconds 6, kMaxWidenSeconds 10, kSearchBudget {40000, 100000, 160000}, kMaxFailsPerSpot 60, kRetriesPerStop 3.

**decide() (pathfinder.cpp L1934):**
- While repairing, the horizon is stretched to the spot plus a little clearance, capped at min(10 s, 6 + level s) (L1967-1992).
- need = distance to the spot + clear (L2009-2022).
- The ideas tried at one decision:
  - "continue": the rest of the previous plan.
  - remembered and human-macro ideas. No macro was loaded for Bloodbath.
  - "played, input N moved ±1/2/4" (L2143-2194): the failed run with exactly ONE of its first 12 input changes moved. The unchanged failed run is forbidden (tabu).
  - steered ideas (buildSteeredCandidates L1539): steer 2-64, height ±0/30/60/100/150.
  - buildCandidates (L954-1195): "keep"; ONE hold, tap or release at an offset from a fixed list of 0-240; holds of fixed lengths; orb-spam bursts; and 40×(1+min(visits,4)+min(level,4)) random scripts of 1-6 changes.
- Then the branching search (L2407-2493). It gets a third of its budget at near stops, the full budget at far stops and the floor, and ×(1+min(level,3)) at a widened spot. That makes 160k simulated ticks at most (budgetScale is 1 at 240 TPS). Results are cached per kept start, budget and level ("the same stop on the same budget is the same answer").
- The verdict (L2830-2877): at the floor, play the best idea "anyway" only when it lasts longer than the margin (120 ticks) and isn't a certain death. A certain death includes being killed by an object the game has confirmed as deadly. Otherwise, escalate.
- Forward decisions in tight stretches commit ideas the simulator knows die, played to just short of the death (forcedCommit, L2910-2946). That is how the run walked into 7454, 7472, 8576 and 8643.

**Branching search (trajectory.cpp):**
- SearchState (L2907) and beginSearch (L3143): carry on (in a UFO, no flap) until death, then branch backwards from the death tick. Every tick is a branch point for the last 8×(1+widen) ticks, then steps of 4/8/16/32. Branches can start at most maxBack = 128×(1+min(widen,3)) ticks before a death, so 512 at most. A child may only branch after its parent's branch tick.
- alternativesFor (L2868) gives the UFO exactly one alternative, a flap, plus an orb press when touching a ring.
- Each branch re-runs the rest of the horizon, so 160k ticks over a 1,500-2,400-tick horizon is only about 70-110 full runs.

**Going back:**
- beginRepair (L3864): floor = 3 s + level, clamped to min(10 s, 6 + level) before the death. Only real deaths, plus a simulator dead end that closes a loop once per resume, count as fails.
- beginBacktrack (L3994): stops come from backDistance (L3849: 1, 2, 3, 4, 6, 8, 12, 16 ... ×1.5/×1.33), shifted by a third each pass (passes % 3, so the same stops come back every 3 passes). The failed prefix is made tabu at each stop. Stops are looked at in the simulator from the kept real start (startAt; captureStart trajectory.cpp L1254 keeps about 2,464 ticks of starts).
- escalate (L3427): level+1, floor 1 s further back but never beyond 10 s, "anyway" counter reset.

**Why it repeats forever:** past level 3 or 4 nothing grows any more: search budget ×4, horizon 10 s, maxBack 512, extra random ideas capped at level 4, floor fixed at 6277, stop ticks repeating. Once the game confirmed killer 7382, every best idea that dies there is a "certain death", so the floor never plays "anyway" again; the stops at 6277-6629 last 70-81 ticks, under the 120-tick margin. The searches and runs come back from the cache, which is why levels 5-22 took about 0.4 s each.

**Divergence check:** afterTick (L3800-3841) re-plans when the real player drifts more than 1 unit from the plan. That produced the 7132 and 7805 rewinds.

### Hypotheses

- **Local combination: the ideas tried can't build a new route through the section (primary)**
  - For: The stops covered the whole section and more: 8676 back to 6277 (10 s, about 850 ticks before the 7132 divergence that seems to be the section's way in). Away from the death, the only ideas that reached the death column at all were the failed run with one input moved by up to 4 ticks; steers, height-holds, randoms, single taps and the branching search never did. The idea pool is mostly 'one change, then nothing'. The search branches at most 128-512 ticks before each death, only gives a UFO a flap, and has budget for about 70-110 full runs. A UFO section needs many flaps whose timings depend on each other, which matches Nigel's own description ('a click can seemingly be open but then could make the click afterwards impossible'). Zafari 2's 14% wall was the same kind of gap (no idea held a height) and was fixed by adding an idea shape, not by changing the approach.
  - Against: There is no proof a way exists from the states the run reached. From the near stops (8601-8664) the search could branch at every tick over the whole remaining distance and still found nothing, so the needed change may be at least 100-300 ticks back, with the state on entry mattering. That shades into the 'earlier decision' hypothesis, at the scale of seconds rather than the whole level.
- **Earlier decision (approach and entry), within seconds**
  - For: Forward decisions in tight stretches committed plans the simulator said die (forcedCommit at 7454, 7472, 8576, 8643), so the run entered the last stretch on ideas it already knew were doomed. Both runs took exactly the same route in (deterministic). Zafari 2's 77.5% stall (tick 11469) had the same signature and was only broken by playing a DIFFERENT route 'anyway' from a floor 8-10 s back (bn: 9549 'steer 16, height +0'; bp: 9549, 9069 'steer 32', 9057 'tap at 96', then 'random #198 (lasts 499)' 265 ticks before the death).
  - Against: Every decision that mattered was inside the window the repair already searches (10 s back). No evidence anywhere needs a choice more than 10 s back or a view of the whole level. The failure is producing alternative routes at those stops, not reaching back far enough. A whole-level planner would still have to produce exact flap timings through 1.5-6 s of section.
- **Simulator mismatch**
  - For: The World trigger model was OFF in both Bloodbath runs ('the World state after branches is different'), so moving objects used the fallback paths. A decision made ahead of the game was 42.28 units off in y at 7132. Slaughterhouse in the same session (also World off) shows real deaths the simulator missed and 'search says it lasts 89 ticks but its script lasts 63'. Routes at other heights through the UFO have never been checked against the game.
  - Against: The one route actually played for real (6996 to 8677, 1681 ticks) died exactly where the simulator said, and the log reads 'the game agreed ... (unique id 7382)'. The killer is called static ('it cannot move'). Bloodbath logged no 'did not see that death' and no divergence between 7132 and 8677.
- **Window too short**
  - For: From the furthest stops (6277-6629) nothing lasted more than 81 ticks, so alternative ways in were effectively only tested from about 6800 (7.8 s before the death).
  - Against: The floor reached 6277, 2,400 ticks (10 s) back and before the section's apparent way in at about 7132. The hard part is 7454-8676 (5 s). Humans pass Bloodbath from whatever state they arrive in. Nothing points to a fix more than 10 s back.
- **The escalation loop (makes the stall permanent, but isn't the root cause)**
  - For: Once the killer was confirmed, 'anyway' was switched off. Budget, horizon, branch reach and random ideas all stop growing at level 3-4, the floor stays put, stops repeat every 3 passes, and results come from the cache: levels 5-22 took about 0.4 s each. Run 1's loop came from the floor-horizon bug fixed in -bw.
  - Against: This explains why it never gets unstuck, not why no idea gets past 8676.

### How to tell for sure

In order, cheapest first:

1. **Check the simulator with a known-good route.** If any Bloodbath macro exists (Nigel's .gdr/.slc files), load it as the human reference, or replay its inputs in the simulator (gucci_abs_simulate) from a kept start around 7100-7300, and compare tick by tick with gucci_sim_vs_real / gucci_tick_probe through 8700.
   - If the simulator kills the human route near 8676, the problem is the simulator, not the search.
   - If the simulator passes it, a way exists and the problem is search coverage.

2. **Find out why the World turns off on Bloodbath** ("the World state after branches is different"). Re-run the same stall with it on, if it can be fixed safely. If the stall moves or disappears, the fallback simulator was involved.

3. **A narrow exhaustive check from the nearest stops** (8601, 8569, 8537): run the branching search uncapped (budget unlimited, maxBack covering the whole distance, every tick a branch point). It's cheap there (75-140 ticks) and tells you whether the state 8601 already dooms the run.

4. **The M2 physics map from kept starts, latest first:** 8550, 8450, 8300, 8100, 7800, 7500, 7141, then 6800 and 6300, each swept to about 8700. The first stop with a route past 8676 says how far back the needed change is: under about 2 s is local combination, 5-10 s is approach within the window, none down to 6277 with the simulator checked means window or state. Only an uncapped failure counts as a proof. Note that merging quantised states (y about 0.05, yVel about 0.01) makes a "no way" result approximate.

5. **Compare with Zafari 2 at tick 11469.** Its logs show the same signature and how it was broken (floor 'anyway' 8-10 s back). Running M2 there too would show whether that stall was also a combination the sweep finds from a stop under 10 s back.

### Other stuck patterns

**Zafari 2, 14% wall** (memory; tick about 2260, mini ship, x about 2366).
- Every route dived under the two black boxes. gucci_sim_vs_real showed simulator = game to 0.01 through the portals.
- Fixed by -bn's height-hold steer ideas (14% to 77.5% in 2 minutes, full solve in 244 s).
- Class: local combination. A missing idea shape, a long run of inputs that holds a height, which the one-change ideas and the branching search couldn't build.

**Zafari 2, 77.5%** (tick 11469, x about 13,000; "Geode 2026-09-28 22.57.57.log" build -bn and "23.13.37.log" -bp).
- Exactly Bloodbath's signature: "nothing lasts even a tick from here"; every stop's best is "played, input N moved ±k" lasting exactly to 11469; a floor "anyway" with a nudge of the failed run (10029, "input 3 moved +4", lasts 1439) died at 11469 again; then levels 1-4. In -bp the game confirmed killer 13178.
- How it was broken:
  - -bn: about 70 s; a floor "anyway" 8 s back with a different route ("tick 9549: steer 16, height +0 (lasts 1031)"), then local repairs.
  - -bp: about 4.5 minutes (23:20:54 to 23:25:28); "anyway" at 9549, 9069 ("steer 32") and 9057 ("tap at 96"), then "tick 11203: random #198 (lasts 499)" from a stop 265 ticks before the death.
  - The -bq run passed it with memory.
- Class: mixed. A change of approach 8-10 s back plus lucky multi-input random ideas, all inside the repair window. It escaped only because the floor's best ideas died on something other than the confirmed killer, so "anyway" was still allowed. Bloodbath lacks that escape.

**Zafari 2, late section** (ticks 13276-14691).
- About 9 real deaths with "the simulation did not see that death", at y about 1455-1490 (near the ceiling).
- Each was repaired in 1-3 s from 12-256 ticks back.
- Class: simulator mismatch with cheap local repairs. Also big divergences elsewhere ("off by 0.00, 127.64" at 11223; 188.72 at 10297 in -bq).

**Bloodbath, early dead ends** (run 2: 1029/1040; 2908-2938).
- Fixed from stops 16-267 ticks back; at 2938, "the way found from tick 2876 did not get past the spot", then a retry.
- Class: local timing, fine.

**Slaughterhouse** (12.53 log, from 12:57:56; World OFF).
- 208 dead ends at ticks 3300-3499 in about 2 minutes, never past.
- The killer object 1734 is at a different place from run to run ((3482,581) to (3499,586)).
- Real deaths the simulator missed at 1953 and 3328; "search says it lasts 89 ticks but its script lasts 63" six times; divergences at 3320/3339/3322 including x off by 6.28.
- Retry loops at the same stop 3297 ("retry 1 of 3" ...).
- Class: simulator mismatch (moving objects on the fallback paths) plus local loops.

**Other runs.**
- Xmas Challenge: solved in 55.8 s (-bu); earlier runs were closed by the user, not stuck.
- DIOOZ GOD: stopped by the user after 8 s, nothing to classify.

**Memory, pre-port pathfinder** (Sonic Wave 40%, orb corridors).
- Recorded as decisions that couldn't see the death beyond the look-ahead, and floors that approved "survives the horizon".
- Class: window and horizon bookkeeping bugs, fixed in code since.

### What it means for Nigel's design

Mostly no, the evidence doesn't support "the AI must see the whole level and make the decisions".

In every stall in the logs, the fix lies within about 1-10 s of the death, and today's repair already goes back that far (Bloodbath's floor reached 6277, 10 s back, before the section's apparent way in at about 7132). What fails is what it tries at those stops:
- one change, then nothing;
- the failed run with one input nudged by up to 4 ticks;
- a branching search that branches at most 128-512 ticks before each death, gives a UFO one move (a flap), and has budget for about 100 full runs;
- on top of that, loop rules that stop growing at level 3-4 and, once the game confirms the killer, never let the floor try a different route again.

So the pathfinder is stuck because it can't generate the many-flap combination through about 1.5-6 s of section, not because a decision far upstream is wrong. A whole-level AI that "makes the decisions" would hit the same wall unless it can turn its decision into exact flap timings, and that is exactly the local combination problem. "The AI decides, the physics makes the path" is also roughly what exists today: ideas are proposed, the exact simulator judges them.

**Where Nigel is partly right:**
- Approach choices do matter, at the scale of seconds. Zafari 2's 77.5% stall broke only when a different route was committed 8-10 s back.
- The forward planner deliberately walks into plans it knows die in tight corridors.

So an "AI" (or a sweep) that picks routes over the next few seconds, latest stop first, fits the evidence. A view of the whole level isn't needed for these stalls; it might help with things not seen here, such as alternate paths or choosing which kept start to repair from.

**Two cautions:**
- It's possible the simulator itself is wrong on Bloodbath: the trigger model was off in both runs. Neither an AI nor a sweep can fix that, so check it first with a known-good macro.
- The physics map (M2) needs to sweep from stops several seconds back, not only a few seconds ahead of the death, and with merged states a "no way" result is only approximately a proof.

## 2. Design A (chosen): the Route Planner, no training

**Design A: the Route Planner. A whole-level map plus a chess-engine-style route search, with GD's exact physics drawing each leg and proving dead ends**

### In plain English

When a level loads, the bot reads every object in the level at once and draws its own map: where the gaps are, which ones split into an upper and a lower way, and where every orb, pad, portal and mode change sits. On that map it picks a whole route from start to finish, the way a sat-nav does. A route is a list of choices: "go under this block", "use this orb", "skip that pad", "be low and falling when you reach the UFO part". Nobody teaches it anything. It picks the cheapest-looking route, and tight gaps and many fast clicks cost more. Then GD's own physics, on copies of the player, works out the exact clicks to get from one choice to the next, a short stretch (a "leg") at a time. The real game plays those clicks, as it does today. If the physics finds no way through a leg, it says so, and when it tried every possible click to get that answer, that counts as proof. The planner then crosses that choice off for good and picks the next-best route. That can mean changing a choice 30 seconds back, not only the last 10 seconds. It never retries a choice it has proven dead, so it can't spin on one spot the way today's pathfinder does at the Michigun UFO. It only fixes bad choices, though. If GD's physics copy gets the game wrong at a spot, the bot still gets stuck there and reports it, and fixing the copy is separate work.

### How it sees the whole level

THE ATLAS (new, src/absense/route/atlas.*), built once per level read and patched as runs see more.

1. Objects. It makes one pass over pl->m_objects, the same loop Client::sendLevel in src/absense/gpu/client.cpp uses, and calls classify() on each object. That gives a box or circle plus one of 18 kinds: solid, hazard, slope, jump/gravity/drop/spider/dash/gravity-dash orbs with their power and multi-activate flag, jump/gravity/spider pads, gravity portals up, down and toggle, mode portals (power = mode 0-7), size portals and speed portals. changesEverything() marks dual, solo and teleport portals, teleport orbs, custom rings, and triggers 2066, 2900 and 3022. world::certaintyOf() labels each object Static, Modelled (the World runs whatever moves it) or Uncertain. classify() and changesEverything() move into a shared file so the atlas and the GPU client use one copy.

2. Time along the level. In a non-platformer level the horizontal speed comes from speed portals only, so arrival time is roughly a fixed function of x. The atlas walks the speed portals in x order from the start speed and builds t(x), the tick at which any route reaches column x. A speed portal that a route can pass above or below is marked as a decision with two timelines. In practice this means moving objects are at the same place for every route at a given x. The World runs triggers off player 1's x (its spawn walk), so most of them fire at the same x whatever route is taken.

3. Columns and openings. The level is cut into columns one block (30 units) wide; Bloodbath's 39,369 units give about 1,312 columns. In each column it takes the vertical band the current mode allows (the bounds a mode portal sets for ship, UFO, wave, swing, ball and spider; open upward for cube and robot unless set otherwise). It then subtracts hazards, grown by the player's outer hitbox, and solids, grown by the inner hitbox, once for normal size and once for mini. What is left is a list of OPENINGS, free y-intervals, usually 1 to 4 per column.

4. Dynamic columns. A column whose objects are Modelled, Uncertain, or in a toggled group is marked dynamic. The atlas makes no geometric claim there: it treats the whole column as open with an unknown cost. Openings for it are filled in from what the exact sim's runs see when they pass that x (the World puts the objects where they are at that time), and refilled if the timeline changes. Positions after a changesEverything marker are not predicted. The atlas splits the level into segments at those markers and re-anchors on the state physics gives it after the crossing.

5. The graph. Openings in neighbouring columns that overlap are linked. The atlas groups them into REGIONS (runs with no split) and places a GATE wherever a choice exists or the route gets tight:
- a fork, where an opening splits around an obstacle;
- an orb, pad or portal that a route can either touch or avoid;
- a mode, size, gravity or speed section boundary;
- a column whose opening is under about 2 player heights;
- at least one gate every 1-2 s, so no leg is too long.
Dual parts get a pair of bands per gate, one for each player.

6. Size: about 300-1,500 gates per level, with 1-4 options each. RAM is roughly 1-5 MB, including a transient copy of the object boxes at ~40 B each. Building it should take well under a second (one pass plus a sort by x), but that is an estimate. Nothing is stored in the mod download.

### What a decision is

A decision is the option picked at one gate. Every option is written as a WAYPOINT: an x, a y-band (optionally with a vertical-speed band, rising or falling), the mode, size and gravity it expects, the interactions it requires since the previous gate, and the ones it forbids. The concrete kinds:
- Fork side: above or below obstacle k, which means a particular opening in column c.
- Orb: use it or skip it. "Use" forces the route's band through the orb's contact region (the box or radius grown by the player's hitbox) and requires the ring to fire. The exact tick inside the contact window is not decided here; the leg's physics sweep finds it, because alternativesFor() already offers the on-ring press and the multi-activate spam cadences at every tick of contact. Dash orbs add "use, and release by column c".
- Pad, gravity/mode/size/speed portal: touch or avoid. This exists only when the opening extends past the object's vertical span. Most mode portals fill the corridor, so for them there is nothing to choose.
- Entry band for a section or a tight gap, for example "enter the UFO part between y 150 and 210, falling" or "enter the wave at the floor". This is the kind of choice height-hold steering made by hand to solve Zafari 2's 14% wall.
- Gravity state at a gate inside ball, spider or swing, where clicks flip gravity.
- Dual: a band for each player.
Per-tick clicks are deliberately not decisions: how many clicks, and exactly when, is the physics' job inside a leg. Coarse click budgets only enter as a cost: an option that needs a click every few ticks to survive costs more.

### How it decides

1. Calibrate the movement limits. For each (mode, speed, size, gravity) the level uses, the exact sim flies a copy in open air for about 2 s with button held, released, and the mode's fastest switching, and records how far y can rise or fall per unit of x. Examples: the wave's fixed slope, the ship's climb and dive limits, the UFO's flap arc, and the cube's jump arc (reachable only from the ground, an orb or a pad). This takes about 8 short sim runs per mode, measured from GD itself rather than typed in. The result is cached per level in the save folder (a few KB).

2. Edge costs. An edge goes from option a at gate i to option b at gate i+1. If b's band falls outside the movement limits from a's band over that x distance, the edge gets a very large cost, but not infinity: only physics may prove something impossible. Otherwise the cost is a sum of:
- tightness, summed over columns as player height divided by opening height;
- clicks per second the stretch needs;
- frame-perfect contact windows (orb contacts shorter than a few ticks);
- dynamic or unknown columns, which cost a little extra.
Then the FEASIBILITY CACHE (step 5) changes it: a witness found here makes it cheaper; each budget-out doubles a penalty; a proof makes it infinite.

3. Global search. A* runs from the committed state (current gate, entry bucket) to the level end over this graph. The heuristic is the remaining x times the cheapest cost per column, which never overestimates. A K-best pass (Yen's algorithm, K = 8) keeps the next routes ready. Ties break by gate index, then lower band, then option index, so the result is deterministic. The graph is small, so a full replan takes about a millisecond. It reruns after every leg result.

4. Execute the first two legs. The physics tries leg 1 (to gate i+1) and then leg 2 from leg 1's exits. Leg 1 is committed only when leg 2 has a witness too. The bot therefore never enters a gate in a state the next stretch cannot get out of, which is the "a click that's open now makes the next click impossible" trap Nigel described.

5. Record results. Each leg result is stored under (gate, option, entry bucket). An entry bucket is y in steps of 15 units, vertical speed in 3 bands, mode, gravity, size, speed, button held, orb contact, and in dual both players. Possible results:
- Witness: one or more exits, with scripts.
- Budget-out: the frontier was capped or the tick budget ran out. This means "unknown".
- Proof: no state got through, and nothing was ever dropped except deaths and exact duplicates.

6. Learn what is impossible (nogoods), the way a SAT solver learns from conflicts. Each gate keeps the complete set of alive states that crossed it, but only when every sweep since the last real start was uncapped. If that set is proven unable to pass every option at the next gate, then the option that produced it is proven dead. The same reasoning then repeats one gate further back. If any sweep along the way was capped, the result only raises costs.

7. Re-decide. If the best route still agrees with the committed path up to the current gate, carry on. If it first differs at an earlier gate g_b, jump back to g_b, however far that is (see when_physics_says_no). Hysteresis stops it flip-flopping: it jumps back only when the current continuation is proven dead, or costs much more (2x) than the alternative after at least two budget increases.

8. Fallback. If every route is proven dead from the level start, it reports "no route with whole-tick inputs". If the remaining routes are all budget-outs, it raises budgets and repeats. Unattended runs have a time limit per spot.

### How physics makes the path

THE LEG EXECUTOR (new, src/absense/route/leg.*). This is the M2 physics map with a goal added.

Starting set S: the kept start at the committed tick (m_starts, one SimStart per tick for the last ~10 s), or exits of the previous leg, carried there without real ticks with Trajectory::advanceStart().

Search: breadth-first over ticks.
- Every alive state expands into "carry on" plus every option alternativesFor() gives at that tick: tap, hold, release, re-press on a ring, and the orb-spam units. Deaths are dropped; the World, phantoms and confirmed killers all apply as they do now.
- States are merged by key: quantized in speed mode, bit-exact in proof mode. Each state keeps (parent, input), so the winning script can be rebuilt without storing histories.
- States are ranked inside the frontier by: inside the route's corridor bands, distance from the corridor's centre line, fewer clicks, then a deterministic tie-break.
- In speed mode, states outside the corridor are pruned, except for a small escape quota (about 5% of the frontier). Proof mode prunes nothing.
- The frontier cap comes from M0's snapshot-cost measurement. Where snapshots are expensive, it uses the DFS's trick: a snapshot every 4 ticks (kSnapEvery = 4), re-running the ticks in between.

Success: a state crosses the gate's x inside its band, with the required orb uses done and the forbidden objects untouched. Ring use and contacts come from the Sim's m_ringUsed and activated sets, which every snapshot carries. Each exit must also survive a short tail (carry-on or steer, ≥ clearance ticks, the same clear = max(8, margin/8) decide() uses). Up to E = 8 exits are kept, in distinct buckets, so the next leg starts from a spread of states and not one fragile one.

Cheap paths first. Before any sweep, the existing cheap ideas get a go: steer() with an absolute target height (a small change to Trajectory::steer(aimOffset) so it aims at the gate band's centre), Trajectory::search(), and remembered solutions. The sweep only runs when none of them passes the gate.

Into the real game. The leg's script becomes a Candidate named "route" in decide(), entering the trust chain unchanged: evaluate(), the one-tick-late robust() check, trimStep(), commitTick() on real ticks, kDivergence = 1.0 replan, judge::run on disagreements. While a route is active, decide()'s bar `need` becomes "passes the next gate in band" in place of "lasts long enough". Candidates that pass the gate rank above ones that merely last longer. That replaces the survival-greedy pick behind many stalls, and the existing ideas (continue, steer, height-hold, search, memory, human) still compete under the new bar.

Checkpoints. Anchors (full game checkpoints) are kept at every gate the committed path crossed, in addition to the current ≥240-tick spacing, thinned within the existing 8-64 maxAnchors cap. The level start anchor is always kept. With those, a jump back lands on a gate after a short restep of m_history (finishRestore already resteps in deadline-cut slices).

### When physics says no

How it detects an impossible choice:
- PROOF. The leg ran in proof mode (exact keys, no corridor pruning, no cap reached) and the frontier emptied before the gate, or no state crossed inside the band. That proves no whole-tick input sequence at this TPS gets from S through that option. It is exact only for that start set S. It is not a proof against sub-tick or CBF inputs, and quantized merging would void it, which is why proof mode uses exact keys.
- BUDGET-OUT. A cap, pruning or the tick budget ran out. It is recorded as "unknown". The penalty doubles and the budget doubles up to a ceiling, for example the 40k-160k range of kSearchBudget for speed mode and 1-4M ticks for proof mode (estimates, to be set by M0).
- REAL DISAGREEMENT. The real game died, or drifted more than 1.0 unit, where the leg's witness lived. This is never recorded as impossible. It feeds the existing phantom, confirmed-killer and judge logic and marks the gate "sim untrusted". After 3 disagreements at one gate it stops and reports "the simulation disagrees with the game at x=..." so it doesn't loop.

Re-deciding globally. Proofs set the edge to infinity, and nogoods propagate backwards as described in how_it_decides step 6. A* then reruns over the whole level. The re-decision is not limited to a time window: it lands on the latest gate where an option is not proven dead, which can be any distance back.

Going back further than today's 10 s. Today, kept starts reach ~10 s (kMaxWidenSeconds) and repair floors 6-10 s (kMaxRepairSeconds plus the level). Past that, the jump-back goes through the real game:
1. Cut the committed macro at tick T_b, the tick the committed path crossed gate g_b.
2. Restore the nearest anchor at or before T_b (gate anchors make that T_b itself).
3. Restep m_history to T_b, using the finishRestore / m_restepTo path that already exists.
4. keepStart() there, and run the new route's legs from that state.
The old suffix is kept as a fallback route (as m_bestActions and saveFurthest do now), so a failed alternative can return to the best-known path.

No repeats. Every (gate, option, entry bucket) result is cached. The search never re-runs a proven-dead combination, and budget-outs come back only with a bigger budget. That directly replaces today's escalation, which repeats cached searches at the same stops. The existing m_tabu, traps and escalate() stay as the fallback when no route is active (World off, a platformer level, or the Route switch turned off).

### What it fixes, and what it doesn't

It fixes:
1. Wrong-entry decisions made before the death: the wrong height or speed band entering a section, an orb taken or skipped that dooms the next stretch, a gravity flip on the wrong side. Gates force the choice to be explicit, the two-leg rule refuses doomed entries, and nogoods push the change back to whichever gate caused it, whatever the distance.
2. The survival-greedy objective. decide() keeps the idea that lasts longest, but the only way through is often a shorter-lasting plan that is in the right place. In the Michigun log (Geode 2026-09-30 12.53.13) the UFO stretch was entered on "orb spam at 40 (lasts 420)", "orb spam at 16 (lasts 241)" and "release 11 at 96 (lasts 230)" at ticks 7803-8224. Every stop from 8664 back then said "nothing here", the dead end being object 218 at (12185, 472), which "cannot move". Judging by "passes the gate in band" changes which plan gets committed.
3. A missing combination. The leg sweep tries every whole-tick input at every tick, not a fixed menu.
4. Repeated cached searches and endless escalation. Proven-dead combinations are never retried, and budget-outs only come back larger.
5. A cause more than 10 s back. Jump-backs go through anchors and the committed history.

It does not fix:
1. The simulator being wrong. In the same log the real path left the simulated one at tick 7132 by 42 units, and the real game killed the player at 7134 where the sim saw nothing. If the Michigun death comes from that kind of mismatch, this design hits it as a real disagreement and stops with a report; it does not solve it. That needs the World and the copies fixed (gucci_sim_vs_real).
2. Spots needing sub-tick or CBF timing. Proofs only cover whole ticks.
3. Platformer levels, where x is not fixed. They are outside the 2.0 bar; this design is off for them.
4. Huge open sections (long wave spam, swing corridors) when even one leg's frontier is too big. More gates help, but a capped sweep only says "unknown", never "impossible".
5. Levels whose geometry depends on the route through triggers the atlas can't see: touch triggers, item counters fed by orbs, random triggers, toggles set by earlier choices. The atlas misleads there, which costs time but never produces a wrong macro, because physics decides.

I'm not sure the Michigun stall is a decision problem. The log is consistent with a wrong entry 2-4 s before the death, which is inside today's 10 s window. That points at the objective and the menu, not the window length, and this design fixes both. Running the M2 sweep from those kept starts is still the deciding test.

### Training

None. No learned weights and no example data. The only thing it "learns" while running is the movement-limit calibration (sim runs at level start, seconds of work, a few KB cached per level in the save folder) and the per-run feasibility cache. memory::Store can optionally keep route choices that worked ("gate at x=12,000: below") across runs, as it keeps spots now.

### Size and speed

What ships is code only: an estimated 4-6k lines of C++, which should add a few hundred KB to the DLL (estimate). No model file, no downloads, no Claude, no internet. The mod stays about 233 MB.

RAM per level (estimates):
- atlas 1-5 MB;
- feasibility cache capped at about 50 MB;
- gate anchors: up to the existing 64-anchor cap, megabytes each on big levels, as today;
- leg frontier capped by M0's measurement, with a target under 500 MB. Each SimSnapshot holds per-player state plus hash maps and a World state; their size is unmeasured.

Time (estimates until M0 measures sim ticks/s and snapshot cost):
- atlas build under 1 s;
- a global replan about 1 ms;
- a leg: tens of ms when a cheap idea passes the gate; 0.1-5 s for a capped sweep; possibly tens of seconds for a proof-mode sweep;
- a long jump back costs a restore plus at most the anchor spacing of restep.

Rough totals: easy RobTop levels in minutes. Zafari 2 should be no slower than today's 170-450 s, because cheap ideas still run first under the gate bar. Bloodbath maybe 10-60 min unattended, if the sim is right there. Everything runs on GD's main thread in deadline slices: one sim per GD process, so the i5-12400's other cores don't help unless M0 shows a second GD process is safe.

### Fit with the existing code

REUSED:
- Exact simulator (src/absense/trajectory/trajectory.{hpp,cpp}): Trajectory::run / steer (aimOffset) / search / beginSearch-stepSearch-finishSearch; SimSnapshot and the Sim snapshot/restore; captureStart / useStart / advanceStart / startState; alternativesFor() as the complete per-tick move set, with the orb-spam cadences and on-ring presses; the m_ringUsed / m_ringTouch contact tracking; phantoms; lastKiller().certainty.
- World (src/absense/world/): run as now. certaintyOf() labels atlas columns; cardSlice() / the materializer give dynamic objects' poses.
- Level reading (src/absense/gpu/client.cpp): classify(), changesEverything(), dashRise(), and the object loop of sendLevel, moved to a shared file. The 18 ObjectKinds come from src/absense/gpu/protocol.hpp.
- Pathfinder (src/absense/pathfinder/pathfinder.cpp, .hpp):
  - decide() / evaluate() / robust() / trimStep() / commitTick() unchanged. Route witnesses enter as Candidates named "route", and d.need gets a gate-pass variant.
  - Kept starts (keepStart, m_starts, ~10 s) and anchors (keepAnchor, maxAnchors 8-64), plus gate anchors.
  - beginBacktrack / finishRestore / m_restepTo / rewindTo do the long jumps back. m_history is the committed macro.
  - m_tabu, traps and escalate() / moveStartBack() stay as the fallback.
  - RunMemo caches runs inside a leg.
  - m_bestActions / saveFurthest keep the fallback route.
- Memory and human macros (src/absense/pathfinder/memory.*, human.*): memory::Store keeps route choices per x. A loaded human macro, played through the atlas, gives its gate options as a strong first route (optional).
- Trust and testing: src/absense/judge.* and the MCP tools gucci_sim_vs_real, gucci_abs_simulate, gucci_tick_probe, gucci_open_level and gucci_restart_game.

NEW (src/absense/route/):
- atlas.{hpp,cpp}: columns, openings, regions, gates, t(x), dynamic and observed columns.
- envelope.{hpp,cpp}: movement-limit calibration per mode, speed, size and gravity.
- planner.{hpp,cpp}: edge costs, A* plus Yen K-best, the feasibility cache, nogood propagation, jump-back choice with hysteresis.
- leg.{hpp,cpp}: the goal-directed physics-map sweep (M2) with corridor pruning, exits, and proof vs budget-out.
- An overlay through the existing TrajectoryDrawNode.
- A Pathfinder engine pill "Route", next to Absense and Classic in the Pathfinder page.
- MCP tools gucci_route_atlas, gucci_route_plan and gucci_route_leg, so a session can check it offline from Nigel.
- About 200 lines of hooks in pathfinder.cpp: the route candidate, the gate need, the gate-anchor policy, the jump-back target.

This replaces AI_PATHFINDER_PLAN.md's M4-M6 (the learned network). It keeps M0 (measure), M1 (the level as data, now the atlas), M2 (the physics map, now the leg executor) and M3 (confirm run, the two bug fixes, overnight harness).

### Build steps

- **R0: measure (the plan's M0)**: Logged sim ticks per second, SimSnapshot save and restore cost and size, restore-plus-restep cost per second of level, and whether a second GD process is safe. A speed-test button and gucci_abs_benchmark. These numbers set the leg frontier cap and the proof-mode budgets. *Nigel tests:* Run the pathfinder on Zafari 2 for a minute, press the speed test and read the numbers out (or Claude reads the log). Try opening GD twice.
- **R1: the atlas and its overlay**: The whole-level map built at level load: openings shaded by mode section, gates as vertical ticks, orbs, pads and portals marked touch-or-avoid, dynamic columns hatched, changes-everything markers in red. gucci_route_atlas dumps it. *Nigel tests:* Open Zafari 2, Xmas Challenge and Bloodbath with the overlay on and scroll or play through. Do the shaded gaps sit where the real gaps are, especially the Michigun UFO stretch (about x 10,000-12,400) and Zafari 2's 14% ship gap? Screenshot anything that looks off.
- **R2: the planner draws its route, without driving**: Movement-limit calibration plus A* and K-best. A line through the whole level from the start, with dim alternatives. Nothing is played. *Nigel tests:* Look at the line in the three levels. Does it go where a person would? At the Michigun UFO, does it pick the way you'd take? Point out where the line is impossible: those are the costs to tune.
- **R3: the leg executor on demand (the plan's M2)**: The physics map with a goal: from the current state, find a way to the next gate. It shows the dots, then reports found (with the path drawn), proven impossible, or out of budget. gucci_route_leg exposes it. *Nigel tests:* In practice mode, stop just before the Michigun UFO (and at the spot where the old pathfinder dies) and press 'find a way to the next gate' from a few points 2-6 s back. Is there a way past 12,185, or a proof there isn't? This is the test that shows whether the stall is a decision problem or a simulator problem.
- **R4: the Route engine drives, inside the 10 s window**: A 'Route' engine pill. Route witnesses become decide() candidates, the gate-pass bar applies, the two-leg rule, the feasibility cache, and nogood propagation within the kept starts. *Nigel tests:* Zafari 2 and Xmas Challenge with Route vs Absense: it must be no slower and still pass the replay. Bloodbath: does it get past the UFO?
- **R5: long jumps back and route memory**: Gate anchors, jumps back to any earlier gate through restore plus restep, the fallback route, hysteresis, the 'simulation disagrees at x' stop, and route choices remembered across runs. *Nigel tests:* Bloodbath from the start, unattended. Watch the log for 'jumping back to gate at tick N' further back than 10 s, and confirm it never retries a proven-dead choice.
- **R6: proof runs (the plan's M3)**: The fix for the restart-after-replay level-end skip and the fix for the judge's armed step flag. The cold confirm run plus Check Macro. The overnight harness over a level list, with a solved/failed report. *Nigel tests:* One night unattended on the RobTop levels plus your 20 online levels, then read the report against the 2.0 bar.

### Risks

- Simulator mismatch is the real cause at the Michigun UFO. The 2026-09-30 log shows a 42-unit drift at tick 7132 and a real death at 7134 the sim didn't see. Then the route planner stops with a report and does not solve it.
- Proof-mode sweeps (exact keys, no pruning) may blow up in flying modes, where exact states rarely coincide. The result would be budget-outs rather than proofs, and soft costs rather than clean global re-decisions.
- Snapshot cost and size are unmeasured. SimSnapshot carries hash maps and a WorldState, so a wide frontier may cost hundreds of MB or be slow. The fallback (sparse snapshots plus re-running) trades memory for time.
- The atlas is approximate on 2.2 content. classify() bakes rotation into axis-aligned boxes (so rotated hazards and slopes over-block), and moving or toggled groups, area triggers and keyframes leave columns 'dynamic'. A wrong map only costs time, because only physics proves anything impossible, but bad costs could make it much slower than today on some levels.
- The assumption that arrival time depends only on x breaks with speed portals a route can miss, dash orbs in odd setups, teleports, rotate-gameplay and gravity triggers. The atlas has to split timelines and segments correctly.
- Long jumps back cost real restores plus restep. The anchor memory cap (64, megabytes each) limits how many gate anchors fit on long levels.
- Integration into the 4,385-line pathfinder.cpp risks half-ports: the gate bar or the route candidate gets wired in one path (live, ahead, probe, pair) and not another.
- Dual and two-player parts multiply the state and bucket space. Gates with a band for each player may blow up the cache and the sweeps.
- Everything is on GD's main thread, one sim per process: no parallel speed-up unless a second GD process proves safe in R0.
- The World is Windows-only, so off Windows the atlas's dynamic columns are never observed accurately and the design runs as a rough guide on macOS, iOS and Android.

### Unsure

- Whether the Michigun UFO stall is a decision problem at all. The log fits a wrong entry 2-4 s before the death, but also shows sim drift and unseen deaths nearby. R3 decides it.
- Sim throughput (ticks per second) and SimSnapshot size and cost, so every time and RAM figure here is an estimate until R0.
- Whether exact-key proof sweeps finish within a usable budget in UFO, ship and wave legs, or only ever return budget-outs.
- Whether the World can give positions of moving objects at arbitrary future ticks without a player run (cardSlice() steps ahead from the live tick). If not, dynamic columns are only learned from sim runs that reach them.
- How well arrival time depends only on x in Bloodbath, Zafari 2 and Xmas Challenge, i.e. whether any of their speed portals can be missed.
- Whether speed portals are touch-activated with a hitbox a route can avoid in 2.2081. I believe so but haven't checked.
- The right gate density for long open sections: too few gates make legs blow up, too many make the A* graph and the two-leg rule slow.
- The exact flying bounds each mode portal sets in 2.2. The atlas should read them from the sim's PortalState rather than from constants I'd type in.
- How much the gate-pass bar alone (without the atlas) would already change on Bloodbath. It might be worth testing first as a cheap experiment.
- Restep speed for long jumps back (real ticks per second while restepping), which decides whether 30-60 s jumps are practical.

## 3. Design B (optional later): a trained whole-level decider

**Design B: a small self-trained "route drawer" that sees the whole level and picks the decisions, with GD's physics filling in the exact clicks**

### In plain English

When a level loads, GucciBot first turns the whole level into a strip of data: one slice per block of distance, showing where the blocks, spikes, orbs, pads and portals are. A small AI, trained by us and about half a megabyte in size, reads the whole strip at once and draws a rough band through the level: "go under here, take this orb, skip that pad, arrive at this height going up". Those choices become checkpoints. GD's own physics then finds the exact clicks that get from one checkpoint to the next. If the physics proves a checkpoint can't be reached, that choice is crossed off and the AI is asked again with it ruled out. It can jump back to the choice that caused the problem, even if that choice is 20 seconds earlier, instead of creeping back tick by tick and stopping at 10 seconds the way today's pathfinder does. You're right that the problem is decisions. The part that fixes it is this "decide, then let physics check" loop, and that loop needs no training. The trained AI makes it pick right first time more often, so it wastes less time on dead ends. It learns from levels GucciBot solves (or proves unsolvable) by itself overnight, plus practice levels we generate, never from anyone's macros. Expect several nights of your PC running and one to three hours of teaching per round. It can't fix spots where the simulator itself is wrong. My honest advice is to build the no-training version of this loop first, measure where it wastes time, and only then train the AI.

### How it sees the whole level

THE LEVEL TAPE (new src/ai/tape.*), built once per level at pathfinder start.

1. Object source: classify() and changesEverything() move out of the anonymous namespace in src/absense/gpu/client.cpp into a shared src/ai/level_data.* (plan M1 already calls for this). Every object in pl->m_objects is classified, not a slice around the player as sendLevel() does. This gives the gpu/protocol.hpp Object records: box, radius, power, kind (18 kinds), flags (multi-activate, facing down), vx and vy.

2. Movers (a survey run): world::cardSlice() steps the World "with no copies", so it misses spawns that the player's x fires. Instead, one real-game survey pass runs with noclip and a "keep" script. At every column it records the classified boxes within about 1,000 units, so moving objects sit where they really are when the player passes. Columns where a mover depends on the player (touch or orb-fired spawns, Certainty::Uncertain objects) get an "uncertain" flag. This works off Windows too, because it is the real game.

3. Columns: one per block (30 units of x). In classic, non-platformer levels the tick at which x is reached hardly depends on input. The exceptions are speed portals (known from the tape), teleports, gameplay rotation and dual mode, and those are cut out as separate sections. So a column is also a fixed tick. Each column holds:
   - a 64-cell vertical window of 15 units each (960 units in all), with 3 occupancy bits per cell (solid, slope, hazard) and the window's origin as a scalar. The origin is the flying band set by the mode portal, or a smoothed track of the walkable surface in ground modes; this rule is unsure.
   - up to 6 "special" tokens: orbs, pads and portals, each with kind one-hot, y, power, flags, vx and vy.
   - column scalars: speed, the section's mode and size, changes-everything, uncertain, the floor at y 90.
   That is about 300 features per column, mostly sparse bits.

4. Sections: cut at mode, size, speed and gravity portals, gravity toggles, changes-everything objects, and dual or solo portals. Dual and teleport stretches are marked "hand back to today's decide()".

5. Decision slots, found geometrically: a "fork column" is one where the free space in the band splits into 2 or more corridors separated by solid or hazard; consecutive columns with the same corridor layout merge into one fork. Every orb and pad is a slot. Every section boundary is an exit-state slot.

Size: Bloodbath is 39,369 units, about 1,312 columns. That is about 100 KB bit-packed, or about 1.6 MB as floats for the net. Expect roughly 20-60 sections and roughly 200-800 decision slots per level (an estimate, not counted). An overlay draws the tape: occupancy, corridors, slots and section cuts.

### What a decision is

A decision is a waypoint constraint, never a tick. Three kinds:

1. ROUTE BRANCH at a fork: which corridor (counted from the bottom) the player's hitbox is inside across the fork's columns.

2. INTERACTION per orb or pad: for an orb, CLICK (press while touching), TOUCH-NO-CLICK, or AVOID. For a pad or a gravity portal you can go around: TOUCH or AVOID. A dash orb also gets "hold length band" (short, medium, long). Orb spam on multi-activate rings is CLICK with a count band (1, 2-4, 5-16, continuous).

3. EXIT STATE at a section boundary or a pinch column, as a discrete class:
   - height band: 8 bands of the corridor;
   - vertical-speed sign: up, flat or down;
   - gravity: normal or flipped;
   - held (for ship, wave, UFO and swing);
   - onGround (for cube, ball, robot and spider).
   That is up to about 48 classes per mode, most of them impossible and masked.

Optional, a soft hint only: a click-count band per stretch (0, 1, 2, 3-5, 6-10, more than 10), used to order leg ideas and never as a hard constraint.

The discrete player state along the plan is tracked symbolically and exactly from the chosen interactions and the portals on the tape: mode, gravity, size, speed, dual. A section that needs upside-down entry therefore forces an odd number of flips before it, and the planner checks this with no physics and no AI.

### How it decides

THE MODEL (src/ai/decider_net.*): a "masked decision in-painter" over the whole tape.

Architecture:
- per-column input projection, about 300 sparse features to 64 channels;
- 10 residual, bidirectional, dilated 1-D conv layers (kernel 3, dilations 1, 2, 4 ... 512), each with a 1x1 mix and layernorm, plus one level-wide pooled vector broadcast back in. The receptive field is ±1,023 columns, so every column sees the entire level for any level up to about 61,000 units.

Extra input channels per column hold the decision state: chosen option, ruled-out options (proven or soft), "failure here" marker.

Heads:
(a) a PATH HEATMAP: per column, a 64-way distribution over height cells. This is the AI literally drawing the path. A corridor's score is the logsumexp of its cells.
(b) per orb or pad token: click / touch / avoid (plus a count band).
(c) per section boundary: exit-class logits.
(d) v2: a BLAME head. Given a failure marker at column c, per earlier decision slot: P(changing this slot fixes it).

About 200-250k parameters.

PLANNER LOOP (src/ai/planner.*; the same loop runs with a hand-written heuristic scorer when there is no net):
1. Encode the tape once. Ask the net for all marginals, about 20-60 ms (estimate).
2. Decode the plan for the next 15-30 s ahead of the real player. At each slot in x order, take the best option that is not ruled out and agrees with the symbolic state (gravity and mode parity), and write it into the decision channels. Every 10 slots, or after any change, re-run the net so later choices are conditioned on earlier ones. The net is pure C++, so it runs on a worker thread; the simulator stays on the main thread.
3. Hand the waypoint list to physics, leg by leg (next field).
4. On a leg failure, mark the option ruled out (hard if proven, soft if budget-out) and re-ask. The next option at the same slot comes first. If every option at slot i fails from the entry set, the planner backjumps: with no net, to the nearest earlier slot whose alternatives are not exhausted; with the net, to the slot the blame head ranks highest. Nogoods are kept as (entry-set id, slot, option) so nothing is retried.
5. Ordering only: the scorer never kills a state or skips an option forever. Every option is still reachable by the search, so a bad AI costs time, not solvability.

HEURISTIC SCORER (no training, the same interface src/ai/scorer.hpp):
- corridor: widest clearance first, then least vertical travel from the current band;
- orb: click if the straight path hits something within 1 s, else avoid;
- exit band: the centre of the next section's first corridor;
- for flying modes, vertical speed toward it.

### How physics makes the path

A LEG runs from waypoint i to waypoint i+1 (src/ai/leg.*). Its input is an entry set of up to K = 4-8 exact states (SimStarts, diverse in y and vy within the band) plus the constraints. Because column equals tick here, a leg has a fixed tick length.

Tier 1, cheap and today's speed: today's idea pool, reused as is.
- Sources: AbsensePathfinder::buildCandidates, buildSteeredCandidates (steer with aimOffset set to the target band's centre, which is the height-hold that cracked Zafari 2) and the beginSearch/stepSearch DFS. Each runs from each entry state through Trajectory::run / steer / useStart.
- A run is accepted only if it is alive at x_{i+1} with its hitbox inside the target band, and it obeyed the corridor and interaction constraints. The corridor checks use the per-tick TraceSample y against corridor bounds. New: a small "objects touched or activated this run" log in trajectory.cpp, so CLICK and AVOID are checkable.

Tier 2, exhaustive, only when tier 1 finds nothing: the M2 physics map (a breadth sweep over ticks) from the entry set.
- Expansion and merging: alternativesFor() expansions, with merging keyed on quantised y and yVelocity, mode, gravity, size, speed, held, onGround and orb contact.
- Pruning: states outside the chosen corridor at fork columns, or breaking an interaction constraint, are pruned as plan-violating. That is exact against the plan, never a guess about physics. Deaths are dropped exactly.
- Frontier cap: set by M0's snapshot cost.

Output: up to K surviving states in the target band, chosen far apart in y and vy, each with a witness script. They become the next leg's entry set through Trajectory::advanceStart, so the next leg has several starting options without going back. This targets Nigel's own Bloodbath observation: "a click can seemingly be open but then could make the click afterwards impossible".

Commit: the plan's script is the chosen witnesses joined end to end. Each leg's witness enters decide() as an ordinary Candidate named "plan leg i", so the unchanged trust chain applies:
- evaluate() and robust() (one tick late);
- trimStep() drops needless clicks;
- commitTick() plays real ticks;
- kDivergence = 1.0 re-plans from the real state;
- phantoms and confirmed killers;
- judge::run for disputes;
- the M3 cold confirm run before a level counts.

Kept starts and checkpoints:
- Planning runs 15-30 s ahead of the real player in the simulator only. Boundary SimStarts are kept for that uncommitted region (about 20-60 boundaries x K states), so most backjumps cost no real rewind.
- A backjump behind the real player restores the nearest real Anchor (m_anchors, every at least kAnchorEveryMin = 240 ticks) at or before the blamed slot, then captureStart() there. That is one real restore per backjump, not one per probe.
- m_starts (the last ~10 s, one per tick) still serves short repairs.

### When physics says no

Three outcomes per leg:

1. FOUND: carry on.

2. PROVEN IMPOSSIBLE: the tier-2 sweep from the full entry set ran uncapped, never hit the frontier cap, and ended with zero states in the target band. Every killer on the way must also have been static or World-certain: Candidate::killerStatic / Trajectory::Killer, and world::certaintyOf not Uncertain. Even then it is a proof only "up to merge resolution", because merging can rarely hide a survivor. So a hard nogood that would make the planner backjump more than about 10 s is first re-checked at half the quantisation. A proof is recorded as a hard nogood: (entry-set hash, slot, option).

3. BUDGET-OUT: the cap was hit, or an Uncertain killer was involved. This is a soft rule-out: the option's score drops (x0.3) and the option comes back later at 2x the cap. The planner never treats this as proof. That fixes today's failure mode, where escalation past the cap repeats cached searches: a re-try is always a different cap, a different option or a different entry set.

RE-DECIDING GLOBALLY:
(a) Other options at the failing slot come first, ordered by the scorer with the ruled-out channel set.
(b) If all of them are hard-ruled-out from this entry set, the entry set is dead. The blame moves to the decisions that produced it.
- With no net: the nearest earlier slot with untried options (chronological backjump). Symbolic shortcuts come first: if the failing section needs a different gravity, size or mode at entry, jump straight to the latest earlier slot that can change it, such as a gravity orb or an avoidable portal, skipping everything between.
- With the net: the blame head's ranking.
(c) Nogoods persist for the whole run and are saved in the level's memory file (src/absense/pathfinder/memory.* already keeps per-level JSON), so a second run never repeats them.

FURTHER BACK THAN 10 S: today's limit comes from kMaxWidenSeconds = 10 s and from how many per-tick m_starts are kept. Here going back is by decision, not by distance. A blamed slot 20-60 s earlier is reached by restoring the nearest real Anchor before it (or the start of the level) and re-planning from there. Only the legs after the blamed slot are re-run, and the cached legs before it are reused because their witnesses are deterministic.

A hard stop on churn: if more than N (say 50) backjumps land on the same failing slot, the planner marks the spot "suspect simulator" and does two things:
- plays today's "trying anyway" path into it, so the real game gives its verdict;
- logs for gucci_sim_vs_real.

### What it fixes, and what it doesn't

WHAT IT FIXES:

(1) A missing coordinated decision earlier on. Example: entering the Michigun UFO low and rising, or taking or skipping an orb 2-6 s before, which only matters later. Today's decide() picks greedily by "survives longest" over 4 s, from a fixed idea menu. It never deliberately aims for an entry state that doesn't pay off inside its window. Waypoint constraints plus tier-2 sweeps make the target explicit, and exhausting the options at a slot is exact.

(2) A fix further back than 10 s. Backjumping goes by decision, to Anchors, not by distance.

(3) Loops that repeat cached searches. Nogoods and the hard/soft split mean the same work is never done twice for the same reason.

(4) A run of clicks no single menu idea expresses. Tier 2 enumerates every alive merged state within the leg.

The evidence for (1) is suggestive, not proven. In the 2026-09-30 log, every stop from 8664 back to 7236 dies at the same object 218 at (12185, 472), "lasts N, needed N+15". That fits "the way through needs a different entry", but it fits two other causes as well.

WHAT IT DOES NOT FIX:

(a) The simulator being wrong. The same log shows "real path left the simulated one at tick 7132 (off by 0.00, 42.28)", a 42-unit miss about 1,500 ticks before the stall. If physics wrongly proves a route impossible, the planner wrongly rules it out. Only gucci_sim_vs_real and simulator fixes help, plus the "suspect simulator" escape.

(b) The -bw/-bx floor bug, untested in-game. If that was the real Bloodbath cause, the current pathfinder might already pass. B2 below tells the three causes apart.

(c) Things the tape can't express: dual mode, teleports, gameplay rotation, platformer, heavily player-triggered movers. Those sections hand back to today's decide().

(d) One-tick-wide timings. The physics still finds them, but robust() rejects them, as today.

(e) Frontier blow-up in long open sections such as wave spam. That is capped and therefore not a proof. The AI only helps by ordering what is kept.

THE TRAINED PART: none of (1)-(4) needs the trained AI. They come from the decide-then-verify loop, which runs on the heuristic scorer. The AI makes the right option come up first more often. That is the "faster" Nigel was sceptical of, but here it is faster at the level of route decisions across the whole level, not at the level of single dots.

### Training

DATA: every source is generated by GucciBot itself on Nigel's PC. There are no other people's macros, and the human-macro candidates in src/absense/pathfinder/human.* are off during data runs.

1. Overnight planner runs (heuristic scorer; later rounds use the current net):
   - which levels: RobTop's non-platformer levels, plus easy-to-hard online levels Nigel approves;
   - held out, never trained on: Zafari 2, Xmas Challenge, Bloodbath, and the 20-level test list;
   - honest caveat: training on RobTop's levels means the "every RobTop level" part of the 2.0 bar is not a test of generalisation.
   Labels per decision slot:
   - positive: the option is on the solved path, or survived k = 3 sections onward in a "label harvest" pass that tries each alternative at each slot of the solved path with a bounded leg plus look-ahead;
   - negative: proven impossible (hard nogood);
   - unknown: budget-out, masked from the loss.
   Blame labels come from backjumps that eventually succeeded: the slot changed and the failure column.

2. Synthetic levels from our own generator (src/ai/levelgen.*): GD level strings built from per-mode templates (spike rows, corridors, orb chains with decoys, gravity choices, speed portals). Some forks are deliberate long-range traps, where the wrong branch dies only 1-5 s later. They load through GD's own level loading. Labels come the same way, by planner plus physics. This is the bulk of the volume. Unsure: that unattended loading of a generated level string works; and that synthetic levels look enough like real ones.

3. Composite levels, which need no physics: each section's feasibility table (entry class -> option -> reachable exit classes, from sweeps at 2-3 representative states per class) is stored. New training levels are built by chaining labelled sections whose mode and speed match at the joint. Exact "on some solution" labels come from forward and backward reachability over the class graph. This is cheap augmentation, approximate because the class abstraction loses detail.

Further augmentation: vertical shifts, plus gravity mirroring where the floor allows it.

HOW MUCH: target about 200k labelled decision slots for a 200-250k-parameter net, which is roughly 500-1,000 level-equivalents. My estimate is that one GD process covers about 50-150 short synthetic levels, or a handful of long real levels, per 8-hour night. So round one is about 4-10 nights, fewer if two GD processes can run at once (M0 checks that). It is more than design A's per-dot net needs, because a level gives about 1,000 labels here against millions of dot labels.

TRAINER: tools/ai_train/, plain C++ built with the repo's MSVC toolchain like run_tests.bat (/O2 /MD /arch:AVX2, 12 threads, hand-written backprop and Adam). Its parts are conv, MLP and layernorm only, chosen to keep the backprop small; no attention, no recurrent layers.
- Data: about 1,000 tapes x about 1 MB, which fits easily in 32 GB.
- Cost: about 5x10^11 multiply-adds per epoch (forward plus backward). At a realistic 20-50 GFLOP/s for hand-written code, that is about 20-60 s per epoch. At 100-200 epochs, that is 1-3 hours per round. These are estimates.
- Checks: gradient checks in run_tests.bat, plus a golden test (the mod's inference equals the trainer's forward pass to 1e-5).
- Training samples reveal random subsets of the true decisions and random true ruled-outs, so the net learns to use those channels.

GENERALISATION: the convolutions are translation-invariant and the inputs are local geometry plus game state, so common patterns transfer (spike rows, corridors, orb chains, mode physics). Long-range coupling, the case that matters for stuck spots, is rare in data and the hardest thing to learn. The synthetic long-range traps are there to teach it, but transfer to human-made traps is unproven. It is measured on the held-out set as:
- top-1 decision accuracy;
- refutations per level;
- wall time against the heuristic scorer.

LEARNED VS NO-TRAINING PLANNER: the planner, legs, nogoods, backjumping and symbolic state are identical; only the scorer changes.

What learning adds:
(a) better first picks where hand rules are wrong (trap corridors that look wide, decoy orbs, exit bands tuned to the next jump);
(b) blame-directed backjumps instead of chronological ones;
(c) a whole-level path heatmap that is useful as an overlay.

What it does not add: any route the planner couldn't otherwise find. It is worth it only if the heuristic planner, measured in B3, wastes most of its time on refuted options, or misses the time budget on more than about 30% of the breadth set. If the heuristic planner meets the bar, skip training for 2.0.

### Size and speed

SHIPS IN THE MOD:
- resources/ai/decider.gbnet: about 0.4 MB as fp16, or about 0.8 MB as fp32, for 200-250k parameters;
- inference code: about 50-100 KB;
- the trainer and the level generator's batch tools never ship. The generator's in-mod loader is debug-only.
That adds well under 1% to the roughly 233 MB download.

AT RUNTIME, PER LEVEL (estimates; M0 sets the real numbers):
- Survey pass: one noclip play-through. Bloodbath is about 110 s at 1x, perhaps 10-60 s stepped faster (unsure).
- Tape build: well under 1 s.
- Net ask: about 220M multiply-adds for Bloodbath's ~1,312 columns, so about 20-60 ms on one core with AVX2. It runs on a worker thread. Expect 300-2,000 asks per level, which is under 2 minutes of background CPU in the worst case.
- Physics: dominates.
  - Easy stretches cost what today's decide() costs, because tier 1 is today's idea pool.
  - Each tier-2 sweep is roughly seconds to a minute, depending on sim speed and snapshot cost. The 2026-09-30 Bloodbath log shows about 90,900 simulations in 219 s, about 415 runs/s; ticks per run vary, so ticks/s is unmeasured.
  - Goal: Zafari 2 no slower than today's 170-450 s solves. The guarantee comes from tier 1 first, plus a fallback to today's decide() when a leg isn't ready in time.
  - Bloodbath from the start: unknown until B2.
- Memory: about 20-60 kept boundary SimStarts x K = 4-8 for the 15-30 s planning lead, plus tier-2 frontiers. Snapshot size is unmeasured, so this could be tens to hundreds of MB.

### Fit with the existing code

REUSED UNCHANGED:
- Trajectory (src/absense/trajectory/trajectory.hpp/.cpp): run(), steer(aimOffset), beginSearch/stepSearch/finishSearch, captureStart/useStart/advanceStart, StartState, TraceSample, Killer, and alternativesFor() for the M2 sweep.
- World trigger model (src/absense/world/*) and world::certaintyOf for proof strength.
- The whole trust chain in src/absense/pathfinder/pathfinder.cpp: Candidate, evaluate(), robust(), trimStep(), commitTick(), kDivergence re-plan, tabu, confirmed killers, phantoms, the RunMemo determinism check (Stats::unrepeatable).
- m_anchors (real restore points) and m_starts (the last 10 s of SimStarts).
- judge::run (src/absense/judge.*).
- MCP automation: gucci_open_level, gucci_restart_game, gucci_sim_vs_real, gucci_abs_simulate, gucci_tick_probe.
- memory::Store per-level JSON (src/absense/pathfinder/memory.*) to keep nogoods.
- GucciBot's noclip for the survey.

MOVED: classify() and changesEverything() from the anonymous namespace in src/absense/gpu/client.cpp into a shared src/ai/level_data.*. This is plan M1, and sendLevel() calls the shared copy.

PLAN STEPS IT DEPENDS ON:
- M0: measure sim speed and snapshot cost.
- M2: the physics map; this design's tier-2 leg is M2 with plan constraints.
- M3: overnight harness, the restart-after-replay and judge step-flag fixes, cold confirm.

NEW FILES:
- src/ai/tape.* (whole-level tape, sections, slots, overlay)
- src/ai/survey.* (noclip survey)
- src/ai/planner.* (waypoints, symbolic state, nogoods, backjump)
- src/ai/leg.* (tier 1 via the pathfinder's idea builders; tier 2 via M2's sweep)
- src/ai/scorer.hpp (interface), src/ai/heuristic_scorer.*, src/ai/decider_net.* (plain C++ inference with an AVX2 path picked by CPUID and a scalar fallback that accumulates in the same order, so it stays deterministic)
- src/ai/levelgen.*
- tools/ai_train/
- resources/ai/decider.gbnet

A small addition to trajectory.cpp: a per-run "objects touched or activated" log for interaction constraints.

INTEGRATION:
- A new Decision::Stage "Plan" ahead of Steers in decide(). Plan legs enter as Candidates named "plan leg i".
- beginRepair()/escalate() gain a "plan backjump" path that sets the target to the Anchor before the blamed slot instead of stepping by backDistance().
- A third engine pill in the Pathfinder tab, next to "Absense (look-ahead)" and "Classic": "Planner", with a scorer switch Heuristic / AI.
- A missing or mismatched .gbnet gives the heuristic scorer.
- Windows-only at first, like the pathfinder and the World.

### Build steps

- **B0 (= plan M0): measure**: Logged sim ticks/s, SimSnapshot/SimStart save+restore cost and size; a speed-test button; a check whether a second GD process can run. *Nigel tests:* Run the pathfinder on Zafari 2 for a minute, press the speed test, try opening GD twice. Real numbers decide the frontier cap, the planning lead and K.
- **B1: the whole-level tape**: Shared classify(), the noclip survey, the tape with sections, forks, orb/pad slots and exit slots, and an overlay that draws it over the level; export to a .gblevel file. *Nigel tests:* In Zafari 2, Xmas Challenge and Bloodbath: do the boxes sit on the real hitboxes, including moving ones? Are the forks and orbs marked where a human would see a choice?
- **B2: constrained legs and the Michigun test (needs plan M2's physics map)**: Tier-1 and tier-2 legs with corridor, band and interaction constraints; a debug panel that, at a chosen stuck spot, enumerates the option combinations at the 3-5 slots before it and says for each: found, proven impossible, or budget-out. *Nigel tests:* Bloodbath at the Michigun UFO (death tick ~8676, x ~12,185). If some combination of earlier choices gets past in the sim and in the real game, Nigel's 'it's the decisions' theory is confirmed. If all are proven impossible, or the sim and real game disagree, the cause is elsewhere (simulator accuracy or the -bw/-bx floor fix). This answer decides the rest.
- **B3: the no-training whole-level planner**: Planner engine pill with the heuristic scorer: waypoint plan, symbolic gravity/mode/size tracking, nogoods, decision-based backjumps to Anchors past 10 s, the 'suspect simulator' escape, and logs of refutations and backjumps per level. *Nigel tests:* Zafari 2 and Xmas from the start: no slower than today's engine. Bloodbath from the start. The log's refutation counts decide whether training is worth it.
- **B4 (with plan M3): overnight harness and data**: The two bug fixes, the cold confirm run, an unattended level-list runner with a watchdog, label harvesting, the synthetic level generator and loader, and section feasibility tables written to example files. *Nigel tests:* One night unattended on RobTop's levels plus generated ones; in the morning, a solved/failed report and the example files exist.
- **B5: the first trained route drawer**: The C++ trainer with gradient checks and a golden test; decider.gbnet v1; a path-heatmap overlay (the AI's drawn band across the whole level). *Nigel tests:* On held-out levels he hasn't trained on: does the drawn band go where he would go? Offline numbers: top-1 decision accuracy on held-out levels.
- **B6: AI-ordered planning**: Scorer switch Heuristic/AI; the AI orders options and (v2) blames backjump targets. *Nigel tests:* A/B on Zafari 2, Xmas and Bloodbath, 3 runs each: fewer refutations and no slower with AI on. If not, it ships off.
- **B7: more rounds and breadth**: Self-play rounds (planner plus net solves make more data) and harder levels. *Nigel tests:* Overnight runs on all RobTop non-platformer levels and the 20-level held-out list: the 2.0 bar.
- **B8: release**: about.md, README, credits, site. *Nigel tests:* Nigel's release test.

### Risks

- The trained AI does not fix stuck spots on its own. The planner loop (waypoints, proofs, nogoods, backjumps) does. If expectations rest on the AI, B6 may disappoint even when B3 succeeds.
- Simulator inaccuracy turns into false proofs. The 2026-09-30 Bloodbath log shows a 42-unit sim/real miss at tick 7132. A wrong 'proven impossible' crosses off the right route. Mitigations: certainty gating, a finer re-check before deep backjumps, the 'suspect simulator' escape. None of them is complete.
- Data hunger: about 1,000 labels per level against millions of per-dot labels for design A. Round one is several nights, more if only one GD process can run.
- Synthetic-to-real gap: the generator's traps may not look like human level design, so the long-range skill that matters most may not transfer.
- The exit-class abstraction is lossy. Two states in the same class can differ in what is reachable next, which makes composite-level labels and the class-level reasoning approximate. This is only safe because the scorer only orders.
- Frontier blow-up in tier-2 sweeps on open sections (wave spam, free cube areas). Capped sweeps are budget-outs, not proofs, so those spots can still churn, just not on identical work.
- The survey and tape misread player-triggered movers, so wrong forks or orbs get offered. That costs time, not correctness, but heavy-trigger levels may get little value.
- Hand-written backprop in C++ is easy to get subtly wrong. Gradient checks and the golden test are mandatory.
- Determinism: AVX2 and scalar inference must give identical choices, or the same level plans differently on different PCs.
- Memory: boundary SimStarts times K times a 15-30 s planning lead, at an unmeasured snapshot size.
- Scope: this is a new engine (tape, planner, legs, generator, trainer). Expect weeks, paced by usage and by Nigel's tests.

### Unsure

- Whether Bloodbath's Michigun UFO stall is a decision problem at all. It could instead be simulator inaccuracy, or the -bw/-bx floor/horizon bug that was fixed but never tested in-game. B2 is designed to tell these apart.
- Sim ticks/s and SimSnapshot cost and size: unmeasured. The about 415 runs/s from the 2026-09-30 log does not give ticks/s.
- Whether x truly maps to a tick independently of input across all 2.2 objects (I believe it holds for classic levels apart from speed changes, teleports, gameplay rotation and dual, but have not verified it in code).
- How fast a noclip survey pass can be stepped, and how many movers in real levels depend on the player's y or input.
- How to place the 64-cell vertical window in ground-mode sections. The 'walkable surface track' rule is a guess.
- Whether GD can load our generated level strings unattended through its own loading, and whether two GD processes can run at once for data generation.
- Whether a 200-250k parameter conv net trained on about 1,000 level-equivalents learns long-range coupling well enough to beat the hand-written scorer. Also, whether the blame head gets enough examples to be useful.
- The number of decision slots per level (200-800) and nights per round (4-10) are estimates, not counts.
- Whether 'never slower than today' holds on Zafari 2 once the survey and planning lead are added.
