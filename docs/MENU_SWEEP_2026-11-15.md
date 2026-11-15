# Menu sweep, 2026-11-15

Claude checked every control in GucciBot's menu against the code: for each switch, slider, button, field and hotkey, it found the code that reads it and checked that the code does what the label and hint say. It was done in six groups, each traced by one agent and re-checked by a second ("skeptic") agent that tried to prove each finding wrong. Then the surviving findings were fixed on a lane branch, the fixes were tested in a hidden test copy of the game (the "sweep" copy, never Nigel's game), and landed on master one build per fix.

## Headline numbers

- **682 controls checked**: Macro page, Compact Mode and hotkeys 128; Hacks, Settings and Themes 163; Calculate 112; Frame Editor, Macro Tools and Editor Tools 92; Trainers, Indicators and Click sounds 110; Pathfinder, Render, Credits and the overlays 77.
- **44 findings.** The skeptic pass confirmed 43 of them (a few with corrections) and was unsure about one (Hide attempts). The game test then showed that one was a real bug too.
- **The game test found 2 more:** Macro Tools > Edit a macro > Open was broken for every macro (fixed), and the Trainer page's macro picker has the same duplicate-name problem fixed elsewhere (not fixed, needs a decision).
- **Fixed:** 39 of the 44 fully, plus the Edit a macro bug. 29 of those 39 fixes, and the Edit a macro fix, were checked in the test game (for a hint fix, that means the new text was seen there). The rest can't be checked in a hidden test game: phone or Mac code, a file dialog, the level editor, or something only visible on screen.
- **Partly fixed or a note only, with a decision for Nigel:** 5 (hacks-settings-1, hacks-settings-3, trainers-sounds-5, pathfinder-render-1, macro-8). Details below.
- **Not fixed:** 1, the Trainer page's picker (new from the game test).
- **Nigel's five calls (answered 2026-11-15):** four built, reviewed, checked in the test game and landed as builds 2026-11-15-aq through 2026-11-15-at (Prevent death, RNG lock and its seed, Trainer attempts, the Trainer page's picker). The fourth call, the Agency map while you play, stays out of 2.0 at Nigel's word. So after the calls, hacks-settings-1, hacks-settings-3, trainers-sounds-5 and the Trainer page's picker are fixed; pathfinder-render-1 and macro-8 stay partly fixed or a note.
- **Landed on master** as 27 builds, 2026-11-15-d through 2026-11-15-al (other lanes' builds sit in between). Each one built on Windows and passed the offline test suites before it was committed.

The kinds of finding: WRONG (it does something other than it says), STALE (the label or hint is out of date), HALF (only part of it is wired up), UNSAVED (it's lost at a restart without saying so), PLATFORM (it's broken or unchecked on Mac or phones), CRASHY (it can crash the game).

"Checked in the game" below means Claude checked it in the sweep test copy, in a hidden game. No player has tried any of these yet.

## Findings and what happened to each

### Macro page, Compact Mode and hotkeys (128 controls, 8 findings)

| # | Control | Kind | What was wrong | What happened |
|---|---|---|---|---|
| macro-1 | Tick rate | WRONG | Every keystroke counted: typing 480 set the game to 4, then 48, then 480 TPS, and while recording each one went into the macro. | Fixed in build 2026-11-15-m (4ce540d). Checked in the game: typing 480 while recording gives one TPS change. |
| macro-2 | Show on death (hint) | STALE | The hint pointed at "the switch above", which is now Show Trajectory, not Show hitboxes. | Fixed in build 2026-11-15-al (a168913) (hint names Show hitboxes). Checked in the game. |
| macro-3 | Compact Mode: Load a saved macro | WRONG | A name saved under two themes showed twice with no tag, and both entries loaded the same file. | Fixed in build 2026-11-15-g (0a45e24). Checked in the game: two entries, one tagged (JupiterBot), each loads its own file. |
| macro-4 | Saved macros row menu: Calculate | WRONG | It ignored Legit only: it started a run that Legit only then cancelled. | Fixed in build 2026-11-15-h (b5497b9). Checked in the game: greyed out under Legit only. |
| macro-5 | Delete button on the Macro card | WRONG | It acted on whatever was typed in Name, so typing another saved macro's name deleted that file, then claimed "Save brings it back". | Fixed in build 2026-11-15-d (7f6fab4): it shows only for the loaded file and deletes that. Checked in the game. |
| macro-6 | Save | HALF | It never said whether it worked, and a name with / or : failed silently (..\x could even write outside the macros folder). | Fixed in build 2026-11-15-q (86e18b9): it refuses the characters Rename refuses and says Saved or why not. Checked in the game. |
| macro-7 | Mirror inputs / Mirror inverted | WRONG | It acted in 1-player levels too, so with Mirror inverted on every hold came out inverted. The game test showed it: a press at 3 and release at 23 were stored as press+release at 4 and release+press at 24. | Fixed in build 2026-11-15-i (2f1d20e): two-player levels only, as the hint says. Checked in the game: only the press at 4 and the release at 24 are stored. |
| macro-8 | Sub-tick clicks (CBF recording) | PLATFORM | Nobody has checked that sub-tick recording works on Mac or phones (it compares GD's press time stamps with GucciBot's clock), and the card said nothing. | Note only: an "untested off Windows" note in build 2026-11-15-al (a168913). The clock check itself came separately in build 2026-11-15-c (it logs one line on a device). **Still owed:** one recording on Android or Mac, reading the card's "placed in the tick" count. |

### Hacks, Settings and Themes (163 controls, 11 findings)

| # | Control | Kind | What was wrong | What happened |
|---|---|---|---|---|
| hacks-settings-1 | Engine > Prevent death | WRONG | Without Backwards stepping on, it can't step back, so it pauses after you're already dead. (With Prediction > Stop before a death on, it does stop in time.) | **Not fixed, a warning note only** (in build 2026-11-15-al (a168913)): the hint now says it needs Backwards stepping, or Stop before a death. To fix it for real (medium): keep the step-back ticks whenever Prevent death is on, or have Prevent death switch Backwards stepping on. Nigel's call which. **Nigel (2026-11-15): do it.** Fixed in build 2026-11-15-aq (6542e99): Prevent death keeps its own step-back ticks while it's on (same store and cap as Backwards stepping), and the warning is gone. Checked in the game: with Backwards stepping off, the player stops at frame 390 paused and alive instead of dying. |
| hacks-settings-2 | Gameplay > RNG lock | STALE | It only acts while recording; the hint didn't say so. | Fixed in build 2026-11-15-al (a168913). Checked in the game. |
| hacks-settings-3 | RNG lock and Seed | UNSAVED | Neither is saved, and nothing said so. | **Hint only** (in build 2026-11-15-al (a168913)): it says they reset at each launch. Whether they should be saved is Nigel's call (small either way). **Nigel (2026-11-15): save them.** Fixed in build 2026-11-15-ar (9a82617): both are saved like the other switches (the hotkey too), and the hint no longer says they reset. Checked in the game: a saved lock and Seed 4242 came back after a relaunch. |
| hacks-settings-4 | Engine > Auto flip on death | WRONG | It only works together with Prevent death; nothing said so. | Fixed in build 2026-11-15-al (a168913) (the hint says so). Checked in the game. |
| hacks-settings-5 | Macro path > Marker size, Line opacity | WRONG | Moving them did nothing on screen until the path changed (idle, paused, or playing a saved path). | Fixed in build 2026-11-15-ag (9ba6df8): the path redraws when they change. Not checked in the game (only visible on screen). |
| hacks-settings-6 | Prediction > Find the best tick | WRONG | It ignored Legit only and stepped the game anyway. | Fixed in build 2026-11-15-aa (0af8c38). Checked in the game: greyed out under Legit only. |
| hacks-settings-7 | Visual > Hide attempts | WRONG | The skeptic wasn't sure. The game test settled it: switching it off left the attempt label hidden until the next restart. | Fixed in build 2026-11-15-r (a382b47). Checked in the game: the label comes back as soon as the switch goes off. |
| hacks-settings-8 | Engine > Scroll speed fix | STALE | It only acts in renders with audio, with Lock delta on; nothing said so. | Fixed in build 2026-11-15-al (a168913). Checked in the game. |
| hacks-settings-9 | HUD > Tick limit readout hint | STALE | It used the old names Max UPR and Dynamic UPR. | Fixed in build 2026-11-15-al (a168913) (uses the Frame pacing card's names). Checked in the game. |
| hacks-settings-10 | Settings > Open the BIG BRRRR folder, theme editor's track | STALE | An audio file in that folder wins over every theme's own track, and nothing said so. | Fixed in build 2026-11-15-al (a168913) (both places say so). Checked in the game. |
| hacks-settings-11 | Prediction card's off-Windows note | STALE | Off Windows the note sat between two cards, and with Show Trajectory on the same note showed twice. | Fixed in build 2026-11-15-aa (0af8c38). Not checked: that code only builds off Windows (CI builds it). |

### Calculate (112 controls, 6 findings)

| # | Control | Kind | What was wrong | What happened |
|---|---|---|---|---|
| calculate-1 | Precision (L*) > Solve L* | WRONG | In a level with no macro playing and the readout on, it did nothing and the label stayed "(stale)". | Fixed in build 2026-11-15-x (5523736). Checked in the game: L* 123.619 over 68 inputs. |
| calculate-2 | Overlay > Text in the level | WRONG | Typing or deleting a window by hand shifted every later message onto the wrong input. | Fixed in build 2026-11-15-y (cdf72c5). Checked in the game: a message over frame 3503 stayed there when a window was typed and removed. |
| calculate-3 | Overlay > Show windows (and the readouts) | STALE | Windows only show while a macro plays; nothing said so. | Fixed in build 2026-11-15-al (a168913). Checked in the game. |
| calculate-4 | Calculate card > Clear (confirm) | STALE | The confirm said only "the measured windows", but it also wipes typed windows and the text in the level. | Fixed in build 2026-11-15-al (a168913). Checked in the game. |
| calculate-5 | Sub-tick counting > Search stride | STALE | It only matters with Bisect edges on (and Full range sweep off), but showed all the time. | Fixed in build 2026-11-15-al (a168913): it shows under Bisect edges, and the hints say Full range sweep turns bisecting off. Checked in the game. |
| calculate-6 | Calculate / At the playhead > Test, under Legit only | HALF | Calculate was greyed out with no reason; Test wasn't greyed out and said "Switch to playback mode first." | Fixed in build 2026-11-15-z (eeeaccd): both are greyed out and say Legit only is on. Checked in the game. |

### Frame Editor, Macro Tools, Editor Tools (92 controls, 6 findings)

| # | Control | Kind | What was wrong | What happened |
|---|---|---|---|---|
| macro-tools-1 | Macro Tools > Load it again | CRASHY | It could load a different macro while Calculate was running, and Calculate then wrote past the end of the new, shorter macro (a crash or a corrupted macro). The Macro page's rows had the same gap. | Fixed in build 2026-11-15-h (b5497b9): no macro loads while Calculate or a pathfinder runs. Checked in the game: "Calculate is running. Load once it has finished.", and Load it again is greyed out. |
| macro-tools-2 | Macro pickers (Diff, Trim, Merge, Edit) | WRONG | The same duplicate-name problem as macro-3. | Fixed in build 2026-11-15-j (a40cb7c). Checked in the game: each entry compares its own file (109 inputs vs 6). |
| macro-tools-3 | Import a macro (file picker) | WRONG | The picker's filter hid GucciBot macros saved under other themes (.juice, .solar and so on). On a Mac they couldn't be picked at all. | Fixed in build 2026-11-15-ah (764bba3). Not checked in the game (a native file dialog). |
| macro-tools-4 | Macro Buffing > Place spikes (refusal message) | STALE | The message named Silicate's labels, not the card's. | Fixed in build 2026-11-15-al (a168913). Not checked in the game (level editor only). |
| macro-tools-5 | Macro Buffing > Only around clicks / Click radius | WRONG | Every click's spikes were centered one tick late (GucciBot stores an input one frame later than Silicate). | Fixed in build 2026-11-15-ai (7b5dcbb), from the code. **I'm not sure of it until it's checked:** it needs one look in the level editor with Click radius 0. |
| macro-tools-6 | Frame Editor on phones | PLATFORM | Select this lane was only in a right-click menu and Select all only Ctrl+A, so phones couldn't reach either, and the hints talked about keys and the mouse wheel. | Fixed in build 2026-11-15-ak (ce8df64): buttons for both, and touch-worded hints on phones. Not checked: phone-only code (CI is its first compile). |

### Trainers, Indicators, Click sounds (110 controls, 5 findings)

| # | Control | Kind | What was wrong | What happened |
|---|---|---|---|---|
| trainers-sounds-1 | Click cue > Calibrate | WRONG | The hint said leaving the level stops a calibration; it didn't, and the cue stayed muted. | Fixed in build 2026-11-15-ab (615167e). Checked in the game: leaving the level stopped it. |
| trainers-sounds-2 | Video Mode switch | PLATFORM | Off Windows it could be switched on but could never work (no FFmpeg there). | Fixed by another lane first, in build 2026-11-15-b (2b8d900). The sweep's own commit for it wasn't landed. |
| trainers-sounds-3 | Click Trainer away from the level, on phones | PLATFORM | Only keyboard keys could click along on the bar, so phones couldn't. | Fixed in build 2026-11-15-aj (a089be3): a Tap button on phones. Not checked: phone-only code. |
| trainers-sounds-4 | Video overlay Pause/Resume and Restart | WRONG | While Video Mode follows the level these did nothing, and Restart wiped your recorded clicks for the run. | Fixed in build 2026-11-15-ac (56e2135): hidden while it follows the level, as the trainer card already does. Not checked in the game (Video Mode needs a video picked in a file dialog). |
| trainers-sounds-5 | Stats: Attempts | WRONG | Attempts only counted deaths, so it always equaled Deaths. | **Partly fixed** in build 2026-11-15-ad (a92476c): a finished run counts as an attempt too. Checked in the game: Attempts 0 to 1, Deaths 0. A restart without dying still doesn't count. Whether it should is Nigel's call (small). **Nigel (2026-11-15): yes.** Fixed in build 2026-11-15-as (fc312fc): a restart counts once at least one tick ran since the last reset; a death or a finished run closes its attempt, so it isn't counted twice, and reopening a level doesn't count the last one again. Checked in the game against GD's own attempt label: 6 attempts ended, 6 counted. Still not counted (GD counts them): a restart with no tick played, and leaving the level alive part way through. |

### Pathfinder, Render, Credits, overlays (77 controls, 8 findings)

| # | Control | Kind | What was wrong | What happened |
|---|---|---|---|---|
| pathfinder-render-1 | Classic > Agency map | WRONG | It only fills in during a search, then kept showing the last search's frozen result. | **Partly fixed** in build 2026-11-15-af (3b2da6c): the old result is cleared after a search and the hint says it shows during a search. Making it work while you play would mean a check on every tick outside a search (medium). Nigel's call. **Nigel (2026-11-15): fine to leave it out.** Not in 2.0. |
| pathfinder-render-2 | Classic > Minimum progress | UNSAVED | The slider goes to 240, but anything above 60 came back as 60 after a restart. | Fixed in build 2026-11-15-ae (24b1273). Checked in the game: 120 survives a relaunch. |
| pathfinder-render-3 | Classic search cover title | STALE | It said "Calculating..." during a pathfinder search. | Fixed in build 2026-11-15-al (a168913). Checked in the game: "Pathfinding...". |
| pathfinder-render-4 | Classic > Hide the search (hint) | STALE | The hint promised you'd see the confirming run; the cover stays up for that too. | Fixed in build 2026-11-15-al (a168913). Checked in the game. |
| pathfinder-render-5 | Status chip (IDLE / PATHFINDING) | WRONG | It said IDLE while the AI pilot was running. | Fixed in build 2026-11-15-u (1fce9bb). Not checked in the game (the test copy has no trained AI). An in-level status box for the AI wasn't built (optional). |
| pathfinder-render-6 | Engine and Search choices during a run | HALF | You could switch engine mid-run and start a second engine on top of the first. | Fixed in build 2026-11-15-v (ba4e619): one engine at a time. Checked in the game: the choices grey out while Classic or Absense runs. |
| pathfinder-render-7 | Switching GucciBot off while the AI pilot runs | HALF | The Disable bot switch stopped everything except the AI pilot, which froze and then resumed later on a changed game. | Fixed in build 2026-11-15-w (2a45365). Not checked in the game (no trained AI in the test copy). |
| pathfinder-render-8 | Render > Separate audio tracks (hint) | STALE | The hint left out the combined track and the frame-window sound track. | Fixed in build 2026-11-15-al (a168913). Checked in the game. |

### Found by the game test

| Control | What was wrong | What happened |
|---|---|---|
| Macro Tools > Edit a macro > Open | It said "That macro isn't there any more." for every macro (it emptied the pick it was given before reading it). This was on master too. | Fixed in build 2026-11-15-l (7868ba4). Checked in the game: Open loads the macro. |
| Trainer page > macro picker | The same duplicate-name problem as macro-3: a name saved under two themes shows twice, and both entries load the active theme's file. | **Nigel (2026-11-15): fix it.** Fixed in build 2026-11-15-at (d3fb558): the picker lists each file with its theme tag and loads that exact file, and the file's extension is saved beside its name (an old save without one still loads the active theme's file; a picked file converted to another theme still loads by name). Checked in the game, including after a relaunch. |

## What still needs a person or a device

- macro-tools-5: one look in the level editor with Click radius 0.
- macro-8: one recording on Android or Mac (the clock line from 2026-11-15-c, and the card's count).
- Phone-only and off-Windows fixes (macro-tools-6, trainers-sounds-3, hacks-settings-11): compiled first by CI; never seen on a device.
- pathfinder-render-5 and -7: need the AI pilot running (a trained network).
- hacks-settings-5, trainers-sounds-4, macro-tools-3: on-screen only, a video in a file dialog, or a native file dialog.
- Nigel's calls, answered 2026-11-15: 1 Prevent death keeps its own step back (2026-11-15-aq), 2 RNG lock and its seed are saved (-ar), 3 a restart counts as a trainer attempt (-as), 4 the Agency map while you play stays out of 2.0, 5 the Trainer page's picker lists each file (-at).
- By eye in a real launch: Prevent death stops before the first spike with Backwards stepping off; flip RNG lock and the seed, close and reopen the game, and both are still set; the Trainer pick comes back the same way.

## The controls that work

Everything below was traced to the code that reads it, and that code does what the control says.

**Macro page:** the setup check's It's fine and Warn again; Mode (Off, Record, Play); Name; Calculate; Game speed; Show hitboxes, Show Trajectory (Length, Moving objects, Move them every), Trail and Trail length, Line width, Fill, the 10 parts and colors, Reset the look; Hitborders (Show, Show on death, Hazards, Blocks, Slopes, Orbs pads and portals, Block borders, Hide used, Ground and ceiling, Rotated objects use your tilt, Player 2, Show yourself as a dot, Your path, Path length, Clicks, Line width, Fill, Darken the level, Darkness, One outline, Colors, Reset the look); Frame stepper (Pause, Step forward, Step back, Backwards stepping, Ticks kept); Block inputs during playback, Maintain gravity, Split the tick at each click and the sub-tick notes; Autosave at the level's end, Autosave on a timer and Every, Back up a macro before overwriting it; Saved macros (filter, Refresh, Open folder, Check macro, row click, Load, Convert to .brrr, Rename, Delete, Export, Open the exports folder, Import, Convert all, per-file Convert, Shift frames).

**Compact Mode:** mode, name, Save, Calculate, Tick rate, Game speed, Show hitboxes, Show Trajectory, Pause and the step buttons, Full menu.

**Hotkeys (all 20 slots):** binding, right-click and Escape to unbind, Open the menu, Pause, Step and Step back, Start/stop playback, Noclip, Show Trajectory, Hitboxes, Layout mode, No mirror effect, Prevent death, Hitborders, Audio pitch follows speed, RNG lock, Autoclicker, Record an intentional death, Auto flip on death, Mirror inputs, Safe mode, Compact mode.

**Hacks page:** Noclip and its flash; Safe mode; Legit only; Instant respawn, Auto retry, Respawn delay; Force platformer; Seed; Noclip accuracy (Show it, Die below, the readout); Layout mode; No mirror effect and Only while recording; Hide percentage; No death flash; Practice range; Macro path, Only up to your best, Show ahead; Frame extrapolation; Stop before a death; Good enough; Step through the tick and Steps per tick; the HUD (Show, all 24 readouts, Corner, Font, Size, Opacity); Lock delta; Audio pitch follows speed; High TPS precision; Input FPS; Frame pacing (Pacing, Ticks per frame, Scale with the tick rate, Target FPS); the Autoclicker (On, Only while holding, Player 1/2, Black orb UFO loop, Hold and Release ticks, Clicks per hold, Swift clicks, Copy to Player 2, status); Presets (Save, Load, Delete).

**Settings page:** the GucciBot switch; Check for updates and Pre-releases too; Auto safe mode, Green dot when clean, Hide the red dot, owner Unlock and Lock; Text size, Menu size, Cycle the accent color, Animations, Compact mode; Keybinds; Assistant Access; Diagnostics; BIG BRRRR (switch, Shake, Flicker, Open the folder).

**Themes page:** built-in and custom tiles; Edit, Delete, New custom theme, Open folder, Reload; the editor's Start from, Name, Macro extension, the colors, Corner radius, Opacity, Pulse, Snow, Subtitle, Brand, Badge, Quotes, Track, BPM, Drop starts at, Save, Save and use, Cancel.

**Calculate page:** Calculate, Cancel, Stop, Copy info, Clear; every Measurement setting (Preset, Algorithm, Recovery window, Search distance, Max window, Horizon, Slack, Full range sweep, Entry sweep, Dependent pair search, Tight threshold, Suggestions, Suspicious at, Move the one before, Joint setup sweep, Show setup range, Ship steering, Reaction time, the three release skips); Sub-tick counting (Count sub-tick windows, Input rate, Count every input, Bisect edges, Faster sub-tick search, Whole numbers on markers, Readout under, Tick-quantized ground); the Overlay (Show windows, Default Look, Bells, Markers, Band HUD and its size, Show desynced, Timing and Rate readouts, Setup suffixes, Include hold modes, Decimals, Marker radius, Label size, Circle skin, Text in the level, Play sounds, Volume); every band control (color, shown, edit, order, remove, add, sort, reset, window, label, sound, image, pulse and fades, shape and border, All Brrr, All Bells); L* (switch, Solve outside a level, Target and Respawn time, Nerve, Fatigue, CPS, the readout and its size); Windows by hand (Include releases, Set every window, the filter and paging, the table's Window field, Use, Use all, Clear, Delete this page's windows, Use my suggestion, Apply label, Test); Show advanced settings, Analysis visuals, Lock camera, Hide spawn effects, Leg trail, Turbo and its budgets, Frame budget, Tick batch, Time cap, Check past next input, 2-player fixes, Verbose log, Analysis overlay, Player state diff, Reset all; and every setting is saved.

**Frame Editor:** Undo and Redo (buttons and keys), zoom, Fit, All lanes, the overview strip, wheel zoom and scroll, click and Ctrl+click select, drag to move, drag an end, box select, double-click to add, Delete, Select this lane, the inspector (Press and Release, Frame, Nudge, Delete, Move by, Delete N, Clear selection, Add a click), the keyboard shortcuts, the lock note, Calculate's suggestion flags.

**Macro Tools:** Refresh; Diff (Moved within, Compare); Trim (From, To, Move it to the start, Trim); Merge (Gap, Merge); Edit a macro (Name in the file, Level, Starting TPS, the seed and saved labels, TPS changes, Save, Revert).

**Editor Tools:** Replace All; Macro Buffing (Record the path, Generate, Place spikes, Clear recording, the 17 wall fields and the 11 spike fields); the converter rows (Import, Convert all, Shift frames, Convert, Convert to .brrr, Export, Open the exports folder).

**Trainers:** the Click bar; Pause, Restart, Clear my clicks, drag to skim, Loop, Window, Perfect and OK within; the macro and best-attempt ghosts, Scrub and Point, JMF's Use it and Clear path; Segments (list, Add at the bar, Name, Starts and Ends at, Note, Jump the bar here, Delete, Loop segments, From and To, Turn on practice mode, Suggest, Copy code, Paste, Import, Notes); Reset stats; Synced music and Sync offset, Import and Remove track; Load, Refresh, Reload; Video Mode (Exit, the switch, Choose a video, Opacity, Sync offset, Nudge, the Alignment Tool).

**Indicators:** the Survival Indicator (switch, Style, Look-ahead, Opacity, colors, Flash on click); Click cue and per-mode cues, Calibrate, Reset, Cancel; Accuracy (Show in the level, Start over, Clear best streak); Click Indicators (all in-level, rhythm lane and scoring settings).

**Click sounds:** On, Pack, Test, Volume, Soft clicks, During playback, In renders, Background noise and its volume, a separate pack for player 2 and its Test, Open the clickpacks folder, Refresh.

**Pathfinder:** Engine; AI (Attempts, Time limit, Old practice rules, Start and Stop, Networks Reload); Absense (Start from the beginning, Follow the planned route, Exact start flags, Remember solutions, Start and Stop, Speed test, Show the level map, Show the planned route, Next gate Find a way); Classic (Start and Stop, Look-back, Restore points, Give up after, Hide the search).

**Render:** Start, Start from the beginning, Stop, Cancel, Render the next level I open, the last error, Open the FFmpeg folder; Presets (Showcase 8K60, Save, Load, Delete); Video (Width, Height, FPS, Codec, Pixel format, Bitrate, File type, Extra FFmpeg options); Audio (Include audio, Audio codec, Music, Sound effects and Trigger sound volume, Play audio while rendering); Timing (Fade in and out, Seconds after the end, Leave the level when done); Output (Folder, File name); the Intro card (every line, Font, the fades).

**Credits and overlays:** the Credits page (13 entries, matching about.md and the README); Calculate's in-level box and its Cancel; the Classic search's corner box and cover Cancel; Absense's status box; the switched-off window; the render-finished popup; the status bar's SAFE chip.
