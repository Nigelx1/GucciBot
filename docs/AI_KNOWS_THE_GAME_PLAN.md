# The Route Map: an AI pathfinder that sees the whole level (the plan)

2026-10-24. This replaces the "level brief" version of this file from earlier today, which Nigel turned down. Part 1 is for Nigel. Part 2 is the build plan. None of it is built yet. The finished pieces sitting on stopped branches land first, in step M0.

## Part 1: For Nigel

**What it is.** It's a GPS for a level. Before it plays, the pathfinder looks at the whole level at once and plans a route from start to finish. Then GD's own physics turns that route into exact clicks and checks them. Nothing is trained, nothing is remembered between runs, no macro is used, and nothing is written for one level or for one kind of object.

**How it works.**
1. **The map.** One hidden copy of the player runs through the whole level first, without clicking, to see where everything is at every moment, moving blocks included. The map is every place the player could be at each 1/60 of a second: height, rising or falling and how fast, mode, gravity, size and speed. Places inside blocks or spikes are left out.
2. **The roads.** A road is what a click, a hold or a release does from a place. How each mode moves through open air is measured from GD itself on every run. For anything you touch (the ground, a slope, an orb, a pad, a portal), the map asks GD's own code what happens. No object gets a rule. A blue pad, a fake orb and a trap portal are all handled the same way: ask the game.
3. **The route.** It works backwards from the end of the level and marks every place that still has a way to finish. Then it picks the safest route through those places: wide gaps and few clicks.
4. **The clicks.** The map is only a sketch. GD's real physics drives the route on hidden copies and finds the exact clicks. The map just tells it which way to try first. Wherever the sketch was wrong, the physics finds out, that road is marked dead, and the whole level is planned again.
5. **The check.** The real game plays the route quickly without drawing it. Deaths are noted instead of happening, safe mode stays on, nothing is saved, and afterwards the game is put back. One pass shows every spot where the route fails. Then the route is played for real and recorded, and the result still has to replay cold.
6. **The repair.** A death in the check becomes a dead road on the map, explained in game terms. For example: "died at x 12185 on a ceiling spike in the UFO, entered right side up; the ball's last click decided that". The whole level is planned again, so the fix can start well before the death.

**Your examples, with no rules:**
- **A fake orb or trap portal.** GD says where touching it takes you, and from there nothing reaches the end. So the route never touches it.
- **The bottom path that dead-ends 30 seconds later.** Working back from the end, nothing on the bottom path has a way to finish, all the way back to the fork. So the route clicks onto the top path at the fork, even before the top path is on screen.
- **The UFO corridor whose top shrinks.** High places near the narrow end can't get down in time, so they have no way to finish, and neither do the high ways in. The route goes in low.
- **Bloodbath's UFO, where every cold run dies today.** The ball's clicks set your gravity just before the UFO portal. If no right-side-up UFO place gets past the ceiling spike at x 12185, the ball's last click is chosen to go in upside down, about 2 seconds before the spike, without dying first. I'm not sure upside down is the only way through. If it isn't, the map will show the other way.

**A correction to what I told you.** The map is a few million places, not a few hundred thousand. Building it takes about 10 to 60 seconds on a normal level and a few minutes on LIMBO or ORBIT, not a fraction of a second. A whole normal level should take about 3 to 8 minutes, close to what passing runs take today. I'm not sure of these numbers until Nov 5, when it's built and measured.

**What it builds on.** These all stay: GD's physics on hidden copies of the player, the trigger model, the rewinds, the recorder and the cold-replay check. The fixes that were stopped on Oct 24 land first and help today's pathfinder too: the UFO fix, the drift check, and reading where objects really are. Today's pathfinder stays the default, behind a switch (Today / Route map), until all 8 bar levels pass on the new one.

**LIMBO, ORBIT and Heliopolis.** The map is only as good as the simulator, and on those three levels the simulator is slow and often wrong today. On LIMBO it missed 22 of 32 deaths. So a second track runs alongside from day one:
- count what each of those levels uses;
- record how the real game really moves things, with a no-click pass where deaths are noted, safe mode on;
- give those recordings to the simulator;
- check routes on those levels on the real game.

Your macros for those levels are used only the way you already allowed: as a yardstick for where the simulator goes wrong, never to steer a run.

**What you'll see.** Every in-game test waits for your OK. Code and offline tests don't need your PC.

