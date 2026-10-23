# Road to 2.0

Everything left before GucciBot 2.0, in Nigel's order (2026-10-19). Boxes get ticked as things land, with the build that did it. Nigel's in-game test is what counts as done for anything you can see in the game.

## 1. Credits (done)

- [x] New order, "Promo help" for DashBlaze and ace, chagh for the converter, peony third (build -eg)
- [x] Nigel's wording: dihmaster500 tester, NaN GD the frame-window originator, Bogdaner09 open source, GWDdoS optimization help, ToastexGD foundation (build -eh); site live

## 2. Minor features and bugs (next, starting with the Mac crash log)

### Click Indicators (Nigel, 2026-10-19: first, before the rest of step 2)
- [ ] Its own feature, not the trainers' click bar, with every option of Bogdaner09's mod (badges, player square and line, draining holds, taps and holds, player 2 colours, timing offset, the rhythm lane, scoring): built -em, untested

### Playback bugs
- [ ] Restarting straight out of a finished replay desyncs the next attempt (seen on VSC, 2026-10-19)
- [x] Mirror Inputs adds inputs to the list it's in the middle of reading: already fixed in build -dk (2026-10-14); the old list hadn't caught up
- [ ] Mac: respawning to a checkpoint crashes. Fixed in build -ei (the action manager re-entered during the respawn's refresh tick; Windows always skipped it), waiting for Nigel's Mac test

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
- [ ] Reorder bands, and Brrr/Bells buttons for band sounds: built -el, untested
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
- [ ] The strict count, plus "my suggestion is x frames" using Juice's Alignment-Independent method (credited), flagged in the frame editor: built -ex, untested (a pass after the strict count, for strict windows of 2 or fewer right after another input; on by default, Calculate page > Suggestions for suspicious clicks)
- [ ] Speed: progress and a time cap (resetting to checkpoints is 87% of the time): built -ez (a time cap that keeps what it measured, and time so far and to go in the level), untested; the restores themselves aren't faster yet
- [ ] Markers in the ground (#26; the fix in build -dt is untested)
- [ ] The trail check being one frame off; recheck the 42,000-frame noclip runaway
- [ ] Markers missing for player 1 in some dual levels, showing only on player 2 during duals (#27; TheCinnyBun, Helios, TUAPEKA ...). The player-1 marker uses the mark's saved position and the twin uses the trail; look at why the first is missing there
- [x] Mirror portals and the markers (Nigel, 2026-10-21): fixed and confirmed in -fl ("it works"). GD flips the object layer itself, so the markers' signed scale had turned them round (-fh); through the flip GD turns the picture and draws the player off its spot in the level, so a jump mid-flip now starts on the player and slides onto its real spot by the end of the flip (-fk, -fl)

## 4. AI pathfinder (AI_PATHFINDER_PLAN.md)

- [ ] A map that knows when blocks move and appear
- [ ] The AI trying every orb and toggle choice
- [x] Building the map in slices, so the game stops freezing for about 9 seconds: built -ff, seen working in Nigel's 2026-10-21 log (each map's ghost done in under a quarter second, spread over 8 to 44 frames)
- [ ] The route engine drives: **the build plan is docs/ROUTE_ENGINE_PLAN.md** (2026-10-22: two diagnoses, three designs, a judge). Step 1 built -fp..-fu, untested (GD gets its anticheat spike after every rewind - really from -fu, -ft's guard had made it a no-op -, so re-played ticks keep their slope launch; the jump back to the route stays within the repair floor; random ideas and route legs repeat run to run; the log says why a route search could not run). Step 2 (measure the simulator, three live checks) needs Nigel. Step 3 built -fv, untested (the route search in slices, no stutter; repeatable; calm: a 33 ms gap between input changes except on orbs, the calmest way in wins). Step 4 built -fw (gates name their options: orb use/skip, pad/portal touch/avoid; the next gate ranked from the player). Step 5 built -gc, untested: Route drives, a third setting of the route switch (Off / Route ideas / Route drives, saved; Off by default) where each decision is a leg to the next gate's option, played only to the gate; with it, a run stops after 5 minutes without getting further and says where, and the stop line gives clicks a second (average and busiest second). Steps 6-8 next. Was: The route engine drives (the plan's real R4: the whole-level route chooses, physics finds each leg's exact clicks, proven dead ends crossed off, jumps back to the cause). What's built only adds route ideas to the old picker, which still keeps whatever survives 4 seconds (Nigel: "it doesn't seem like an AI choosing when to click with knowing the whole level")
- [x] No straight-line hovering: the hold-a-height steers removed in -fg (120 clicks a second on the wave)
- [ ] ToE2: dies in a big-ship slope 'diamond' at x ~22940-23012 (no orb there); the gravity it brings comes from blue orbs at x 21825-22185, 2.6-4.5 s earlier, which today's 4 s look-ahead never presses (plan, diagnoses). Orb use/skip becomes a route decision (plan step 4)
- [x] Deadlocked: 100% from scratch, no macro, in 7 minutes (2026-10-23, build -ge; 16 dead ends, 3.9 clicks a second on average, 81 in the busiest second), and the saved path replays from the start to 100%. Two walls, both found live through Assistant Access: 22% was the simulation (fixed -gb): GD files every object in vertical section 0 (its y section factor is 0), and the World's object writer took that as 100-unit rows, so no moving object above y 300 ever moved in a simulated run - the pad that drops from the ceiling after the blue orb stayed up, and the orb always looked fatal (Nigel: "the block at 22 doesn't move and appear"); every level with moving objects up high had this. 54% was an upside-down robot falling up through a gap in the ceiling: the game only kills it about 700 ticks later, thousands of units up, so falling up looked like the longest survival within the 4 second look-ahead; it now counts as a death where it starts (-ge). Also -gd: the World's own start-up check sometimes failed (a GravityRestored event of no copy of the run) and turned the World off for the whole launch, so a run then played like the old engine; such events now stay out of the run (not confirmed yet by the failure going away)
- [ ] Coins: a "Collect coins" switch, off by default; each coin a gate the route must pass. After the route engine drives
- [ ] Re-run the review of builds -dm to -dq that got cut off
- [ ] The bar (Nigel, 2026-10-21, eased from every RobTop level + 16 of 20 online): RobTop's three demons (Clubstep, Theory of Everything 2, Deadlocked), The Nightmare, Bloodbath, LIMBO, Orbit, Heliopolis. Baseline 2026-10-21 (build -ff): Clubstep 100%, ToE2 78% (84% once), Deadlocked 22%; the other five not run yet. 2026-10-23 (build -ge): Deadlocked 100%

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
