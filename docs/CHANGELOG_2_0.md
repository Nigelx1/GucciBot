# GucciBot 2.0: release notes (draft)

> Every item under "Since 2.0.0-beta.5" was checked against beta.5's source: "New" means beta.5 didn't have it, "Changed" means beta.5 had it and it works differently now, and "Fixes" were bugs in beta.5. "Not tested yet" means nobody has tried it in the game. "Checked in a test game" means Claude tried it in a test copy of the game through Assistant Access, but no player has yet.

---

## Coming from 1.8?

1.8 is the last stable release, and 2.0 is a big jump from it. In short:

- **A new engine.** The record and playback engine is a port of Silicate's, matched to it function by function. Practice checkpoints are saved whole, and Random triggers, plain and Advanced, pick the same groups on every replay of a macro.
- **GBR6**, a new macro format: about 2 bytes an input. 1.x's old `.brrr` macros convert in the game.
- **Other bots' macros, in the game.** It reads 27 formats (GDR, GDR2, Silicate, TcBot, yBot, xdBot, Mega Hack, .ttr3 and more) and writes 21 of them.
- **A new menu**, written from scratch. Every feature that came from ToastyReplay was taken out and rebuilt. Versions after 2.0.0-beta.2 contain none of ToastyReplay's code.
- **Calculate** is still anticroom's analyzer, now with **Windows by hand** (type in any input's window), a "my suggestion is x frames" for suspicious clicks (counted with Juice's Alignment-Independent method), Ship steering, presets, Default Look and more (below).
- **Pathfinder:** its default engine is Absent's search, from Absense (Engine: Search (old), then Search: Absense). 2.0 also adds an AI engine that learns, which is still early and doesn't ship trained yet (below).
- **Auto safe mode**, on by default, and now **Legit only**: one switch that turns off every cheat.
- **New tools:** Hitborders, the setup check, the Disable bot switch, CPS on the HUD.
- **Rebuilt from scratch:** Click Indicators (built around Bogdaner09's free mod) and Compact Mode.
- **Rendering** now runs on Silicate's renderer, with presets and a hardware encoder picked for your graphics card.
- **24 built-in themes**, plus your own.
- **macOS, iOS and Android** are supported, all from one download. Windows is still where it's tested most.

---

## Since 2.0.0-beta.5

### New features

- **Hitborders** (Macro page, under Hitboxes, with its own hotkey). Every hitbox is grown by your player's hitbox, so you become a point and the gaps shown are the room your center really has. It works out the current mode, size, gravity and tilt, and both players in a dual. The colors: red is where your center dies, blue is where you touch something solid, green is orbs, pads and portals. It's drawn the way NaN GD draws his: red over blue over green, used orbs, pads and portals still shown (a switch hides them), and the level darkened underneath (Darken the level). Your path and clicks show in yellow, and a dot for you is a switch, off by default. The shapes follow GD 2.2081's own collision rules. A probe compared every drawn edge with GD's own collision check in a test game, and they agreed everywhere it ran. Hitborders were NaN GD's idea too; he isn't involved. They follow the camera in renders too. *Checked by the probe in a test game, and in 640x360 and 1280x720 renders; not tested by players yet.*
- **Record from where a macro is** (#32). Switch Mode to Record while a macro plays, from the Macro page or Compact Mode, and recording carries on from that frame. The rest of the macro is cut, so you can add to a macro or redo its end. *Checked in a test game.*
- **Block inputs during playback** (Macro > Recording and playback), on by default, as in Silicate. In beta.5 your own clicks always reached the game while a macro played. Now they're blocked unless you turn this off. *Checked in a test game, with the keyboard.*
- **Scale with the tick rate** (Hacks > Frame pacing, Fixed), a new switch, on by default as Silicate has it (#30). It should stop the slowdown at very high tick rates on iPhone. *Not checked on an iPhone yet.*
- **Calibration:** the swing gets its own reaction time and cue switch. *Not tested yet.*
- **Settings > Diagnostics:** the startup self-check's results in the menu, with a pass or fail mark, what each failed check found, and the full list. It also says when the engine's checks were skipped (off Windows, or with GucciBot stood down or switched off), and checks again when you switch the bot back on. Before, the results were only in Geode's log. *Checked in a test game on Windows; the skipped-checks note and the check after switching the bot back on not seen yet.*
- **A new logo**, the iced cone. It replaces the old photo everywhere. It's in Geode's mod list (GBot shows its own logo there while it's the theme), at the top of the menu's sidebar, and on the pause-menu button on phones and Mac.

### Changed

- **Compact Mode** now has Calculate, a macro picker, speed and frame stepping. Before, it had only the mode, the macro's name and Save. *Not tested yet.*
- **Render intro card:** every line can be edited (Render > Intro card; leave a field blank to hide that line). Before, the text was stuck on anticroom's own. A level name too long for one line now takes two, broken at the space nearest the middle, so it clears the legend; a line shrinks only if it still doesn't fit. A name that fits on one line is drawn as before. *Checked in a test game, in renders at 1920x1080 and 640x360.*
- **Renders never show the cheat dot.** *Not tested yet.*
- **The setup check names a second GucciBot file** in your mods folder (#31). Geode lists that file as invalid, with a confusing message about an older version of Geode. The check now tells you which file to delete. *Checked in a test game.*
- **Import a macro** keeps a dated copy of what you import, opens the picker there, and selects a file you already imported. On Android it says what to do when the picker won't hand the file over. *Not tested yet.*
- **Settings are saved a moment after you change them,** not only when GD saves. *Not tested yet.*
- **One "Audio pitch follows speed" switch** (Hacks > Engine) replaces the two that canceled each other out (one in Hacks > Visual did nothing). *Checked in a test game.*
- **The Render card's start buttons are grayed out while FFmpeg isn't loaded,** since nothing can render then. Before, they could still be pressed under the warning. *Checked in a test game.*
- **GucciBot's log files** (guccibot_fw.log and guccibot_mcp.log, in the mod's save folder) start fresh each launch and keep the launch before as `.old.log`, with at most 50 MB a launch. Before, they grew forever: a test save's had reached 127 MB. *Checked in a test game, except the 50 MB cap.*

### Calculate (frame windows)

Many of these come from anticroom's Antic's Counter changelogs. They were rewritten here from his descriptions, since there's no source to port.

- **Presets:** Balanced (the default, the strict count), Accuracy and Absolute. *Not tested yet.*
- **Max window:** once an input's window reaches it, counting stops and the input shows N+. **Sweep range is now called Search distance.** *Not tested yet.*
- **Recovery Range:** three fixes. The next input moves with its release, offsets that can't play are skipped, and one shift's state no longer leaks into the next. Its range defaults to 2. *Not tested yet.*
- **Default Look is back** (Calculate > Overlay; it was in 1.7, and beta.5 had no switch for it): the overlay drawn like the counter in NaN GD's videos. You get a plain ring per input, the number on its left, his legend and the Brrr or his bells. Your own bands stay as they are. *Checked in a test game.*
- **Text in the level** (Calculate > Overlay): your own words over any input, with GD's color tags, saved with the macro's windows and kept through a Calculate run. Before, GucciBot could draw these messages but had no way to make one. *Checked in a test game.*
- **Bands:** decimal edges for sub-tick (CBF) windows, and a Sort by window button. *Sort by window checked in a test game; the decimal edges not tested yet.*
- **Windows by hand** (already in beta.5): the Every input table now comes in pages, and you can delete one page's windows. *Not tested yet.*
- **Set every window to [x] frames** (new, at the top of the Every input table): gives every input the same window in one go, the same as typing it into each row. It asks first if it would replace any window, and says a Calculate run won't undo it. Clear on a row puts Calculate's own count back. Set it to 1 for placeholder 1-frame windows you then edit by hand. *Checked in a test game.*
- **The Leg trail** (Calculate > Advanced > While it runs) draws each test run in the level. *Not tested yet.*
- **A canceled or failed run keeps the windows saved before it.** *Not tested yet.*
- **More accurate counts,** based on comparisons with Antic's Counter. *Run in a test game; not checked by players yet.*
  - After every restore, GD gets back the anticheat spike it expects. A leg that leaves a slope right after its checkpoint now gets its launch.
  - Test legs end one tick earlier, where anticroom's own legs end.
  - Sub-tick legs try Retime first, then Buffer, as Antic's Counter does. In a 2-player level, a Retime leg leaves the other player's inputs where they are.
  - Juice's check-back tries only the timings its answer needs. The first click on KOCMOC went from 402 s to 79 s, with the same results.
- **Ship steering, on by default** (Calculate > Measurement), based on anticroom's Antic's Counter (its beta.16): a ship click or release that kills you still counts if taps a person could make keep you alive as far as the normal test of that timing goes, the way you'd save it when playing. It doesn't check that you get back onto the macro's path after that. Reaction time (100 ms) is how long after the mistimed input before steering can start; late timings past it count the normal way. An input steering widened reads "Calculate, steered" in the Every input table, and Copy info says how many there were. Max window goes by the normal count, so a steered window can go past it. Switch it off to count ship like every other mode, exactly as before. On KOCMOC, with the settings of Nigel's own run, it widened 13 of 70 ship inputs, made none narrower and changed nothing outside ship. *Run in a test game, where switched off it matched the build before it exactly, and five steered timings were played for real; not checked by players yet.*
- **2-player fixes, on by default** (Calculate > Advanced > Experimental): 2-player levels are measured one player at a time, and a test runs to that player's next input. Windows in 2-player levels can come out wider or narrower than before. Switch it off for the old count (an older save that had it off loads it on too). Without it, a player 2 press that shares a tick with player 1's release can get a wrong early edge. *Checked in a test game on one 2-player macro.*
- **New and reworked switches, off by default** (your counts don't change until you turn them on). *Not tested yet.*
  - **Faster sub-tick search** (Sub-tick card): about a quarter of the test runs. A window with a gap smaller than a frame inside it can come out different.
  - **Check past next input** (Calculate > Advanced > Experimental).
  - **Dependent pair search** (Measurement card), already in beta.5, reworked: it only looks at tight inputs right after another one, measured the way Calculate measures them.
- **Smaller fixes:** UFO inputs no longer get the hold-mode mark. Click Between Frames isn't flagged while GucciBot has it paused. Calculate's notices fit on the screen. Its start message counts inputs the same way its report does. A look change mid-level redraws the markers already passed. In a dual, markers show on both players whatever an input's player flag says (#27). *#27 isn't confirmed with the reporter's macros.*

### Pathfinder

So it's clear what each change is, every item is labeled one of three ways. **Search** means trying moves in a copy of the game and backing up when they die. **Planning** means reasoning over the whole level. **Learning** means trained networks. The Pathfinder needs Windows.

- **Learning: the AI engine, still early.** The Pathfinder page now has an **Engine** choice: **AI** or **Search (old)**. It starts on **Search (old)**, as the Pathfinder always has. AI means trained networks play the level and practice it. The networks learn from human macros (as examples of normal play; no macro is followed at run time) and with reinforcement learning, playing levels in test copies of the game. **It isn't ready yet.** It doesn't beat the levels the search engine beats, and this download doesn't include trained networks, so picking AI shows "No trained AI yet" and Start stays grayed out.
- **Search: a much stronger search engine** (Engine: Search (old), then Search: Absense, the default). In Claude's unattended test runs it beat all five of these from a cold start, with no macro loaded and nothing remembered, and each result replayed: Clubstep, Theory of Everything 2, Deadlocked, The Nightmare and Bloodbath. LIMBO, ORBIT and Heliopolis are still out of reach. What changed:
  - It never reads a macro. Every search starts cold unless **Remember solutions** is on (off by default).
  - **Exact start flags** (on by default): when it tries again from an earlier moment, each portal, orb, pad and trigger counts as used or not exactly as the game had it.
  - It can fly the UFO properly. Before, it could never flap one.
  - The simulation of busy levels is much faster: LIMBO went from about 20,000 to 132,000 simulated ticks a second. Moving objects high up in a level now move in the simulation too.
  - Fewer clicks: a trim pass takes out taps the path doesn't need (ToE2 went from 13.9 clicks a second to 4.4 on average).
  - Fewer freezes, and every freeze is logged. A run that goes five minutes without getting further stops and says where.
- **Planning: Route drives.** Follow the planned route was an on/off switch in beta.5. It's now a choice: Off / Route ideas / Route drives (Off by default). Each decision aims at the planned route's next gate, and the search finds the clicks into it. It got through The Nightmare in one October test run, from before every search had to start cold, so that result isn't confirmed. It's still experimental, and it isn't the full whole-level AI that was planned. *Not tested by players yet.*
- **The level map** shows where objects really are rather than where their sprites are drawn, and it follows teleport portals to their exits.

### Safe mode and Legit only

GucciBot's safe mode follows what the Demonlist allows, strictly. The aim is simple: nobody should be able to fake a completion with GucciBot.

- **Legit only** (Hacks > Gameplay): one switch that turns off everything that counts as a cheat and keeps it off. Turn it off and your settings come back as they were. While it's on, a macro can't play, record or render, Calculate, the Pathfinder and frame advance won't run, and the game stays at 240 TPS and 1.00x speed. *Checked in a test game, except its hotkeys; not tested by players yet.*
- **New cheats** (the attempt doesn't count while they're on): recording a macro, live hitboxes and hitborders (the "on death" versions are still allowed), the HUD's Touching orbs and Time warp readouts, a trainer page's click bar while it follows its own level, and the Pathfinder's planned route and next-gate drawing. *Checked in a test game.*
- **The cheat mark shows on the end screen too,** as Mega Hack's does. *Checked in a test game.*
- **Plain play gets GD's own random.** GucciBot fixes the random seed only while a macro records or plays, or Calculate runs. *Checked in a test game.*
- **The Safe mode switch works** (Hacks > Gameplay). In beta.5 it did nothing. Now it keeps a run from counting, cheat or not. It also leaves stats, achievements, attempts and jumps alone, and stays on across launches. *Checked in a test game, except pressing the switch itself (the test set it through its saved setting).*
- **An editor level finished under safe mode no longer says "Level Verified!"** Safe mode already kept GD from verifying it, but the end screen still said it had, and a recording could pass that off as real. Now it says "Safe mode was on: this run didn't verify the level." *Checked in a test game on Windows.*
- **A run with a cheat or the bot on never verifies an editor level,** even with auto safe mode off. Recording counts, as it does for safe mode. Before, with auto safe mode off, such a run verified the level. The end screen says "A cheat or the bot was on: the level wasn't verified." A clean run with safe mode off still verifies. *Not tested yet.*
- Auto safe mode counts the Survival Indicator, since it shows the way.
- The Click sounds page notes that the Demonlist doesn't accept added click sounds as click audio.
- **GucciBot stands down while Antic's Counter is enabled,** as it does for ToastyReplay Lite and Silicate. It says why and leaves the game alone for that launch. Turn the other one off and restart. *Not tested by players yet.*

### Themes

- **Four new themes: RondoBot, SolarBot, JupiterBot and GBot.** *Checked in a test game's screenshots on every page.*
  - **RondoBot** (`.rondo`): Celtics green and white, on the TD Garden's parquet.
  - **SolarBot** (`.solar`): sun yellow and deep orange, with Janet looking in from the corner and the Orrery behind the menu.
  - **JupiterBot** (`.jupiter`): the JMF Trainer's yellow on its new night sky, on every page. The JMF Trainer page wears the same sky.
  - **GBot** (`.gbot`): a disguise. The menu says GBot, the Themes page moves into Settings, and the JMF Trainer page, the Credits page, BIG BRRRR and the macro list's theme tags all hide. Imports take `.gbot` too. Geode's mod list still shows the real name.
- SolarBot, JupiterBot and GBot have no BIG BRRRR. It comes back with any other theme.
- **BIG BRRRR switches to the new theme's song** if you pick a theme mid-drop. This is why LemonadeBot's and YoungstaBot's songs seemed not to work. *Checked in a test game.*
- **BrrrBot's snow** falls behind the menu while Settings > Look > Animations is on. Custom themes get a Snow switch. *Checked in a test game.*
- Turning Animations off also stops the accent's pulse. The JMF Trainer page no longer shows the theme's name, subtitle, tag or quote. The sidebar's logo mark is bigger, easier to see and takes the theme's color. JupiterBot's SAFE chip is green.
- The menu fits in a short game window.

### Fixes

- **The Classic Pathfinder no longer saves over your macro.** With a macro loaded, a solved search used to replace that macro's file. The solution now goes beside it as "name pathfinder". *Checked in a test game.*
- **Recording no longer writes extra presses.** When one tick held a release and a press, it was recorded as five events instead of three. The extra buffered press could then fire the next orb early. **Macros recorded with 2.0.0-beta.2 through beta.5 may contain these extra presses.** Re-record any that misbehave.
- **Reopening a level with random triggers no longer crashes** the game or quietly damages its memory.
- **Playback no longer ends early** when you respawn at a checkpoint, or step back, onto a click's frame. *Checked in a test game.*
- **Flip 2-Player Controls:** with it on, GucciBot pressed or read the wrong player in places. It should now get the right player in the autoclicker, auto flip on death, Mirror Inputs, the practice restore, CPS, the Survival Indicator, Click Indicators and the markers. It also fixes Maintain gravity (with the option on in a 1-player level, recording with Maintain gravity made no input at all) and Show Macro Path (a macro recorded with the option on drew no click markers). It follows GD, which flips a button's side only in classic 2-player levels. In a platformer, a practice respawn gives each player back its own held buttons. *Maintain gravity in a 1-player level and the macro path were checked in a test game; the rest is not tested yet.*
- **Maintain gravity in a 2-player level** now drives player 2 too, from its own key (after Flip 2-Player Controls and Mirror Inputs). Before, player 2's clicks were dropped while it was on: player 2 never moved and nothing was recorded for it. With Mirror Inputs on too, both players are mirrored the same way. Player 2 counts the gravity it starts the dual with as upright (GD usually starts it upside down), so with no key held it acts like a normal dual player, and practice respawns keep that. Platformer levels are unchanged. *Checked in a test game.*
- **Record from Off with a macro still loaded** starts a new recording from that frame, and a button you're holding at the switch is recorded too. Before, nothing was recorded until you restarted or died, and the old macro's inputs stayed. *Checked in a test game.*
- **Recording on levels with shake, teleport or Advanced Random triggers:** a restart while recording now seeds them from the seed the macro saves, as playback does, so the replay should draw the same as the run. Before, they were seeded from the previous attempt's seed. *Checked in a test game for Advanced Random; shake and teleport not tested yet.*
- **Plain Random triggers replay the same.** A macro through a level's plain Random triggers could pick other groups on each replay, so where a pick changes the path, the replay could go another way. Now each pick comes from the macro's seed while a macro records or plays, so every replay picks what the recording did, after a practice respawn or with the level opened again too. Advanced Random already replayed the same. Plain play still gets GD's own random. *Checked in a test game: after Record and a restart, Record from Off with no macro loaded, a practice respawn, and the level opened again.*
- **A restart after a playback that ended with player 2 holding** lets go of player 2. Before, in a 2-player level, player 2 stayed held through a restart in Off or Record, and Record from Off then recorded a press nobody made. *Checked in a test game for Record from Off.*
- **Click sounds** play as the player the click moves, not by its button side. *Not tested yet.*
- **Autoclicker:** "Only while holding" hears player 2, the mouse and touch. Runs over 65,535 clicks save whole. *Not tested yet.*
- **Rendering:** a PC with a Ryzen processor and a GeForce card no longer gets AMD's encoder (#3). An encoder that won't open falls back to libx264. With Separate audio tracks on, the combined track keeps the music and sound effects. Each render frees its graphics memory. *Not tested yet.*
- **A render can't outlive its level.** Pressing Start while a level was closing started a render that never stopped: the Render card said "Rendering" with a frozen frame count until you pressed Stop. Now it says "The level is closing" and doesn't start. *Checked in a test game.*
- **Showcase renders** (lossless RGB) play back in the right colors. They were tagged as YUV, so players showed a teal background and a green ground. *Checked in a test game.*
- **The Render card's frame count** counts up while it renders (it always said 0), and says when it includes the intro card. The same fix stops a second render in one launch from reusing a frame's memory before a slow encoder has read it (found by reading the code, never seen in a video). *Checked in a test game.*
- **File names** with characters outside English now work for videos, render folders and macros. A macro whose name Windows can't spell no longer crashes the game at launch. *Not tested yet.*
- **Trainers:** the macro and best-attempt ghosts show only in the trainer's own level. The best ghost lines up with the run, and Reset stats clears it. *Not tested yet.*
- **No death flash** hides the bright circles GD's death effect adds. The shattered icon stays. *Checked in a test game.*
- **Practice:** deleting a checkpoint no longer leaves it behind for backwards stepping. *Not tested yet.*
- **Macro Buffing** records spider snaps again. *Not tested yet.*
- **Saving:** a new recording no longer takes the old macro's frame windows. Backups keep a macro's frame windows with its clicks. Autosave names a macro with the theme's extension and skips one with no name. Macros saved under a custom theme show in the list after a restart. *Not tested yet.*
- **Macros with one name under several themes** (zz.juice and zz.solar) get a row each, with its own theme tag, and each row's menu acts on its own file. Before, they all showed one theme, and a click on one row could act on another. *Checked in a test game.*
- **Backups keep the macro's own extension** (zz.juice.bak). Before, every backup was named .brrr.bak, so backups under different themes overwrote each other. *Checked in a test game.*
- **The Saved macros list** reads the folder again when you open the Macro page or the menu, so a macro copied in before you first opened it shows up. *Checked in a test game.*
- **Calculate** gives the music and sound effects back when a run ends. Before, with its Mute while it runs switch on, the game stayed silent afterward. *Checked in a test game.* The switch itself is gone: Calculate always mutes the game while it runs, so it did nothing.
- **Presets** refuse names they can't save, and say when a save fails. *Not tested yet.*
- **Hide percentage** (Hacks > Visual) now hides GD's percentage label. In beta.5 it did nothing. *Not tested yet.*
- **Video Mode:** Opacity, Sync offset, Nudge and the Alignment Tool can be reached again, in a small "Video settings" window over the video. *Not tested yet.*
- **Hotkeys:** every hotkey toggle (except RNG lock) now saves its setting, the same way its menu switch does. *Not tested yet.*
- **Update checker:** its crash guard no longer switches it off by mistake. If you had it off, turn Check for updates back on in Settings. *Not tested yet.*
- **The start-position warning** can show now: a run started from a start position is marked when it's saved. *Checked in a test game.*
- Geode's log no longer fills up with lines from every tick, or with Calculate's step-by-step detail, which goes to guccibot_fw.log now. *Checked in a test game.*
- Text across the menu was fixed so it says what each setting really does, in American spelling.

### Platforms

One download covers Windows, macOS, iOS and Android (32 and 64-bit). Windows is home and gets the most testing. The rest is experimental.

- **Intel Macs** are supported (the Mac build now covers both Apple silicon and Intel). *Not tested on an Intel Mac yet.*
- **Seeded random teleports** and **checkpoint placement** now work off Windows too, so macros through random teleports should replay on Mac and phones. *Not tested on a device yet.*
- High TPS precision is grayed out off Windows, with a note, since it does nothing there.
- On iOS, Settings > Keybinds no longer offers keys, since no key reaches GucciBot there.
- **Still Windows only:** the Pathfinder (all of its engines), rendering and Video Mode, tick rates under 162 TPS and High TPS precision, and the trigger model behind the look-aheads.

### Credits

The credits on this page, the README, about.md and the in-game Credits page match: **Nigelx1, Claude, Juice, anticroom, dihmaster500, DashBlaze, Isaiah, Absent, Bogdaner09, GWDdoS, ToastexGD, C0nscious and Gucci Mane.** Claude wrote essentially the whole codebase, these notes included. Nigelx1 created GucciBot, owns it and makes every call. Since beta.5, a few people asked to come off the credits, and they have. The whole frame-window trend started with NaN GD, and the L* precision number GucciBot puts on a macro comes from his formula. Hitborders were NaN GD's idea too; he isn't involved.

### Get it, and get help

Download `nigelx1.guccibot.geode` from the release and install it with Geode's **Install From File**. Turn off ToastyReplay Lite, Silicate and Antic's Counter first. On Windows the menu opens with **Left Alt**, and on a Mac with **Option**. On phones and Mac there's also the logo button in a level's pause menu.

Questions: [the Discord](https://discord.gg/m95NUPdbpJ). Bugs: [open an issue](https://github.com/Nigelx1/GucciBot/issues/new/choose) with Geode's crash report.
