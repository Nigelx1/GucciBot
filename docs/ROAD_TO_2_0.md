# Road to 2.0

Everything left before GucciBot 2.0, in Nigel's order (2026-10-19). Boxes get ticked as things land, with the build that did it. Nigel's in-game test is what counts as done for anything you can see in the game.

## 1. Credits (done)

- [x] New order, "Promo help" for aura, DashBlaze and ace, chagh for the converter, peony third (build -eg)
- [x] Nigel's wording: dihmaster500 tester, NaN GD the frame-window originator, Bogdaner09 open source, GWDdoS optimization help, ToastexGD foundation (build -eh); site live

## 2. Minor features and bugs (next, starting with the Mac crash log)

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
- [ ] Other bots' macros (.gdr, .xd, .json and the rest) show in the list and convert in one click
- [ ] Silicate .slc
- [ ] Old .brrr v1, rebuilt fresh (never from the old ToastyReplay-era code)
- [ ] The same formats as the site's converter, wherever possible
- [ ] .flp (#19)

### Calculate leftovers
- [ ] The result stays visible after a run
- [ ] Conflicting-mod warning in the menu (it's only logged now)
- [ ] An in-level progress box with Cancel
- [ ] Start-position warning
- [ ] Reorder bands, and Brrr/Bells buttons for band sounds
- [ ] Mute during Calculate
- [ ] Per-band images, pulse and sound packs (the site still advertises them)

### Render
- [ ] Presets saved by name, plus the one-press Showcase preset (8K60 lossless)
- [ ] A "render done" popup
- [ ] An "FFmpeg missing" banner with an Open Folder button
- [ ] Pixel format choice
- [ ] An in-render HUD
- [ ] Nigel's test of the FLAC fix and "Start from the beginning" (build -du)

### The rest
- [ ] Status bar: what the bot is doing (recording, playing, calculating, pathfinding, rendering), plus frame, TPS, speed and inputs
- [ ] Update checker, tried again (removed in 1.7 because it crashed the game on launch; this time it waits until the game is fully up)
- [ ] Trainer video import for the normal Trainer, with a file picker that can't crash
- [ ] Eclipse speedhack override
- [ ] Moving wave spam (#25)
- [ ] Bug reporters' follow-ups: #20 Android practice-death crash (likely the same bug as the Mac one, fixed in -ei; needs a phone test); #23 start positions and checkpoints; #11, #13 and the -103 error; macros ignoring inputs (#14, #8, #18); logs from the Calculate reports
- [ ] about.md, the README and the site match what ships

## 3. Calculate

- [x] Port audit against anticroom's original (docs/CALC_PORT_AUDIT.md): two port faults fixed (builds -ed, -ee), plus the end-of-level death (-ef)
- [ ] The strict count, plus "my suggestion is x frames" using Juice's Alignment-Independent method (credited), flagged in the frame editor
- [ ] Speed: progress and a time cap (resetting to checkpoints is 87% of the time)
- [ ] Markers in the ground (#26; the fix in build -dt is untested)
- [ ] The trail check being one frame off; recheck the 42,000-frame noclip runaway

## 4. AI pathfinder (AI_PATHFINDER_PLAN.md)

- [ ] A map that knows when blocks move and appear
- [ ] The AI trying every orb and toggle choice
- [ ] Building the map in slices, so the game stops freezing for about 9 seconds
- [ ] Re-run the review of builds -dm to -dq that got cut off
- [ ] The bar: Nigel's 3 levels, every RobTop level, 16 of 20 online levels, Mindcap's levels

## 5. Mac, iOS and Android parity (everything but the pathfinder)

- [ ] Rendering and Video Mode
- [ ] Under-162 TPS and High TPS Precision
- [ ] The trigger model in the look-aheads
- [ ] Seeded random teleports and shake
- [ ] Checkpoint placement

## 6. Standalone copy (Windows, no Geode)

- [ ] One codebase with a build switch: a loader, the hooks, a small stand-in for the parts of Geode GucciBot uses, and bindings for the new GD version

## 7. How-to, VirusTotal, GitHub replies

- [ ] A how-to (#23, "how do I open it")
- [ ] A VirusTotal report
- [ ] Replies to the rest of the GitHub issues (Claude drafts, Nigel posts)

## 8. Polish round

- [ ] A feature-by-feature test of the rebuild and the new menu
- [ ] Cosmetics: the Mega Hack-style skin, the animated backdrop, BrrrBot's snow, the old title bar and fades, a menu opacity slider
- [ ] Click sounds' background noise

## Release

- [ ] The new GD version: Geode support, or the standalone copy
- [ ] The trailer ships with 2.0
- [ ] Release day: master and engine-port-2.0 level with multiplatform, version bump, about.md/README/site/changelog, tag v2.0.0, the GitHub package as Latest, the site's Download link

## Not coming back

- The "Macro Saved" popup (removed at Nigel's ask)
- The old analyzer's settings and debug tools (anticroom's analyzer replaced them)
