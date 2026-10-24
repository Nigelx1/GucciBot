# Road to 2.0

Everything left before GucciBot 2.0, in Nigel's order (2026-10-19). Boxes get ticked as things land, with the build that did it. Nigel's in-game test is what counts as done for anything you can see in the game.

## 1. Credits (done)

- [x] New order, "Promo help" for DashBlaze and ace, chagh for the converter, peony third (build -eg)
- [x] Nigel's wording: dihmaster500 tester, NaN GD the frame-window originator, Bogdaner09 open source, GWDdoS optimization help, ToastexGD foundation (build -eh); site live

## 2. Minor features and bugs (next, starting with the Mac crash log)

### Click Indicators (Nigel, 2026-10-19: first, before the rest of step 2)
- [ ] Its own feature, not the trainers' click bar, with every option of Bogdaner09's mod (badges, player square and line, draining holds, taps and holds, player 2 colours, timing offset, the rhythm lane, scoring): built -em, untested

### Playback bugs
- [ ] Flip 2-Player Controls (found while fixing #27): GucciBot pressed or read the wrong player in places with that option on (autoclicker, auto flip on death, Mirror Inputs, the practice restore, MCP clicks, the pathfinder, CPS, Survival Indicator, Click Indicators, frame-window markers). GD's handleButton flips a button side only in a classic 2-player level (read from the 2.2081 binary), and -ho follows that everywhere; with the option off nothing changed. Untested in-game: a 2-player level with the option on (autoclicker on player 1 only, Mirror Inputs, a practice respawn while holding), and a 2-player platformer
- [ ] Restarting straight out of a finished replay desyncs the next attempt (seen on VSC, 2026-10-19)
- [x] Mirror Inputs adds inputs to the list it's in the middle of reading: already fixed in build -dk (2026-10-14); the old list hadn't caught up
- [x] Recording wrote phantom presses (fixed -gh/-gi, 2026-10-23): a tick with more than one button event (a release and a press inside one tick) was recorded as R P R P R instead of R P R, because every press is recorded twice and the duplicate check only looked at the frame's last event (since the 2026-09-28 Zafari 2 fix). Nothing moved until the next orb, which the extra buffered press fired early - every Absense pathfinder path with taps or double presses failed its cold replay (ToE2 at 10%). Human recordings with a release and a press in one tick had it too; macros recorded 2026-09-28..10-23 may hold the extra presses
- [ ] Mac: respawning to a checkpoint crashes. Fixed in build -ei (the action manager re-entered during the respawn's refresh tick; Windows always skipped it), waiting for Nigel's Mac test
- [x] Reopening a level with random triggers could crash the game, or quietly damage its memory where it didn't (fixed -he, 2026-10-23; the batches since reopened ORBIT, Clubstep, ToE2 and The Nightmare dozens of times without a crash): the practice fix keeps a pointer into every advanced random trigger, and the list was never emptied when a level closed - Silicate empties it in PlayLayer init, onQuit and goEdit, the port never called it - so the next level's first reset wrote its seed through the last level's freed triggers. Found when the game crashed reopening ORBIT
- [ ] Hacks > Gameplay > Safe mode did nothing (fixed -hk, 2026-10-23, untested in-game): it set a flag nothing read - a half-port since the -bz menu - and wasn't saved, so it was off again at every launch; with it on, a completion, a death's percent and the new-best popup all went through. It now keeps a run from counting, cheat or not, and stays on across launches. Silicate's version also leaves stats, achievements and the attempt and jump counts alone: built -hl (credit peony), untested

### Switches that do nothing
- [ ] Force Platformer (the code behind it is gone and needs writing): built -ej, untested; safe mode counts it as a cheat
- [ ] No Mirror: Only Recording (needs a switch and saving): built -ej, untested
- [x] Render colour fix: the switch is gone (-ej). Nothing read it, in Silicate either, and the fix it names (BT.709, limited range) is always on
- [ ] Instant Respawn (doesn't actually respawn instantly): built -ej, untested
- [ ] Show Macro Path, plus the JMF Trainer reveal mode that uses it: built -ej (Hacks > Visual), untested
- [ ] Custom respawn delay (no control for it): built -ej (Auto retry > Respawn delay), untested
- [ ] Changing TPS while recording (not saved into the macro): built -ej, untested
- [ ] Classic pathfinder: "Hide the search" and its status readout: built -ej, untested

### Macro list
- [ ] Rename a macro (its extra files follow it): built -ek (the row's "..." menu), untested
- [ ] Open the replays folder: built -ek, untested
- [ ] A "..." menu on each macro: convert to .brrr, Calculate: built -ek (also Load, Rename, Delete; right click opens it too), untested
- [ ] The list refreshes itself when files change: built -ek, untested
- [ ] Theme tags showing which theme each macro was saved under: built -ek, untested

### Full conversion in the game
- [ ] Other bots' macros (.gdr, .xd, .json and the rest) show in the list and convert in one click: built -ev (Saved macros > From other bots: Convert, Convert all, Import a macro, Shift frames; each macro's "..." > Export as), untested. Which frame each bot counts from needs an in-game check per bot: Silicate's are shifted by one (its engine is GucciBot's, one frame behind), the rest carry over as saved
- [ ] Silicate .slc: built -eu (v3 and v2 from libslc's layout, v1, and anticroom's .scbf1); 60-odd real .slc v3 macros read cleanly; untested in-game
- [ ] Old .brrr v1, rebuilt fresh (never from the old ToastyReplay-era code): built -eu, worked out from a real 1.x macro (format 4); it matches the bundled JMF .gdr frame for frame; untested in-game
- [ ] The same formats as the site's converter, wherever possible: built -eu, and .ttr3 in -ez at Nigel's call (27 read, 21 written; .ttrl still out); every writer round-trips (tools/run_convert_tests)
- [ ] .flp (#19): waiting on a sample; asked for one on #19 and none has come

### Calculate leftovers
- [ ] The result stays visible after a run: built -el, untested
- [ ] Conflicting-mod warning in the menu (it's only logged now): built -el, untested
- [ ] An in-level progress box with Cancel: built -el, untested
- [ ] Start-position warning: built -el (set when a macro recorded from a start position loads), untested
- [ ] Reorder bands, and Brrr/Bells buttons for band sounds: built -el, untested; a Sort by window button built -hl, untested
- [ ] Mute during Calculate: built -el (Calculate page switch), untested
- [ ] Per-band images and pulse (the site advertises them): built -ew (a band's own image in place of its shape; a pulse on the marker, the label or both, with fade-in, hold and fade-out), untested

### Slipped through: in docs/FEATURE_AUDIT.md, never carried onto this list (re-checked row by row, 2026-10-21)
- [ ] Manual Frame Windows: the table of every input with a window you can type, marked auto or manual, with Clear (Juice's idea; the old menu had it). The suggestions go in it: built -fe (Calculate > Windows by hand > Every input); Nigel checked the menu, 2026-10-21
- [ ] Band sound packs: name, import and export a set of band sounds (the -ew line wrongly counted Brrr/Bells as this)
- [ ] Import your own band sound and image files with a picker, and Import Folder for a whole folder of them
- [ ] A color for the band pulse (the old pulse had one)
- [ ] Legend groups: several bands counted on one legend row
- [ ] Ring boldness: line thickness of the plain ring
- [ ] The CBF placed / on-edge counter: counted per attempt, shown nowhere
- [ ] Settings > Diagnostics: the startup self-check's results
- [ ] The Log Frame Increments switch
- [ ] HUD branding overlay: what it showed is unknown; waiting on Nigel's description
- [ ] Ask Nigel: branch playback (View / Go) for the suggestion's alignments; Juice's numbers on the in-level markers (clashes with "markers stay strict")

### Render
- [ ] Presets saved by name, plus the one-press Showcase preset (8K60 lossless): built -er (Showcase is lossless RGB x264 with FLAC in MKV: yuv444p isn't a format this renderer writes), untested
- [ ] A "render done" popup: built -er, untested
- [ ] An "FFmpeg missing" banner with an Open Folder button: built -er, untested
- [ ] Pixel format choice: built -em, untested
- [ ] An in-render HUD: built -er as frames and seconds on the Render card while it renders (an in-level overlay could end up in the video), untested
- [ ] Nigel's test of the FLAC fix and "Start from the beginning" (build -du)

### The rest
- [ ] Status bar: what the bot is doing (recording, playing, calculating, pathfinding, rendering), plus frame, TPS, speed and inputs: built -es (under every page title), untested
- [ ] Update checker, tried again (removed in 1.7 because it crashed the game on launch; this time it waits until the game is fully up): built -es with a crash guard (a crash mid-check switches it off for the next launch), untested; not run on launch here (GD was closed)
- [ ] Trainer video import for the normal Trainer, with a file picker that can't crash: built -et (the Trainer page's own Video Mode card; one Video Mode at a time), untested
- [ ] Eclipse speedhack override: parked (Nigel, 2026-10-20: GucciBot overriding other mod menus' speedhacks, "too many moving parts"; maybe later)
- [ ] Moving wave spam (#25): goes with the AI pathfinder (Nigel, 2026-10-20)
- [ ] CPS on the HUD (#28): built -ew (CPS and Max CPS, ported from Silicate), untested; Nigel keeps the speedhack as it is
- [ ] Converter: GDR2 macros reading as all player 2 (#15). The reader matches libGDR's own; needs the reporter's file
- [ ] Bug reporters' follow-ups: #20 Android practice-death crash (likely the same bug as the Mac one, fixed in -ei; needs a phone test); #23 start positions and checkpoints; #11, #13 and the -103 error; macros ignoring inputs (#14, #8, #18); logs from the Calculate reports
- [x] about.md, the README and the site match what ships: -fa (acad95e) and the site (aaa4a8b)

## 3. Calculate

- [x] Port audit against anticroom's original (docs/CALC_PORT_AUDIT.md): two port faults fixed (builds -ed, -ee), plus the end-of-level death (-ef)
- [ ] anticroom's antics-counter changes, round 1 (his beta.2-beta.7 changelogs; no source, written from the descriptions): built -hl (2026-10-23), untested in-game. Reviewed: counting core and hooks clean; the page delete, re-import, non-English file names and #27's player choice fixed in -ho (with two older bugs the review found: a new recording saving the old macro's windows, a crash at launch on a macro file whose name Windows can't spell):
  - Search distance (Sweep range renamed), Max window (a window that reaches it shows N+ and counting moves on), presets Balanced / Accuracy / Absolute
  - Recovery Range: three fixes (state leak, the next input moves with its release, offsets that can't play are skipped); it widens windows on purpose
  - Check past next input (Advanced, experimental, off by default); a test leg's reach is one rule now (legReach)
  - Decimal band edges for CBF windows, Sort by window, the Every input table in pages with Delete this page's windows
  - UFO inputs lose the hold-mode mark; Click Between Frames isn't flagged while paused; the trace's false frame-jumped warnings
  - A canceled or failed run keeps the windows saved before it (Save On Finish)
  - The Leg trail (Advanced > While it runs): each test run drawn in the level
  - Import: a dated copy in replays/imported, the picker opens there, a file already imported is selected; Android's permission note
  - Text boxes let go of the keyboard after a click on the game; settings reach the disk a moment after they change
- [ ] antics-counter round 2: built -hn (2026-10-23), every part behind a switch that starts off, so counts don't change until it's on; needs an A/B test by Nigel or Juice on the same macros (switch off, then on). 2-player fixes (Calculate > Advanced > Experimental: order-independent dispatch, player-aware CBF splits, the setup sweep and entry offset per player, a test runs to the same player's next input, imported 1-player double-flag clicks counted once); Faster sub-tick search (Sub-tick card: seeded from the whole-frame result, about a quarter of the test runs; a gap smaller than a frame inside a window can come out different); Dependent pair search reworked (only tight inputs right after another, in checkpoint order, measured the way Calculate measures). Each part reviewed alone, then merged and reviewed together; the switch-on fixes from that are in progress. Nigel's call: should ~N show on markers or only in the frame editor (markers stay strict)?
- [ ] The strict count, plus "my suggestion is x frames" using Juice's Alignment-Independent method (credited), flagged in the frame editor: built -ex, untested (a pass after the strict count, for strict windows of 2 or fewer right after another input; on by default, Calculate page > Suggestions for suspicious clicks)
- [ ] Speed: progress and a time cap (resetting to checkpoints is 87% of the time): built -ez (a time cap that keeps what it measured, and time so far and to go in the level), untested; the restores themselves aren't faster yet
- [ ] Markers in the ground (#26; the fix in build -dt is untested)
- [ ] The trail check being one frame off; recheck the 42,000-frame noclip runaway
- [ ] Markers missing for player 1 in some dual levels, showing only on player 2 during duals (#27; TheCinnyBun, Helios, TUAPEKA ...): built -hl, untested. Likely cause: inputs stored as player 2 in a 1-player level (recorded with Flip 2-Player Controls on, or some imports); markers now go on both players in a dual whatever the flag. Not sure it's the reporter's cause: their macros haven't been seen
- [x] Mirror portals and the markers (Nigel, 2026-10-21): fixed and confirmed in -fl ("it works"). GD flips the object layer itself, so the markers' signed scale had turned them round (-fh); through the flip GD turns the picture and draws the player off its spot in the level, so a jump mid-flip now starts on the player and slides onto its real spot by the end of the flip (-fk, -fl)

## 4. AI pathfinder (AI_PATHFINDER_PLAN.md)

- [ ] A map that knows when blocks move and appear
- [ ] The AI trying every orb and toggle choice
- [x] Building the map in slices, so the game stops freezing for about 9 seconds: built -ff, seen working in Nigel's 2026-10-21 log (each map's ghost done in under a quarter second, spread over 8 to 44 frames)
- [ ] The route engine drives: **the build plan is docs/ROUTE_ENGINE_PLAN.md** (2026-10-22: two diagnoses, three designs, a judge). Step 1 built -fp..-fu, untested (GD gets its anticheat spike after every rewind - really from -fu, -ft's guard had made it a no-op -, so re-played ticks keep their slope launch; the jump back to the route stays within the repair floor; random ideas and route legs repeat run to run; the log says why a route search could not run). Step 2 (measure the simulator, three live checks) needs Nigel. Step 3 built -fv, untested (the route search in slices, no stutter; repeatable; calm: a 33 ms gap between input changes except on orbs, the calmest way in wins). Step 4 built -fw (gates name their options: orb use/skip, pad/portal touch/avoid; the next gate ranked from the player). Step 5 built -gc, untested: Route drives, a third setting of the route switch (Off / Route ideas / Route drives, saved; Off by default) where each decision is a leg to the next gate's option, played only to the gate; with it, a run stops after 5 minutes without getting further and says where, and the stop line gives clicks a second (average and busiest second). Steps 6-8 next. Was: The route engine drives (the plan's real R4: the whole-level route chooses, physics finds each leg's exact clicks, proven dead ends crossed off, jumps back to the cause). What's built only adds route ideas to the old picker, which still keeps whatever survives 4 seconds (Nigel: "it doesn't seem like an AI choosing when to click with knowing the whole level")
- [x] No straight-line hovering: the hold-a-height steers removed in -fg (120 clicks a second on the wave)
- [ ] The simulation fast enough for the heaviest levels (2026-10-23, builds -gt..-he): LIMBO (392,852 objects) went from 20,000 to 132,000 simulated ticks a second (-gx: the objects the real game moved since a kept start are written back a column at a time as a copy reaches them - every run wrote all ~5,000 of them first, 46 of 108 s of deciding), ORBIT (311,259) from 1,440 to 5,000-8,000 (-ha: a search going back only looks again at written objects that can be near; -hc/-he: a moved group's members are bounded by the range its moves covered, not every step added up). ORBIT is still about 50 times slower per tick than a normal level - hundreds of real objects move next to the player every tick, and each is written into the game - so the next step, if needed, is that a search going back looks again only at what the abandoned try moved. Two reviews found real bugs in these changes, all fixed (-gy, -he). New tools: gucci_abs_benchmark splits a simulated tick and a run's setup into parts, every run's stop line has a "World upkeep" line, and gucci_abs_profile times a real run part by part. Assistant Access no longer crashes when the game answers a call late (-gy; that also explained the odd replies the test batches got)
- [ ] Fewer clicks and freezes (2026-10-23): the trim now covers the whole realtime commit (-go) and has guided (-gn), chunk (-gm) and hold (-gp) passes - ToE2 4.4 clicks a second on average and 98 in the busiest second (was 13.9 and 240), Deadlocked 3.1 and 54; freezes are logged with their length (-gq) and Deadlocked's fell to 19 for 18 s in all (30-72 s before; no re-simulation before portals, short freezes become slow play). Left: Clubstep keeps a 240-press second where a mini cube flips on orb after orb between landings (needs a hold for the landings and fresh presses for the orbs at once); longer plans for hard flying stretches were tried (-gq) and taken out again - Deadlocked stalled at 70% with them
- [x] ToE2: 100% from a cold start on the normal engine, no macro loaded, in 260 s (2026-10-23, build -go; 10 backtracks, 4.4 clicks a second on average, 98 in the busiest second), and the cold replay completes. What did it: the trim now thins everything a realtime plan plays (-go; it stopped at the first third, and the rest went out as orb spam - 240 presses in the busiest second, 13.9 a second on average), with the chunk (-gm) and guided (-gn) passes; the recording fix (-gh) made its paths replay at all. Before: the normal engine stopped at 85% (x 24712) and Route drives from a cold start at 89% (x 25982); Route drives finished it twice with the previous path loaded as the "human" reference. Was: dies in a big-ship slope 'diamond' at x ~22940-23012, the gravity coming from blue orbs at x 21825-22185 that the 4 s look-ahead never pressed
- [x] Deadlocked: 100% from scratch, no macro, in 7 minutes (2026-10-23, build -ge; 16 dead ends, 3.9 clicks a second on average, 81 in the busiest second), and the saved path replays from the start to 100%. Two walls, both found live through Assistant Access: 22% was the simulation (fixed -gb): GD files every object in vertical section 0 (its y section factor is 0), and the World's object writer took that as 100-unit rows, so no moving object above y 300 ever moved in a simulated run - the pad that drops from the ceiling after the blue orb stayed up, and the orb always looked fatal (Nigel: "the block at 22 doesn't move and appear"); every level with moving objects up high had this. 54% was an upside-down robot falling up through a gap in the ceiling: the game only kills it about 700 ticks later, thousands of units up, so falling up looked like the longest survival within the 4 second look-ahead; it now counts as a death where it starts (-ge). Also the World's own start-up check sometimes failed and turned the World off for the whole launch (a run then played like the old engine - one stalled Deadlocked at 23%): the cause was a copy's GravityRestored, raised while the copies are set up between ticks, going to the real game (-gd guessed wrong; fixed -gs, 2026-10-23)
- [ ] Coins: a "Collect coins" switch, off by default; each coin a gate the route must pass. After the route engine drives
- [ ] Re-run the review of builds -dm to -dq that got cut off
- [ ] The bar (Nigel, 2026-10-21, eased from every RobTop level + 16 of 20 online): RobTop's three demons (Clubstep, Theory of Everything 2, Deadlocked), The Nightmare (Jax, id 13519), Bloodbath (Riot, 10565740), LIMBO (86084399), Orbit (ORBIT by MindCap, 133175713), Heliopolis (MindCap, 136530685, unrated). Baseline 2026-10-21 (build -ff): Clubstep 100%, ToE2 78% (84% once), Deadlocked 22%; the other five not run yet. 2026-10-23, from cold starts, every result replaying cold: Clubstep 100% (normal engine, -gp: 250 s), ToE2 100% (normal, -go: 260 s), Deadlocked 100% (normal, -ge/-gl/-gs: about 7 min), The Nightmare 100% with Route drives (-gs: 186 s, 1.3 clicks a second) and 75% on the normal engine (a ball corridor at x 21354: the repairs' ideas never find the one way through, and the simulation and the game part there by 1.77 units at tick 16380 - under investigation); Bloodbath 100% on the normal engine (-gs: 208 s, 3.8 clicks a second, replays cold from a fresh open); later runs: Clubstep 100% on -gx/-hf/-hg/-hh (161-230 s) and once stuck at 70% (x 19019, a spot repairs sometimes take a while to get past), ToE2 100% on -hh (194 s) and twice stuck at its old 85% wall (x 24721), Deadlocked 100% on -gx/-he (315 s); The Nightmare 100% on the normal engine too (-hp, 2026-10-23 19:28: 120 s, no dead ends, 1.3 clicks a second; saved as "The Nightmare 2", replays cold to 100%). Its earlier 75% stalls: GD restores each portal's used flag exactly as it was when the checkpoint was made (read from the 2.2081 binary, then checked in-game), so a portal still used after a rewind had already fired at that checkpoint's tick; the simulation guessed otherwise. Exact start flags (-hp, Pathfinder page, a test switch that starts off) gives runs from a kept start the flags the game really had; a run with it on is being compared. LIMBO (2-6%) and ORBIT (1%) stall early, and their simulation is still slow (see the item above); Heliopolis not run yet. Nigel's macros for LIMBO, ORBIT and Heliopolis are in Downloads as references (the pathfinder must work without them)

## 5. Mac, iOS and Android parity (everything but the pathfinder)

- [ ] Rendering and Video Mode
- [ ] Under-162 TPS and High TPS Precision
- [ ] The trigger model in the look-aheads
- [ ] Seeded random teleports and shake
- [ ] Checkpoint placement
- [ ] iOS: the game slows down at 1,000+ TPS (#30)

## 6. Standalone copy (Windows, no Geode)

- [ ] One codebase with a build switch: a loader, the hooks, a small stand-in for the parts of Geode GucciBot uses, and bindings for the new GD version

## 7. How-to, VirusTotal, GitHub replies

- [ ] A how-to (#23, "how do I open it")
- [ ] A VirusTotal report
- [ ] Replies to the rest of the GitHub issues (Claude drafts, Nigel posts)

## 8. Polish round

- [ ] A feature-by-feature test of the rebuild and the new menu
- [ ] Cosmetics: the Mega Hack-style skin, the animated backdrop, BrrrBot's snow, the old title bar and fades, a menu opacity slider
- [ ] Click sounds' background noise: built -fa (a pack's noise track loops under the clicks), untested

## Release

- [ ] The new GD version: Geode support, or the standalone copy
- [ ] The trailer ships with 2.0
- [ ] Release day: version bump, about.md/README/site/changelog, tag v2.0.0, the GitHub package as Latest, the site's Download link

## Not coming back

- An overall difficulty number beside L* (dropped by Nigel, 2026-10-21, after agreeing to it the same day)
- The "Macro Saved" popup (removed at Nigel's ask)
- The old analyzer's settings and debug tools (anticroom's analyzer replaced them)
