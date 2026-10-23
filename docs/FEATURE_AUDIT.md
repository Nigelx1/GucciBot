# Feature audit after the menu rewrite (2026-10-18)

The menu was rewritten from scratch on 2026-10-02 (build -bz) when the ToastyReplay-derived code came out. This audit rebuilt the list of everything GucciBot has ever offered from our own records only: README, about.md, the website, release notes, commit messages and Claude's notes. The old menu code was never read. Every feature was then checked against the current source.

**337 features:** 256 in the menu, 21 in the code with no menu control (or a switch nothing reads), 52 missing, 8 duplicates.

A few of the missing ones were removed on purpose and are marked so. The rest is the rebuild list. File references are as of build -dx.


## Missing


### Calculate

- **Calculate algorithm: Alignment-Independent**: Juice's third algorithm: re-tests each click with the previous click shifted +-Z (Search Radius 1-30), with a continuation check and a Dominant Cluster Threshold. [In 1.6-1.7; went with GucciBot's analyzer in 1.8, 0c63836] *(Only dead leftovers: enum states AiBuildPred/AiSweepX and comments (GucciBot.hpp:1000-1008))*
- **Alignment-Independent results table**: Per-click table: macro vs representative window, range, alignments and sensitivity. [Went with 0c63836] *(Nothing)*
- **Branch playback (View / Go)**: Watch a tested branch play out (Go), with a View popup per branch. [Went with 0c63836] *(Nothing)*
- **Show Alignment-Independent results in the level**: Switch the in-level markers to Alignment-Independent's numbers, with blue X start-position marks. [Went with 0c63836] *(Only a dead field: fwOverlayShowAlignmentIndependent (GucciBot.hpp:884-890), read by nothing)*
- **Max Window (old analyzer)**: Hide windows larger than N frames. *(Only a dead field: fwMaxWindow = 25 (GucciBot.hpp:949); no .cpp reads it Closest now: each band's 'shown' switch on the Bands card hides that band's markers.)*
- **Position Tolerance (old analyzer)**: A survived shift only counts if it ends within Position Slack of the real path. [Went with 0c63836] *(Nothing)*
- **Max Frames Measured and Sim Speed (old analyzer)**: Two more Analysis Settings of the pre-1.8 analyzer (not described further). *(The old settings went with 0c63836 I'm not sure they map one to one. Closest now: Horizon (field maxFrames) and the Advanced > Speed settings (Tick batch, Frame budget).)*
- **Manual Frame Windows (old)**: Per-click rows (frame, player, press/release, %) where you type your own window, marked auto/manual, with Clear; Calculate kept them. Juice's idea; the site calls them the Trainer tab's manual entries. [Went with 0c63836, not brought back] *(No per-click manual rows Closest: Calculate > Advanced > Playhead labelling, one input at a time.)*
- **Calculating progress box**: Corner box with real progress and a Cancel button while Calculate runs. *(No in-level corner box with Cancel Progress and Cancel are on the Calculate page's Calculate card (calculate.cpp:150-154). The in-level overlay (framewindow.cpp:5229) is debug text shown only with Verbose log + Analysis overlay, with no Cancel.)*
- **Calculate start-position warning**: Warning about starting from a start position; the notes say nothing sets it now, and I'm not sure it ever showed. *(The Macro card draws gb->startPosWarning (shell.cpp:383-384), but nothing sets it (declared only at GucciBot.hpp:1290))*
- **Delay Marker Capture (old)**: Diagnostic switch that captured marker positions a tick later. [Went with 0c63836] *(Nothing)*
- **Debug Mode (old analyzer)**: Slow each test and mark it (green ring survived, red X died), with tolerance boxes, start-position X marks and a Debug Pause length. *(Nothing. Only dead declarations are left: src/core/GucciBot.hpp:1024-1039 (fwDebugMode, fwDebugSlowdown, FwDebugMark, debugPauseOrContinue), with no definitions and nothing reading them This went when GucciBot's own analyzer was replaced. anticroom's analyzer (src/analysis/ac/) can't slow down or mark each test. The closest thing now is Calculate > Advanced > Debug: Verbose log, Analysis overlay, Player state diff (src/ui/pages/calculate.cpp:659-672). Since 2026-10-23, Calculate > Advanced > While it runs > Leg trail (anticroom's antics-counter idea) draws each test in the level as it plays, green when it made it and red with an X where it died; it doesn't slow them down.)*
- **View Debug History + Go (old analyzer)**: List every tested frame and jump to one to try the click yourself. *(Nothing. Dead declarations only: debugTeleportToMark and debugTeleportToAiBranch (GucciBot.hpp:1039, 1173) Nearest feature: Advanced > Playhead labelling > Test (calculate.cpp:601-610). It measures the last N inputs before the playhead, then plays back to where you were. It does not list tests or offer Go.)*
- **Macro Saved popup (old)**: After saving a macro, a popup offered Calculate or Skip. [Removed at Nigel's request in 690030a] *(Nothing. The Macro page's Save button just saves (src/ui/shell.cpp:363-371), and a separate Calculate button sits next to it (shell.cpp:374-378) Removed on purpose at Nigel's request (690030a). Don't rebuild it.)*
- **Legend groups (old)**: Count several tiers on one legend row. *(Nothing. A dead legendGroup field remains in the unused old tier struct (GucciBot.hpp:1208-1230, fwTiers, which nothing reads) The Band HUD draws one row per visible band (framewindow.cpp:4956-4980).)*
- **Reorder bands**: Up/down arrows on each band (the first match wins). [Built -el; a Sort by window button added 2026-10-23, after anticroom's antics-counter beta.5] *(Up/down arrows on each band row in bandsCard (calculate.cpp:506-513), applied as a swap after the loop, and Sort by window next to Add band (calculate.cpp:571), smallest window first. The first match still wins (FrameWindowAnalyzer::tierForValue in framewindow.cpp), so order matters. Neither has been tried in-game yet.)*
- **Ring Boldness (old)**: Stroke thickness of the default double-ring marker. *(The plain ring is drawn at a fixed 4 px dark edge under a 2 px coloured line (framewindow.cpp:4637-4640), and no setting changes that The per-band Border / Line width doesn't apply to the plain ring. The old concentric double-ring default isn't there either.)*
- **Brrr and Bells buttons**: One click gives every band the Brrr or the Bells (a 'Use Bell Sounds' switch before a2f2775). *(Not on the Calculate page. applyBellSounds left the code in 385a5f7 (found with git log -S) The Bells are already what an empty Sound plays, so Reset bands brings them back. The Brrr can only be typed into each band's Sound by hand (fw_default.mp3).)*
- **Sound packs**: Name, import and export packs of band sounds. *(No pack code anywhere in src: no Pack Name, Import or Export Added in 8a9526c. git log -S shows 385a5f7 last touched those strings in src, so the packs most likely went with the old menu. Only the '<N>f SFX' naming survives, as the fallback clip name.)*
- **Import Sounds/Images (old)**: Copy your own tier sound or image files into fw_assets. *(Nothing copies files in. A band's Sound takes a typed path, looked up in the mod's config folder and then in resources (sound.cpp:40-46) You can still put a file in the config folder and type its name. main.cpp:49-62 still copies fw_assets/*.wav into the save folder, but nothing reads that copy. Images aren't supported at all.)*
- **Import Folder (old)**: Import a whole folder of frame-window assets at once. *(Nothing)*
- **Per-band image**: Use a custom image as a tier's marker. [Went with 0c63836, not brought back; the site still lists it] *(Nothing draws one. A dead imageFile field remains in the unused old tier struct (GucciBot.hpp:1211) The site still lists it (GucciBot Site/index.html L240).)*
- **Per-band pulse**: Pulse a tier's marker and/or text, with colour and your own fade-in, hold and fade-out times. [Went with 0c63836, not brought back; the site still lists it] *(Nothing. Dead markerPulse*/textPulse* fields remain in the unused old tier struct (GucciBot.hpp:1222-1227) The site still lists it (index.html L240).)*

### Click sounds

- **Click sounds background noise**: Optional background noise under the click sounds. *(Nothing plays a noise track. When a pack loads, files named *noise* are skipped (src/audio/clicks.cpp:144-153))*

### HUD/indicators

- **HUD branding overlay**: A corner overlay the old menu called displayOverlayBranding (contents not described). *(Nothing in src draws a branding or watermark overlay It lived in the old menu, and the sources I'm allowed to read don't say what it showed.)*

### Hacks

- **Instant Respawn**: Respawn straight away after dying. *(A switch named 'Instant respawn' sits on Hacks page > Gameplay card (src/ui/shell.cpp:526). Its only reader is src/hooks/hook_playlayer.cpp:521, which sets m_respawnTimer to 0 instead of 2 That timer is the engine's 2-frame step clamp right after a reset (hook_gjbasegamelayer.cpp:268-271), and it runs after the respawn. Nothing shortens GD's own wait of about 1 s between death and respawn; only Auto retry retimes it, at a fixed 0.5 s. I'm not sure what the version before the removal did. As written, the switch does not respawn you straight away.)*

### Macro

- **Macro list follows the folder**: The saved-macro list re-reads the replays folder by itself when files change. *(No folder watcher. reloadMacroList runs only at startup (engine_core.cpp:2473), after Save/Delete/Trim/Merge, and from the Refresh buttons refreshReplayListIfNeeded survives only in a comment (analysis/pathfinder.cpp:1584). So a file the Pathfinder saves, or one copied in, doesn't show until Refresh.)*
- **Open the replays folder**: Button that opens the replays folder in the file browser. *(No button. openFolder is used only for BIG BRRRR (bigbrrr.cpp:58), click packs (clicksounds.cpp:135) and custom themes (pages/themes.cpp:632))*
- **Rename a macro**: Rename a saved macro from a popup; its sidecar files are renamed with it. *(No rename code for macros in src/ (the only fs::rename calls are the sidecar migration at engine_core.cpp:550 and macro_ops' temp-file save))*
- **Per-macro '...' menu**: A '...' actions menu on each saved-macro row (Convert to .brrr, Calculate). *(Saved-macro rows are plain Selectables (shell.cpp:504) with no row menu)*
- **Convert a macro to .brrr**: From a macro's '...' menu, change a macro saved under another theme's extension (.juice, .icebrrr, .toosii...) to .brrr, sidecars included. *(No convert/re-extension code in src/)*
- **Calculate from the macro list**: From a macro's '...' menu, load it and run Calculate straight away (greyed out, with the reason, outside a level). *(No list-row action Nearest thing: click the row to load it, then Calculate on the Macro card (shell.cpp:376), which is greyed out with a reason outside a level.)*
- **Convert other bots' macros**: Macros in other formats (.gdr JSON or binary, .xd, .json, old .brr) showed as Incompatible rows; clicking one converted it to GucciBot's format. [convertToBRR removed in 37d97df, 2026-10-03] *(No converter or Incompatible rows. incompatibleMacros (.gdr/.xd/.json/.brr) is still filled (engine_core.cpp:1369) but nothing shows it)*
- **Legacy BRR macros**: Open and save macros in the old pre-GBR6 BRR (.brrr v1) format. [Dropped in 2.0.0-beta.3 with the ToastyReplay code; not rebuilt] *(core/brr_format.hpp:49-52: deserialize/loadFromDisk return nullptr and persist() is empty; load() still calls the stub (engine_core.cpp:1178) README.md L44 and about.md L33 still advertise it.)*
- **Legacy format converter**: Convert old .brrr v1 / legacy macros to the current format in one click. [Gone with the BRR reader] *(Nothing: it went with the BRR reader)*
- **Silicate .slc import**: Read Silicate v2/v3 macros. [Built and verified, then reverted at Nigel's call: native formats only] *(absense/compat/reference.hpp:3-5: 'GucciBot does not read .slc files (the site's converter does)')*

### Menu

- **MegaHack-style menu skin**: Alternate menu that looks like Mega Hack (tabs across the top, teal on the stock theme); Settings switch and hotkey. *(No other skin, no switch and no hotkey in src/ui; the key list has no slot for it (src/ui/shell.cpp:56-76))*
- **Menu background opacity**: General slider for how see-through the menu is (0.5-1.0, default 0.96); it is stored on the theme, so it may be the same control as the custom theme's opacity (I'm not sure). *(There is no general opacity slider, and the built-in themes have fixed opacity (src/ui/themes.cpp:221-261). The only control left is the custom theme editor's Opacity (src/ui/pages/themes.cpp:371) That editor slider is the 'Custom theme: opacity' entry. Nothing changes a built-in theme's opacity.)*
- **Status bar**: Shows in colour what the bot is doing (RECORDING, PLAYING, ANALYZING, PATHFINDING, RENDERING, IDLE), plus frame, TPS, speed and input count. *(The menu has no status bar Closest today: the in-level HUD's Bot state, Frame and TPS readouts (Hacks page > HUD card, src/hacks/hud.hpp:41-74) and the Macro card's 'N actions | TPS' hint (src/ui/shell.cpp:380-382). Nothing shows ANALYZING, PATHFINDING or RENDERING in the menu; the Pathfinder has its own small overlay (shell.cpp:1336-1350).)*
- **Animated backdrop**: A backdrop with ambient waves behind the menu. *(Nothing draws a backdrop behind the menu. The window background is the theme's flat colour (src/ui/look.cpp:187, 222))*
- **Old window chrome**: The old menu's title bar (name, version, diamond icon), drag-handle shimmer, open fade and sliding tab pill with hover. *(The window has no title bar (src/ui/shell.cpp:1205). The sidebar shows the theme name and version (1242-1252) and a fixed accent bar beside the selected page (1269-1273) No diamond icon, no shimmer, no fade on open, no sliding tab pill. Cosmetic only.)*

### Render

- **Render pixel format**: Pick the pixel format. *(No control. The renderer takes the first format the codec supports out of yuv420p, nv12, rgb0 and rgb24 (src/render/renderer.cpp:395-432))*
- **Render presets**: Save and reload full render configurations by name. [Came from ToastyReplay, removed in 0b9431f, not rebuilt; CLAUDE.md lists it to rebuild later] *(Nothing in src saves or loads render settings under a name Each render setting is its own saved value. CLAUDE.md section 6 lists presets to rebuild later.)*
- **Showcase render preset**: One press: 8K60, lossless x264 yuv444p, FLAC, music 1 / game SFX 0 / level SFX 1, thread count read from your CPU. [Sat at the top of Render Presets and went with them in 0b9431f] *(Removed with Render presets; nothing in src sets up the one-press 8K60 lossless configuration)*
- **Render done popup**: Popup when a render finishes. *(Nothing shows one. SLRenderer::publishRenderResult still fills m_lastRender for it (path, size, duration, pending = true; src/render/renderer.cpp:826-839, renderer.hpp:205-214), but nothing reads that record Only a failed start gets a notification.)*
- **In-render HUD**: A HUD shown while a render runs. *(Nothing draws one. renderedFrames() (src/render/renderer.hpp:219) has no caller, and GucciBot's HUD hides while a render runs (src/hacks/hud.cpp:275) With the menu open, the Render card shows a Rendering chip and Stop.)*
- **FFmpeg missing warning**: Banner with an 'Open FFmpeg Folder' button when the FFmpeg DLLs aren't loaded. *(No banner and no Open FFmpeg Folder button on the Render page; the page never checks isFFmpegLoaded() (src/render/renderer.hpp:226) Pressing Start without FFmpeg gives 'Render didn't start: FFmpeg not loaded' (renderer.cpp:331-332) as a notification and a note on the card.)*

### Settings

- **Update checker**: Popup on launch with a link when a newer release exists. [Removed in 8b463f4 (1.7): it crashed the game on launch] *(Left out on purpose: src/core/engine_core.cpp:2306 says 'NO UPDATE CHECKER HERE ON PURPOSE')*

### Trainers

- **JMF Trainer look**: The JMF page's own full-screen navy-and-gold look (sun, stars, wave ribbons, a pentagram/sparkle backdrop and a half-dome 'Wheel Thing' ornament), with GucciBot branding hidden. *(Nothing. The JMF page uses the normal theme, and nothing in src/ui draws a backdrop or hides the branding The site still describes it (index.html L597-602).)*
- **Video Mode: your own video**: Load and review your own video in Video Mode. [Removed at Nigel's request in 0b4eb84; the docs say built-in JMF video only since 80d3596] *(Only the bundled jmf_showcase.mp4 plays (videomode.cpp:41) Removed at Nigel's request in 0b4eb84 because its picker crashed.)*
- **Open JMF Video Folder**: Folder button for dropping in your own JMF video. [Removed when the video got bundled] *(Nothing No longer needed: the video ships inside the mod.)*
- **Video Mode Exit button**: Always-on-top button to leave Video Mode. *(Nothing draws one Probably not needed any more. The video is drawn under the menu on ImGui's background layer (videomode.cpp:414-417), so the card's switch stays visible. With the menu closed, though, there's no on-screen way out, only the menu key.)*

## In the code, but nothing in the menu reaches it


### Calculate

- **Calculate result shown after a run**: The result (and any conflict warning) stays visible after a run. [Lost at -bz: now the result vanishes] *(finish() keeps the result message in m_status (framewindow.cpp:3123), but the page draws status() only while running (calculate.cpp:151) After a run the note shows the stale start message 'Analysing N inputs.' (fwAcReport, engine_core.cpp:2743). The 'N measured, N skipped, N desynced' hint does stay (calculate.cpp:185-188).)*
- **Conflicting-mod warning**: Names frame-timing mods (CBF, other frame-window mods) that break measuring, before a run. [UI half lost at -bz; now only logged] *(GucciEngine::analyzerConflicts (engine_core.cpp:2683-2713) is only logged at run start (2739-2741); no UI reads fwAcConflicts The Macro page's 'Heads up' setup check names mods that hook checkpoints and other bots, but not CBF or the other frame-window mods on this list.)*
- **Mute during Calculate (old)**: Silence game music and sounds while Calculate runs. *(muteAnalysisMusic/unmuteAnalysisMusic (engine_core.cpp:2534-2560) are called only by the Classic Pathfinder (analysis/pathfinder.cpp:162, 1596) Calculate never calls them, and there is no switch.)*

### Hacks

- **No Mirror: Only Recording**: Apply No Mirror Effect only while recording. *(GucciEngine::noMirrorRecordingOnly (src/core/GucciBot.hpp:874), read in src/hooks/hook_gjbasegamelayer.cpp:880. No switch sets it and no saved key loads it It is always false, so No mirror effect applies to recording, playback and renders alike. Needs a switch under No mirror effect and a saved key in loadEngineSettings (next to engine_core.cpp:2403).)*
- **Force Platformer**: Play any level in platformer mode. *(Switch on Hacks page > Gameplay card (src/ui/shell.cpp:528), saved and loaded (shell.cpp:90). Nothing reads GucciEngine::hackForcePlatformer (src/core/GucciBot.hpp:683) A setting nothing reads, so the switch does nothing. The code that forces platformer mode is gone: this needs writing, not just wiring.)*
- **Custom respawn delay (respawn timer)**: Set your own delay before respawning. *(gucci::respawn::retime in src/hooks/respawn.cpp:17-41 (Absense's approach; accepts 0-10 s). Its only caller is Auto retry (hook_playlayer.cpp:794), which passes the fixed 0.5 s No delay control and no saved key. The 'same delay slider' that f8d084b mentions did not survive the menu rewrite.)*

### Macro

- **Mid-macro TPS changes**: Change the TPS during a recording; the change is stored in the macro and applied at that frame on playback. *(GucciEngine::recordTpsChange (engine_core.cpp:1507) has no caller. Playback applies TPS actions (hook_gjbasegamelayer.cpp:471); GBR6 saves and loads them (engine_core.cpp:1062-1066, 1159-1161) Typing a new Tick rate while recording (Macro page > Speed) only calls updater.setTps (shell.cpp:398), so nothing is stored in the macro. The only way to add one is afterwards: Macro Tools > Edit a macro > TPS changes (macro_tools.cpp:431-476). The code says the same: 'Mid-macro TPS changes (Macro Tools adds them)'.)*
- **CBF placed / on-edge counter**: Per-attempt count of presses placed inside the tick versus on its edge. *(LiveRecorder counts them: notePlaced/noteAligned and placed()/aligned() (replay/scbf_input.hpp:53-58, called at hook_gjbasegamelayer.cpp:428-448) Nothing reads placed() or aligned(): no menu, HUD or log shows the counter.)*
- **Theme tags in the macro list**: Each saved macro shows a coloured tag for the theme (extension) it was saved under. *(reloadMacroList still builds per-extension sets (jaMacros, juiceMacros, ... customThemeMacrosByExt; engine_core.cpp:1283-1366); the Saved macros list draws plain names (shell.cpp:499-506) Nothing in the UI reads those sets.)*
- **Import GDR macros**: Load GDR macros in JSON and binary (MessagePack) form (about.md still lists it; the Incompatible-row converter, convertToBRR, was removed in 37d97df). *(GDR JSON and MessagePack readers (engine_core.cpp:1518-2060) only read the bundled JMF trainer macro (engine_core.cpp:2083-2101) A user's .gdr is put in incompatibleMacros (engine_core.cpp:1369) and never listed. knownMacroExtensions has no .gdr, and load() reads only GBR6 (engine_core.cpp:1118-1178). about.md still lists it.)*
- **Show Macro Path**: Draw the macro's recorded path with click/release markers, with marker size and opacity sliders. *(analysis/macropath.cpp draws it when gb->showMacroPath is set (l.75), with macroPathMarkerSize/macroPathLineOpacity (GucciBot.hpp:727-729) Nothing sets or loads showMacroPath (default false), so it never draws. The path data is still saved (.path sidecar). Hacks > Visual > 'Practice range' is a different feature.)*

### Pathfinder

- **Pathfinder (Classic): search status**: Live readout while it searches: runs, depth, best frame and %, agency frames, probes, decision points, look-back, held-back and skipped counts. *(The page shows only best %, the stage and the run count (shell.cpp:666-669). depth, bestFrame, agencyFramesSeen, probeRuns/probeFails, lastPointCount, lastLookback, deferredCramped/deferredReplayed and skippedDeadEnds are all tracked (analysis/pathfinder.hpp:59-95, 226) but nothing shows them PARTIAL.)*
- **Pathfinder (Classic): cover while searching**: A full-screen 'Calculating...' cover (best %, progress bar, run count, Cancel) while it searches; switch it off to watch with a corner status box. *(The 'Hide the search' switch (shell.cpp:689) saves pf_hide_search (analysis/pathfinder.cpp:55, 68), but nothing reads it HALF-PORT: the switch only stores a value. Nothing draws a 'Calculating...' cover or a Classic corner status box (the corner readout at shell.cpp:1335 is Absense-only). The drawing has to be written.)*

### Render

- **Render colour fix**: Colour-space correction option. *(Switch at Render page > Video card > Colour fix (src/ui/shell.cpp:913), loaded into m_settings.m_colorFix (src/render/renderer.cpp:306, renderer.hpp:35). Nothing reads m_colorFix A setting nothing reads, so the switch does nothing (half-port).)*

### Settings

- **Diagnostics panel (self-check)**: Startup self-check summary (PASSED/FAILED) in Settings > Diagnostics. *(The self-check still runs at startup (src/core/engine_core.cpp:2508, into src/tools/selfcheck.hpp:38-120) and writes PASSED/FAILED to the Geode log. Its results list (gbcheck::g_results) is read by nothing, so no panel shows it)*
- **Log Frame Increments**: Diagnostics switch that logs every frame-counter step and checkpoint state to guccibot_frameinc.log. *(The logging into guccibot_frameinc.log is still there (src/core/engine_updater.cpp:68, 692, 793; src/hooks/hook_playlayer.cpp:523; src/core/engine_core.cpp:259). Nothing ever sets updater.m_logFrameIncrements (src/core/GucciBot.hpp:401): no switch, no saved key, no Assistant Access tool)*

### Themes

- **Theme identity text**: Each theme's bot name and subtitle in the title bar, brand word on the status bar, quotes on the Replay/Tools/Credits pages and a Credits badge. *(Data only. Each theme's subtitle, brand, badge and three quotes are in src/ui/themes.cpp:50-200 (and the custom editor), but nothing draws them. The sidebar shows only the theme's name and the version (src/ui/shell.cpp:1242-1252) No subtitle in a title bar, no brand word on a status bar, no quotes on any page and no Credits badge. The theme's 'title' field isn't used either.)*
- **BrrrBot snow**: The BrrrBot theme puts live snow over the menu panel. *(BrrrBot's snow flag is set (src/ui/themes.cpp:254) and copied around, but nothing draws snow (src/ui/themes.hpp:58 says 'nothing draws it yet') The comment on the Animations switch promises a falling-snow backdrop (src/ui/look.hpp:51), but that switch only eases control animations (src/ui/kit.cpp:16-29).)*
- **Custom theme: name, subtitle and brand tag**: The custom theme's name and its own subtitle and brand tag. *(Editor > Name and file card > Name (src/ui/pages/themes.cpp:342-347) works: it shows on the tile and as the sidebar heading. Editor > Words card > Subtitle and Brand (pages/themes.cpp:376-377) are saved to the theme's JSON (src/ui/themes.cpp:345-347), but nothing shows them The name part works. Subtitle and brand are settings nothing reads: there is no subtitle line and no status line.)*
- **Custom theme: quotes**: Three quotes with attributions for the Replay/Tools/Credits pages. *(Themes page > editor > Words card > Replay / Tools / Credits quote and By (src/ui/pages/themes.cpp:379-388). The editor previews them and they are saved, but no page shows a theme's quotes A setting nothing reads outside the editor's own preview.)*

### Trainers

- **JMF Trainer Mode**: The macro path and markers only show up to the furthest point you've reached, plus a lookahead buffer. *(The reveal code is in src/analysis/macropath.cpp:84-103 (trainerRevealEnabled and trainerRevealBuffer, GucciBot.hpp:731-732) Nothing reaches it. The macro-path overlay it limits only draws when showMacroPath (GucciBot.hpp:727) is on, and nothing ever sets it. Reveal on/off and the buffer have no controls either.)*

## Requested since the audit

- **Trainers: video import in the normal Trainer** (Nigel, 2026-10-18): play your own video in the any-macro Trainer, as JMF's Video Mode plays its bundled one. JMF's own "Choose a Different Video" was dropped on 2026-08-31 (0b4eb84) because its file picker crashed; build this one with a fresh, guarded picker.