| By | What lands | What you'd see |
|---|---|---|
| Mon Oct 26 | The UFO fix and the shared fixes (today's pathfinder gets them too) | In practice at Bloodbath's and ToE2's UFOs, "Find a way" draws a way past the spike |
| Thu Oct 29 | The heavy three measured | A table of where the simulator and the game disagree on LIMBO, ORBIT and Heliopolis |
| Sun Nov 1 | GD's physics drives a whole route | "Plan the level" draws a full route on Clubstep, ToE2, The Nightmare and Bloodbath, without playing |
| Thu Nov 5 | The map | "Show the route map": the planned route, plus every place that can still finish. Your three examples become automatic tests |
| Mon Nov 9 | Check and repair | The new pathfinder beats Clubstep, ToE2, The Nightmare and Bloodbath cold |
| Fri Nov 13 | Switches and toggles; new levels | Deadlocked, and your ~10 new levels overnight |
| **Sun Nov 15** | **Checkpoint** | A report. You decide: ship it marked experimental, or hold 2.0 |
| Sat Nov 21 | The heavy three | LIMBO, ORBIT and Heliopolis on the new pathfinder |
| Tue Nov 24 | The bar | All 8 run unattended; you pick the default |
| **Thu Nov 26** | **2.0** | After your in-game test |

**Honest risks.**
- LIMBO, ORBIT and Heliopolis are the most likely to miss Thanksgiving. On Nov 15 you'll get measured numbers for them, not a promise.
- The map is a simplified sketch and may be off on slopes or the spider. GD's physics always has the last word, so a wrong sketch costs time, never a bad macro.
- If the UFO fix doesn't make the hidden copies fly, everything after it slips. The Oct 26 test settles that.
- This is about 7,000 new lines in about four weeks, and the tests need your PC.

**What I need from you.**
1. A yes on this plan.
2. A yes on the check in step 5: the real game plays the route quickly with deaths noted, then is put back. Safe mode stays on and nothing is saved. Without it, every failure costs a real death and a rewind, which is much slower on the heavy levels.
3. Tell me when I can use the game for tests. Nothing runs until you say.
4. About 10 levels it has never seen, covering every mode, by around Nov 10.

---

## Part 2: Build plan (for the engineer)

Line numbers are at master 9789add (build 2026-10-24-a). If they've moved, search by function name.

### 2.1 Constraints (Nigel's, all standing)

- **No macros, no memory.**
  - No macro ever steers a search.
  - Remember solutions (`pf_remember_solutions`) stays off for every measured run. "Cold" means no macro and no memory.
  - Nigel's macros are only a yardstick for simulator mismatches (memory feedback_safe_mode_and_no_macros).
- **No rules for levels or objects.** No level-specific fixes, and no rules per object ("be basic"). What an object does is asked of GD's own code.
- **No training as the core.**
- **Physics and cold replay.** GD's exact physics has the last word. A result counts only after a cold replay from a fresh open.
- **Speed and stopping.** Slower is fine if the hard levels reach 100%. Stop after 5 minutes without progress (kStuckSeconds).
- **Calm inputs** are a cost, never a rule.
- **Platform.** A Windows-only engine is fine (GB_NATIVE_ENGINE).
- **A switch.** The new engine stays behind a switch, with Today as the default, until all 8 bar levels pass: Clubstep, ToE2, Deadlocked, The Nightmare, Bloodbath, LIMBO, ORBIT and Heliopolis. All 8 must reach 100% for 2.0.
- **Safety and the PC.**
  - Safe mode is on for every automated run; the batch scripts refuse to run without it.
  - Nothing runs in-game until Nigel says so. On 10-24 he said: "dont take control of my computer just yet".
- **Deadline.** 2.0 by Thu Nov 26, with a checkpoint on Sun Nov 15 (ROAD_TO_2_0, Release section).

### 2.2 Facts the plan rests on

**The cold baseline** (-ht/-24a, no macro, no memory):

| Level | Result | Detail |
|---|---|---|
| Clubstep | 100% | |
| ToE2 | 18.4% | x 5340, tick 4002. Killers obj 720 at (5369, 327) and (5369, 246), about 550 ticks into a UFO entered from a ship |
| The Nightmare | 90.5% | x 25740. 213 dead ends at tick 19832 on obj 186 (25755, 225). 100 identical partings at tick 19698: real y 502.1, vy 5.7, right side up; simulated y 507.0, vy 13.3, upside down; each 8 ticks after a rewind to 19688 |
| Bloodbath | 30.9% | Ceiling thorn obj 218 at (12185, 472), uid 7382, tick 8675. The UFO portal is at x 11715 (tick ~8328) after a ball section (x 10995-11715). All 4 cold normal-engine runs went in right side up. The cold Route drives pass went in upside down, died first at obj 216 (11789, 434) at tick 8370, and passed after a local repair |
| Deadlocked | Not finished | GD crashed at 71% (fixed in -24a), not rerun |
| LIMBO (-hp) | 14% | |
| ORBIT (-hp) | 3% | |
| Heliopolis (-hp) | 13% | |

**Code facts checked at 9789add:**
1. **The search can't flap a UFO.**
   - `alternativesFor` (trajectory.cpp 3393-3429) offers the UFO only `{1,false}` (plus `{2,true}` thenRelease on a ring), and it returns before the held branches.
   - `Sim::step`'s `apply` (2769-2794) presses and releases before the update.
   - The UFO flap in updateJump needs `m_stateRingJump && m_jumpBuffered` at the update (physics/player.cpp 609-611). It also reads `m_isOnSlope`, `m_wasOnSlope` and `m_slopeVelocity` (627-637).
   - On step1-inputs, 380bb50 makes legs play thenRelease units, but its `alternativesFor` still has no UFO press held through the tick.
   - Legs that start inside a UFO log exactly 4n−2 steps.
2. **The drift check is muted after decisions made ahead.** `m_lastReplanTick = tick` is set at 4658 and checked at 5428. step2-trust ea7e4c4 fixes this.
3. **The atlas reads sprite positions** (atlas.cpp 318-319, 336-337 and nearby), and GD's enter effects slide sprites. step3-brief 4d3c664 and be0d881 fix this.
4. **LegState (4403-4539) is already a per-tick beam over exact SimSnapshots.** It has:
   - a merge key, `keyOf` (4478): y 0.25, vy 0.05, mode, gravity, ground, held, mini, dual, on-orb, goal fired, and speed;
   - class shares under a cap (4775-4800);
   - pause and resume across slices (4532, 4631-4640);
   - a deterministic step budget.
5. **A session that begins from the live game has the exact object table.** `Sim::begin` calls `world::baseTable` (2428) when its first run comes while the game stands at its tick. Kept starts on LIMBO, ORBIT and Heliopolis never get a table (ledger coveredNow, and lastUncovered after every rewind).
6. **SimSnapshot (1256-1306) copies by value:** activated1/2, ringTouch, ringUsed, activatedIDs, actions, WorldState and the OpLog. The OpLog's sealed chunks are shared by pointer, but the pointer vector grows with the session. So snapshot cost grows over a long session.
7. **The ghost** (`beginGhost`/`stepGhost`, 4931-5076):
   - it is a no-input World run that forces speed portals (5037-5044), ignores hits (5045-5049) and calls `setAllRows(true)` (4974);
   - it never reported back on LIMBO or ORBIT on 10-23. The cause is unknown.
8. **judge::run (judge.cpp 67-197):**
   - it plays a script on the real game synchronously, without drawing, and queues inputs the way applyInput does;
   - it stops at the first death (157-158), and noteDeath keeps only the first death (37-42);
   - it puts the game back with `createCheckpoint` and `resetWithState` (94-100, 174-176);
   - while it runs, `destroyPlayer` notes the real player's death and returns (hook_playlayer.cpp 687-690);
   - nothing in the pathfinder calls it yet.
9. **The Materializer.**
   - `Materializer::write` (materialize.cpp 1180-1202) writes position, node position, last position, move marker, dirty words, group counter and the disabled flag. It writes no rotation and no scale.
   - `startPending` (1391-1395) takes the full write-back whenever `m_fy > 0`. ORBIT logs a y section factor of 0.01 (44.9 s spent there), and Heliopolis very likely does too (105.5 s).
10. **What the World runs.**
    - It runs 29 trigger kinds (def.cpp `kindShipped` 194).
    - It does not run rotate, follow, follow player Y, scale, collision, reset, advanced follow, keyframes, area and enter effects, or the camera. Their groups are Uncertain (`certaintyOf`, certainty.cpp 82).
    - The flying floor and ceiling read the camera (tierd.cpp fact (i)), which is frozen at a run's start.
11. **The switch parameter.** gucci_pathfinder's `engine` parameter already picks Absense or Classic. step2-trust 18f2d17 adds `brain` (today | understanding) and `model/engine.hpp`.
12. **Throughput:**
    - Real runs: 100-324k simulated ticks/s on normal levels, 42.5k on LIMBO, 48.4k on Heliopolis, 10.3k on ORBIT.
    - Leg searches: about 100k steps/s on normal levels, 51k on LIMBO, 36k on Heliopolis, 14k on ORBIT.
    - Stepping the real game without drawing: about 30-55 µs a tick on normal levels, 0.33 ms on Heliopolis, 0.79 ms on LIMBO and 1.07 ms on ORBIT. These come from log totals and are not verified per tick; H0 measures them.

**Stopped branches** (not on master):
- step1-inputs: 629920b, 6ef9938, 380bb50, cb9fc9b, plus uncommitted lineage-parity work in its worktree.
- step2-trust: ea7e4c4, 18f2d17.
- step3-brief: 4d3c664, be0d881, 8dcfc5e, d3da645, b1ebd51. The rulebook draft (d4850c1, b5b6ab3) and b64bbf0 are also on it; this plan does not land those.

### 2.3 Architecture

```
fresh open, game frozen at tick 1 (Phase::Planning; 33 ms slices keep the frame drawing)
 1 SCAN      one no-input copy from tick 1 (the ghost, grown): clock x(t), mover poses,
             each portal's room asked of GD, camera, snapshots every 0.5 s
 2 TIMELINE  per tick: blocked y-intervals, surfaces, room, contact objects
             (statics from the atlas object read at m_positionX/Y; movers from scan or recording)
 3 MOTION    open-air motion per mode/size/gravity/speed/warp/TPS, measured with GD's update, checked
 4 MAP       places + roads; contacts asked of GD (cached); forward sweep, backward sweep
             -> cost to the end of every place; live corridor; best route per class
 5 DRIVE     exact A* on GD's physics in one Sim session from tick 1; h = the map's cost to go;
             produces the exact inputs; sim evidence (dead places) flows back into the map
 6 REHEARSE  the real game plays the driven route without drawing, deaths noted, game put back
             -> game evidence -> map re-plan -> drive -> rehearse from the first changed step
Phase::Playing: the clean route through the commit path (recorded), drift check every tick
 -> saveResult -> cold replay from a fresh open
```

Principles:
- **Physics decides.** The map orders the work. It never forbids a road: a place with no way home is a soft wall that the drive defers, never drops. A wrong map costs time, never a wrong macro.
- **Evidence classes:**
  - `map`: geometry and measured motion;
  - `sim`: an exact copy killed by a Static or Modelled killer, in a trusted span;
  - `proof`: the exact rung, in a trusted span;
  - `game`: a rehearsal or a real death.

  In untrusted spans, `sim` evidence kills nothing.
- **One interface.** pathfinder.cpp includes only `routemap/engine.hpp` (step2-trust's `model/engine.hpp`, renamed). Every engine decision logs a `routemap:` line, so a path that is never reached shows up as a missing line. This is the half-port check.
- **Determinism.** Budgets are counted in simulated steps, costs are integers, orderings are stable, and the clock is only a safety stop.
- **Handoffs.** These go to Today and are logged: platformer, two-player with separate controls, reverse (1917) and rotate gameplay (2900).

A **step** is 1/60 s: 4 ticks at 240 TPS, or TPS/60 ticks in general. The map and the drive both use steps as their layers. The atlas's 30-unit "columns" are a different thing.

### 2.4 The scan (`routemap/scan.*`, grown from beginGhost/stepGhost)

- **Start.** It begins at tick 1 of a fresh open, before anything else runs, from the live game, so it has the exact table. "Plan from here" in practice begins from the live state instead. While planning, the game is frozen and the scan has the simulator to itself.
- **What it does.** It takes no input, ignores hits, and forces each speed portal at its x (the existing code).
- **What it records each tick:** x, player speed, time warp, the gravity mod, and the camera fields getMin/MaxPortalY read.
- **Rooms.**
  - At each mode, size, gravity, dual or solo portal it branches: restore the scan snapshot a few ticks before the portal, put the copy at the portal's center height, step through, then read the PortalState and the floor and ceiling.
  - This is asked of GD, not computed. b64bbf0's numbers appear only as a cross-check in the log.
- **Movers.** Each tick it records every collidable World slot within reach of x(t) (slot, m_positionX/Y, enabled). Rotation and scale come from the recording once H2 lands. Clubstep, ToE2, The Nightmare and Bloodbath read 0 movers at tick 1, so they cost nothing.
- **Snapshots** every 0.5 s, as restart points for contact answers.
- **Clock forks.** A speed portal that doesn't span the corridor is logged ("clock may fork at x"). The map uses the scan's clock, and the drive is exact regardless.
- **Profiling.** Each part is timed (gucci_abs_profile). H1 finds why the ghost stalled on LIMBO and ORBIT. The candidates are `setAllRows(true)`, the per-slice resume cost, the 120 s cap, or a refusal while a search was active. I'm not sure which.

### 2.5 The corridor timeline (`routemap/timeline.*`)

Each tick fixes x, so every obstacle at that tick becomes a y-interval for the player's center. The timeline is kept per hitbox class: normal, mini, and the wave's if GD's differs. The sizes are read from the copy with getObjectRect, getObjectRect(0.3,0.3) and getOrientedBox after setFakeMode, never assumed.

Each tick has:
- **hazard intervals:** the outer box, the oriented box where `m_shouldUseOuterOb` is set, and circles for saws, the way `phys::collisionCheckObjects` tests them;
- **inner-solid intervals:** a side hit or being crushed, which is death;
- **surfaces:** the tops and bottoms of solids, and slope lines;
- **the room's floor and ceiling;**
- **contact intervals,** each with its uid: orbs, pads, portals, slopes and touchable triggers.

Where the data comes from:
- Statics come from the atlas's object read (`gpu::classifyObject`) at m_positionX/Y (4d3c664), in an x-sorted index. They are computed per tick when asked for, not stored.
- Movers come from the scan, or from a recording (H2).

To stay optimistic:
- Against hazards, the player's footprint is a circle inside the hitbox at every rotation.
- Objects nobody can vouch for don't block; they add doubt cost. These are groups moved by a route-dependent trigger, or by a kind that nothing models or records.

The timeline takes a few MB per level.

### 2.6 Measured motion and contact answers (`routemap/motion.*`, `routemap/contacts.*`)

**Motion tables.**
- **What is measured.** A table is made for each combination the scan sees of mode, mini, gravity, speed, gravity mod, time warp and TPS. A hidden copy in open air runs GD's own `PlayerObject::update` with no collision pass, over:
  - vertical speed on a 0.01 grid;
  - the button state (up, down, and the buffered and ring flags as GD reads them);
  - this tick's input (nothing, press, release, re-press, or a tap inside the tick).
- **What is stored:** vy′, dy and dx. Each combination takes about 10-20 ms. Tables are measured on every launch, so nothing carries over.
- **The key** is what updateJump reads (physics/player.cpp 516-799): yVelocity, upside down, gravity mod, vehicle size, speed and speed multiplier, jumpBuffered, stateRingJump, isAccelerating, onGround, dash and robot boost. Slope and dash ticks are contacts, not table entries.
- **The input check**, per mode and situation (ground, air, on an orb): each input kind is compared with doing nothing over 8 ticks. A kind that changes nothing is not a road. It is logged once, for example `routemap: motion UFO normal 1x 240: a tap inside the tick does nothing; a one-tick press flaps`.
- **The self-check:** three 240-tick random scripts composed from the table must match GD's update within 0.05 units. If they don't, the mode is logged as drifting, and the map uses only exact answers there, staying optimistic.

**Contacts (no object rules).**
- **Generic surface behaviors** are measured once per mode, size and gravity on the level's own blocks during the scan, then reused anywhere by translation and covered by the self-check:
  - landing (stand: snap to the surface, vy 0, on the ground);
  - sliding along a floor or ceiling in the flying modes;
  - a side hit, which is death.
- **Everything else is asked of GD for each object:**
  - orbs of every kind, pads, and gravity, mode, size, speed, dual and teleport portals;
  - dash orbs, toggle orbs, touch triggers and slopes;
  - the ball's and the spider's presses from a surface. The spider's press is a raycast through the level's geometry in GD.
- **How an answer is made:**
  1. Take the scan snapshot nearest before the contact.
  2. Step the scan copy in free air to a few ticks before the contact.
  3. Overwrite its y, vy, mode (setFakeMode), gravity, size and button.
  4. GD's code plays the contact with the road's input, using the copies' phys:: versions (hooks.cpp).
  5. The state a few ticks later is the answer, cached per (uid, entry place, road).
- **Exact answers win.** When the drive reaches a contact from an exact state, its answer replaces the set-up one. If the two disagree, that object takes answers only from exact states from then on. Until the drive gets there, the map treats the object optimistically: touching it may lead anywhere in the room. This is the riskiest new code, and it is only ever a first guess.

### 2.7 The map (`routemap/places.*`, `routemap/sweep.*`)

**Places.**
- **The key:** step, y band (2 units), vy band (0.5), mode, mini, gravity, speed, held, plus a flags byte: the standing surface's id, buffered, dash and robot boost. A standing place exists once per surface per step. The bands are dials.
- **Storage:** each step keeps a hash from key to index. A place takes 8-12 bytes: its cost from the start, its cost to the end, the road it came by, and flags (dead by map, sim, proof or game; untrusted; exact-only).

**Roads.**
- **Kinds:**
  - keep;
  - press at k;
  - release at k;
  - a one-tick press at k (press at k, release at k+1; the release can fall in the next step);
  - a re-press while held;
  - orb spam at a cadence of 2-4 while on a multi-activate orb.

  Only the kinds the input check kept are used: at most 11, typically 1-8.
- **Stamping.** A road starts from the place's corners and center and goes tick by tick:
  - motion comes from the open-air table;
  - a hazard or inner-solid interval kills that sample;
  - a surface is a contact (stand or slide);
  - a contact interval uses its contact answer.

  The road lands in every place a surviving sample lands in, so the map stays optimistic.
- **Costs** are integers:
  - 1 per press;
  - clearance under about 6 units, per tick;
  - timing width: a road whose neighbors 1-2 ticks earlier or later die costs more, read from its sibling roads;
  - doubt: near Uncertain killers or objects nobody vouches for.

**Sweeps.**
- **Forward,** from the start place in step order. x only increases outside reverse and backward-teleport stretches, so this is one pass over a layered graph with no queue. It gives each place its cost from the start.
- **Backward,** from the level's end. It gives each place its cost to the end; infinite means no way home. Places that are finite both ways form the live corridor.
- **The route** takes, step by step, the least cost so far plus cost to go. The best route of each class is also kept at the big choices: gravity and mode at each mode portal, each side of a fork, and each orb used or skipped.
- **Incremental re-plans.** When places at step s die, the backward sweep re-runs from s toward the start until nothing changes (the LPA* idea). That takes seconds, not a rebuild.
- **If a step holds too many places:** coarser bands where the clearance is large, 8-tick steps, or a beam with class shares (stepLeg's pattern at 4775-4797).
- **Explanations** compare the costs per class at each choice. For example: `routemap: UFO x 11715: right side up - no way home (first blocked x 12185: obj 218 uid 7382 static); upside down - cost 311`.
- **Overlay "Show the route map":** the live corridor, the planned route, and dead places colored by evidence, drawn with atlas.cpp's helpers. MCP: gucci_routemap and gucci_routemap_explain.
- **Offline tests** (`tools/route_tests.cpp` in run_tests.bat) use synthetic timelines and a synthetic motion table. They cover Nigel's three examples (trap orb, dead-end fork, shrinking UFO corridor) and the Bloodbath shape (ball flips before a UFO under a ceiling thorn).

```cpp
// routemap/places.hpp
struct PlaceKey { uint32_t step; int16_t yBand, vyBand; uint8_t mode, speed;
                  uint8_t flags; /* gravity mini held standing buffered dash boost */ uint16_t surface; };
struct Place    { uint32_t fromStart, toEnd, backPlace; uint8_t backRoad, evidence /* Map Sim Proof Game */, trust; };
struct Road     { uint8_t kind /* Keep Press Release OneTick Repress Spam */, at, cadence; };
// routemap/motion.hpp
struct MotionKey  { uint8_t mode; bool mini, upsideDown; uint8_t speed, warp, tps; uint16_t gravityMod; };
struct MotionStep { float vy, dy, dx; uint8_t flagsAfter; };
// routemap/timeline.hpp
struct Interval { float lo, hi; uint8_t kind /* Hazard InnerSolid Top Bottom Room Contact */, certainty; int uid; };
```

### 2.8 The drive: exact A* on GD's physics (`routemap/drive.*`, `routemap/snapstore.*`)

- **The session.** One Sim session begins from the live game at the run's start, so it has the exact table. It pauses between slices the way `LegState::pause` does and resumes the way stepLeg does.
- **A node** is an exact state at a step boundary: parent, road, g, h, tick, key, snapshot handle, and a state (Open, Closed, Dead, Deferred or Stub).
- **The key** is keyOf (4478), plus 6ef9938 (buffered press, orbs used), plus the tick and x, plus a digest of the player-caused World events so far.
  - Those events are touch triggers, toggle orbs, pickups, and count and collision events, each with its exact tick.
  - The digest is event-sourced, so equal digests mean equal World states. Debug builds compare WorldState bit for bit when keys collide.
  - Rung 0 quantizes y to 1.0 and vy to 0.5. Rung 1 uses today's leg key (0.25 and 0.05). The exact rung uses the raw bits.
- **Roads** are the map's road kinds, played exactly with `Sim::step`, with the input check's verdicts applied.
- **The search** is weighted A*: f = g + w·h, with w at 1.5-2, dropping toward 1.2 inside repairs. h is the map's cost to go for the place the node falls in.
  - Nodes in places with no way home are Deferred: a soft wall, never dropped.
  - A node outside the map gets the nearest place's h, and a `routemap: the map missed a state at x` line is logged as a map bug.
  - When the map's h changes, the open list re-prioritizes lazily, when a node is popped.
- **Partial expansion:** the map's preferred road is tried first. A node is popped again only if its other roads are needed.
- **Caps:** B per step (64 at rung 0), with a share for each class (gravity × mode × mini × speed × side of a fork; classOf, grown). Dropped nodes become 16-byte stubs.
- **Deadness:** a node whose children are all dead is dead, and that spreads to its parents. Dead places go back to the map as `sim` evidence only where the killer is Static or Modelled and the span is trusted.
- **Rungs,** applied locally where the drive stalls: re-open the stubs, multiply B by 4, use a 1-tick grid, allow one more change per step, and use finer keys. The exact rung (bit keys, no cap, every road) gives a sim proof inside a trusted span, the same meaning as gucci_route_leg with exact=true.
- **The snapstore.**
  - Snapshots are kept for open nodes in the active window and for every 16th step.
  - Other nodes are regenerated by replaying their parent chain.
  - The cap is 512 MB, least recently used first.
  - On every regeneration a parity check (from step1-inputs' lineage work) compares keys. A mismatch is a stepping bug: it is logged and the node is marked untrusted.
- **Snapshot slimming,** measured first:
  - drop activated and ring entries for objects more than 2 steps' worth of x behind, except on levels with reverse sections or backward teleports;
  - group the OpLog's sealed chunks so a copy is tens of pointers, not thousands.
- **Fallbacks:**
  - If a whole-level session misbehaves (memory, OpLog growth), start a new session from an advanceStart node every N steps. An exact parent gives an exact table.
  - If the map is late or slow, the drive runs on today's atlas cost to go (planner.cpp graphFor), with gravity added in every mode. The plan degrades; it doesn't stop.

```cpp
struct DKey  { uint32_t tick; int16_t xQ, yQ, vyQ, y2Q, vy2Q; uint16_t bits; /* mode grav mini held ground orb dual buffered */
               uint8_t speedQ; uint64_t ringsUsed, worldEvents; };
struct DNode { int32_t parent, snap; /* -1: regenerate */ Road road; uint8_t rung, state, session;
               uint32_t g, h, tick; float x, y, vy; uint16_t liveChildren; };
```

### 2.9 The clicks

- **The route is the script.** Every road is exact `TickInput`s (trajectory.hpp 85). There is no steering and no nudging.
- **No same-tick UFO taps,** unless the input check shows they do something and the real game agrees in M0's test.
- **Nets:** trimStep (1868) runs on the final script as a safety net. Tight roads get robust's one-tick-late test (2457) as a report only.

### 2.10 Checking and repair (`routemap/rehearse.*`, `routemap/trust.*`)

**The rehearsal** (judge::run, grown):
- It plays from the real game standing at the plan's start. It is sliced across frames, with the updater paused between slices and `s_active` held.
- Inputs are queued the way applyInput does.
- It notes every death (the first hit of each tick; noteDeath resets every tick) and never stops.
- It logs partings against the drive's trace: x, y, vy, gravity, mode, size, speed, and the uid fired that tick (d3da645). On the heavy levels it also records movers and the camera.
- Then `resetWithState` puts the game back, and the frame and x are checked the way judge.cpp checks them.

**Rehearsal anchors.** A GD checkpoint is kept every ~10 s of level (8-64 kept, as keepAnchor does). A later rehearsal starts from the latest anchor at or before the first changed tick. Normal levels can simply restart from tick 1, which takes seconds if the per-tick cost holds.

**Repeatability.** Two rehearsals of the same script must match. The seeds are reset per attempt (updateRandomSeedOnReset, hook_playlayer.cpp 329-345). A mismatch is logged, and those spans become untrusted.

**Only the first would-be death is firm `game` evidence.** Later ones happen after the player carried on through a spike, so they're hints until a rehearsal reaches them cleanly.

**Risky part, reviewed in parallel with Nigel's test:** judge::run inside a run. The `judge::active()` checks in engine_updater.cpp 843-863 must not collide with the pathfinder's own stepping, and the restore must be exact.

**Evidence to repair.**
- **A death at an Uncertain killer:**
  - `addPhantom` (1432) and a `game`-dead place;
  - drive nodes simulated without the phantom, from its window on, are invalidated and re-expanded on demand.
- **A death at a Static or Modelled killer, or a parting over 1 unit (kDivergence).** The simulation was wrong about something it was sure of, so:
  - **The trust record.** Partings are keyed by tick and offset (to 0.1). Three identical ones mark ±32×scale ticks untrusted. The line names what differs: gravity, mode, size, speed, the used flags of portals, orbs and pads within two atlas columns read through m_positionX, and the object the copy fired.
  - **A re-root.**
    1. Rewind the real game to the latest anchor at or before the parting (rewindTo 4830, finishRestore 5964, settleAnticheatSpike 5937).
    2. Re-step it to the parting tick on the route's inputs.
    3. Begin a second Sim session from the live game, with the exact table and the real flags. That state becomes a root node.
    4. The first session stays, for alternatives before the parting.
  - **Untrusted spans.** Sim verdicts there kill nothing and prove nothing. The route through counts only once a rehearsal passes it. In small zones, candidate roads can be asked of the game (judge runs from the re-rooted state).
- **Then:**
  - the map's incremental backward update;
  - the drive resumes. Its open list keeps every alternative, so the new route can leave the old one anywhere earlier;
  - a rehearsal from the first changed step.
- **Never worse** (saveFurthest 710): the furthest real-checked route is kept. A new route that rehearses worse is dropped, unless it is a forced class switch.
- **An explanation line per repair.** For example: `routemap: died at x 12185 (ceiling thorn uid 7382, static) in the UFO entered right side up at x 11715; no road left that way; re-routed from x 11490 (ball flip); rehearsing from tick 8100`.

**The run that counts** (Phase::Playing):
- The rehearsed route goes through goLive/liveInput (4712/4866), applyInput (5229, queued buttons), stepGame (5251) and commitTick/afterTick (5261/5283), with anchors (keepAnchor 775). The drift check runs against the drive's trace every tick and is never muted (ea7e4c4).
- Anything unexpected is handled like a death or parting above.
- Then saveResult (6069) and the cold replay from a fresh open. Only a passing cold replay counts.

**Stops.**
- **kStuckSeconds:** 5 minutes with no new furthest exact-alive tick and no new furthest rehearsed tick (pathfinder.cpp 105, 905-925).
- **"No road past x":** only after the exact rung agrees in a trusted span, or the game agrees in an untrusted one. It names the step, the classes ruled out and the evidence.
- **"The same dead road again":** evidence flags and dead keys are never retried, and the stop line lists them. They serve as the nogood ledger.

### 2.11 Moving objects, triggers and route-dependent levels

1. **Movers driven by x** (the World's 29 kinds) are exact in the drive and timed in the map from the scan.
2. **Kinds the World doesn't run** are Uncertain. In the map they don't block (they add doubt cost) until they are recorded (H2). In the drive, phantoms and rehearsals settle them.
3. **Route-dependent triggers:** touch (1595), toggle orbs (1594, 3643), pickups, items and counts, collision triggers, on-death, and follow player Y (1814).
   - The drive is exact through the World digest: pressing a toggle orb and not pressing it are different nodes.
   - The map branches the scan (fire or skip) at each choice the atlas finds (its toggles and playerGroups in build) whose targets reach the live corridor. Places carry a branch tag until the two recordings agree again.
   - At most 8 branches are live per region. Past that the region is "route-dependent": doubt cost, settled by the drive and by rehearsals.
4. **Random triggers** (1912, 2068): the World marks their picks unsure. Rehearsals check repeatability, and spans driven by random picks are certified by the real game.
5. **Teleport portals** are contact answers, and the clock jumps (8dcfc5e follows them in the atlas). A backward teleport hands that stretch to the drive alone.
6. **Dual:** the drive keys both players (keyOf for count == 2; enterDual 2575). The map keeps pair places only while they stay few. Otherwise the stretch is optimistic in the map, with h taken from after the solo portal.
7. **Time warp (1935) and gravity triggers (2066):** a motion table per warp factor and per gravity mod.

### 2.12 The heavy-level simulator track (core, in parallel)

- **H0, census** (code with M0): gucci_level_census reports, per level:
  - trigger kinds and counts;
  - for each kind the World doesn't run, how many collidable and killer objects its groups hold (WorldDef group reach);
  - random triggers near the path;
  - dual, teleport, rotate, reverse and platformer objects;
  - toggle orbs and touch triggers;
  - the y section factor;
  - the real game's cost per tick, from a 2,000-tick judge run.

  The census decides what H2-H4 need.
- **H1, measure** (code by Oct 29):
  - (a) the scan alone at tick 1 on LIMBO, ORBIT and Heliopolis, timed part by part;
  - (b) a 2-D pending pass in `startPending` when `m_fy > 0`;
  - (c) a no-input recording pass on the real game from tick 1 (deaths noted, safe mode on, nothing saved) that records the collidable movers near the player and the camera every tick. Compared with the scan, it gives the disagreement table: group, trigger kind, first tick and size of the miss.

  The recording's clock is checked against the scan's. Past a speed portal the no-input player missed, the recording waits for the engine's own routes.
- **H2, recorded movers and camera** (by Nov 11):
  - The Materializer gets a second pose source: recorded poses per tick for groups no route-dependent trigger touches, used while the run's clock matches the recording.
  - `Materializer::write` gains rotation, scale, and whatever GD's collision reads for oriented boxes. Check the binary and Silicate first.
  - The recorded camera fields are written each simulated tick and put back at the end, so the flying bounds follow the camera (tierd.cpp (i)).
  - Recorded groups count as Modelled while the clock matches.
- **H3, the heavy three on the Route map** (by Nov 21): rehearsals along each route re-record movers and the camera as that route fires its triggers. Untrusted spans are certified by the game.
- **H4, speed** (alongside):
  - snapshot slimming;
  - `materializeNear` after a restore re-reads only what the abandoned branch wrote. On ORBIT it takes 18-21 of the 31-35 µs per tick, and up to 64 µs in branches;
  - per-stretch budgets.

  Targets: ORBIT at 60k simulated ticks/s or more, Heliopolis at 100k or more.
- **Porting trigger kinds from the binary** happens only where H1's table shows recording can't cover them, most likely follow player Y and collision triggers. Each one is checked against GD's own step, as the move command was.

### 2.13 Existing code

| Existing | Role |
|---|---|
| Trajectory::Sim (begin 2204, step 2724, snapshot 2925, restore 2982, end 3070); run (3809), captureStart (1357), advanceStart (5094), inVoid (770), addPhantom/hitsPhantom (1432/1448), noteKiller (796), enterDual (2575) | Reused: the physics |
| World (step, fire, certaintyOf); Materializer | Reused. The Materializer gains a recorded pose source, rotation and scale writes, and the 2-D pending pass |
| LegState, beginLeg/stepLeg/finishLeg (4403-4881) | Grown into the drive. Legs stay for Today and for "Find a way" |
| alternativesFor (3393), keyOf (4478) | UFO: a press held through the tick, a one-tick press, and release and re-press from held. Merge key with buffered press and orbs used (M0, both engines) |
| beginGhost/stepGhost | Grown into the scan |
| atlas.cpp build (277), classify, toggles, draw helpers | Real positions, teleports. They feed the timeline, the map's branch points and the overlays |
| planner.cpp | Kept for Today's route modes. Its cost to go is the drive's fallback guide |
| judge::run | Grown into the rehearsal |
| Executor: start (303), tick/mayWork (888/1089), goLive/liveInput, applyInput, stepGame, commitTick/afterTick, keepAnchor, keepStart, rewindTo, finishRestore, settleAnticheatSpike, onWouldDie, saveFurthest, saveResult | Reused, with new Phase::Planning and Phase::Playing. The drift check is unmuted for both engines |
| decide(), better(), evaluate, the repair walk, escalation, rescue legs, Route ideas and drives | Left alone for Today. trimStep and robust are used as nets and reports |
| memory.cpp, the GPU stage, model/rules.* | Untouched or not landed |

**New** (about 7.2k lines):

| File | Lines |
|---|---|
| routemap/engine.* (phases, `routemap:` lines) | ~800 |
| scan | ~500 |
| timeline | ~600 |
| motion | ~500 |
| contacts | ~500 |
| places and sweep | ~1,000 |
| drive and snapstore | ~1,500 |
| rehearse and trust | ~700 |
| UI and MCP | ~600 |
| tools/route_tests.cpp | ~500 |

The UI and MCP part is: the switch "Engine: Today / Route map (test)" saved as `pf_engine`; gucci_pathfinder `brain` = today | routemap; "Plan the level"; "Plan from here" in practice; "Show the route map"; gucci_routemap, gucci_routemap_explain, gucci_motion_table, gucci_rehearse and gucci_level_census.

### 2.14 Order of work, with an in-game test per step

Every in-game test waits for Nigel's go-ahead. Code and run_tests.bat don't need the game. Parallel worktrees:
- **A:** M0 and the drive;
- **B:** the map;
- **C:** check and repair;
- **D:** the heavy track.

B and C start on Oct 26 against interfaces fixed in `routemap/engine.hpp`. Everything lands on master one commit per change.

**M0, Sat Oct 24 to Mon Oct 26: shared fixes, both engines.**
- **Build:**
  - Land these, one commit each: 629920b, 6ef9938, 380bb50, cb9fc9b, ea7e4c4, 18f2d17 (renamed to Route map, with `brain` = routemap and the header moved to `routemap/engine.hpp`), 4d3c664, be0d881, 8dcfc5e, d3da645 and b1ebd51.
  - 8dcfc5e may need a hand merge, because it sits on b64bbf0 in atlas.cpp.
  - New: the UFO menu in alternativesFor, which the branching search also uses at 3604.
  - New: gucci_level_census.
- **Test** (Claude, in practice, safe mode on):
  - Inside Bloodbath's UFO (x ≈ 11800) and ToE2's (about 18%), run gucci_abs_simulate and gucci_sim_vs_real with `t1 r20`, `p1 r20` and `r21`.

    | What happens with `t1` | Next step |
    |---|---|
    | Neither the copy nor the game flaps | The menu fix |
    | Only the game flaps | Fix the copy's input path; check Silicate first |
    | Both flap | Look further with the parity line |
  - gucci_route_leg from a ball checkpoint at x 11300-11600 to x 12425, at any height, draws a way, or reports peak states above 2.
  - No UFO leg logs 4n−2 steps.
  - Regression: Clubstep cold on Today, 100%, with a cold replay.
  - The census table for all 8 levels.

**H1, by Thu Oct 29** (see 2.12). **Test:**
- the scan finishes on LIMBO, ORBIT and Heliopolis with a time per part, or names what stops it;
- ORBIT and Heliopolis pending time falls from 44.9 s and 105.5 s, with Today's % no worse;
- the disagreement table.

**M1, by Sun Nov 1: the drive in the simulation only.**
- **Build:** Phase::Planning; the drive on the atlas guide; the snapstore and the parity check; "Plan the level" and "Plan from here"; the explored-places overlay; gucci_routemap.
- **Test** (Claude, game frozen at tick 1, nothing played):
  - Clubstep, ToE2, The Nightmare and Bloodbath each get a simulated route to the end within about 5 minutes.
  - Bloodbath's route passes x 12185, and ToE2's passes x 5369.
  - The log shows nodes, ticks/s, snapshot memory of 512 MB or less, and 0 parity mismatches.

**M2, by Thu Nov 5: the map.**
- **Build:** scan, timeline, motion tables and their check, contacts, places, sweeps, classes, explanations, the overlay and the route tests. The drive is now guided by the map.
- **Test:**
  - Nigel looks at the overlay, with no run needed:
    - Bloodbath 25-35%: does the corridor through the UFO keep only one gravity, and does the last ball flip before x 11715 match it?
    - ToE2 15-20%;
    - Clubstep 65-72%;
    - The Nightmare 85-92%.

    The question for each: does it look like a way a player would go?
  - Claude:
    - map size and build time are logged per level, with one motion line per mode;
    - on the same four levels, the drive simulates at least 30% fewer ticks than in M1;
    - run_tests.bat passes.

**M3, by Mon Nov 9: check and repair.**
- **Build:** the rehearsal, the trust record, re-roots, evidence into the map and the drive, never worse, Phase::Playing, and the stop lines.
- **Test** (an unattended cold batch: safe mode on, Remember off):
  - Clubstep, ToE2, The Nightmare and Bloodbath each reach 100% with a cold replay. Deadlocked is a stretch goal.
  - Bloodbath passes tick 8675 with no death there.
  - The Nightmare passes x 25740, and its 19698 parting is gone or named within 3 partings.
  - Every repair has its `routemap:` line.

**H2, by Wed Nov 11.** **Test:**
- gucci_sim_vs_real along recordings: ORBIT's tick-521 gravity parting, ORBIT's 1179-1192 ceiling partings, and LIMBO's moving-ground partings are gone or named;
- in a LIMBO rehearsal, the deaths the simulation didn't see fall well below 22 of 32.

**M4, by Fri Nov 13.**
- **Build:** branch tags in the map, and a tools/selfplay batch: a level list, safe mode required, Remember off, a watchdog through gucci_restart_game, a cold replay of every result, and a morning report.
- **Test:**
  - Deadlocked: 100% cold, with a cold replay.
  - Nigel's ~10 new levels overnight: a solved/failed list, each failure with its stop line.

**Checkpoint, Sun Nov 15.**
- The five normal levels, each cold and replayed cold.
- For the heavy three: % reached, scan, map and drive times, and the disagreement table.
- Nigel decides: ship it marked experimental, or hold 2.0.

**H3, by Sat Nov 21.** **Test:** LIMBO, ORBIT and Heliopolis reach 100% cold with cold replays, or a stop line names the step and the evidence. Every rehearsal's would-be deaths are listed.

**M5, by Tue Nov 24.** **Test:** all 8 cold, unattended, in one batch, every 100% replayed cold. Nigel picks the default.

**Release, Thu Nov 26.**
- Tick ROAD_TO_2_0 step 4. Point AI_PATHFINDER_PLAN.md and docs/ROUTE_ENGINE_PLAN.md at this plan.
- The version bump comes only after Nigel's in-game test.

### 2.15 Size and speed (estimates, measured in M1, M2 and H1)

**Normal level (Bloodbath: about 27k ticks, 6,750 steps):**

| Part | Estimate |
|---|---|
| Map | 2-13M places, 16-130 MB |
| Each sweep | 2-15 s |
| Contacts | 1-6 s |
| Scan | under 0.5 s |
| Drive | 0.5-2M steps with a good map: 10-60 s at about 100k steps/s |
| Rehearsal | seconds |
| Play | about 112 s |
| Cold replay | about 112 s |
| **First pass in all** | **about 3-8 minutes** |

**ORBIT and LIMBO** (44-55k ticks, 11-14k steps):
- map: 5-25M places, sweeps of 5-40 s each;
- rehearsal: 40-50 s, if about 1 ms a tick holds;
- play: 185-230 s;
- in all: 15-60 minutes, with no firm bound until H1-H4 land.

**Heliopolis** falls in between.

**Memory:** the map 16-250 MB, the snapstore up to 512 MB, recorded movers about 50 MB on ORBIT, and anchors as today.

### 2.16 Working rules

- **Build and commit (CLAUDE.md §0):**
  - one commit per logical change;
  - `.\build_win.bat`, then look for "Build complete";
  - bump GB_BUILD_LABEL on every build, editing it by the line that ends in a backslash;
  - push to master and watch CI.
- **Running the game:** nothing in-game until Nigel says; safe mode on and Remember off for every automated run.
- **Results:** a result counts only after a cold replay.
- **Reviews:** only for the risky parts (the rehearsal inside a run, Materializer pose writes, snapshot slimming), run in parallel with Nigel's test and never gating it.
- **Half-port checklist:** each milestone's log is grepped for every `routemap:` line its paths must emit.
- **Copy:** user-facing text in American English. Credits unchanged.

### 2.17 Risks and open questions

1. **The map is a relaxation.** If it's too optimistic somewhere, the drive explores more there. That costs time, not correctness. Bands halve locally wherever the map keeps proving wrong.
2. **The map could close a real road** (a wrong table, a misread hitbox, float rounding). Walls are soft and nodes are deferred, never dropped. "No road" stops a run only after the exact rung or the game agrees.
3. **Contact answers from set-up copies** may miss a hidden field: rotation, ring contacts, buffered presses, slope state. Exact answers win, and an object that disagrees goes exact-only.
4. **Place counts may be off by 10× in open flying rooms.** The answers are coarser bands, 8-tick steps, and beams.
5. **A whole-level session has never run.** Unknowns: OpLog growth, the written sets, and the WorldState copy over 50k ticks. M1 measures them. The fallback is a session per N steps.
6. **The UFO.** If a one-tick press doesn't fly the copies either, M0 shows it before anything leans on it.
7. **The rehearsal inside a run:** collisions with `judge::active()`, the exactness of the restore, and the real game's tick cost on the heavy levels (H0 measures it).
8. **The heavy levels:**
   - toggles are dense (148 on LIMBO, 113 on ORBIT);
   - random picks and collision triggers between objects;
   - recorded poses must set everything GD's collision reads.

   This is the likeliest slip.
9. **Handoffs.** If a bar level's hardest stretch is dual, platformer or reverse, the map is weak there.
10. **Schedule.** About 7k lines in 4.5 weeks, and the tests wait for Nigel's PC. If M3 slips past Nov 12, Nov 15 shows 2-3 normal levels, not 5.
11. **Not sure:**
    - whether Bloodbath's UFO needs an upside-down entry;
    - what causes The Nightmare's 19698 parting and Bloodbath's 1119/1127 partings;
    - whether ToE2's 18% wall is only the UFO;
    - whether rehearsals and play draw the same random numbers;
    - why the ghost stalled on LIMBO and ORBIT;
    - the real game's cost per tick.
12. **Safety net.** Today stays the default, and nothing is deleted until Nigel flips the default.