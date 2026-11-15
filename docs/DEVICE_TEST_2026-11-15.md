# Device test, Sunday 2026-11-15 (Mac and Android)

About an hour, on your Apple-silicon Mac and your Android phone. Nothing here needs code. Do the round on both unless a step says Mac or Android only. Keep **safe mode on**, as usual.

## 1. Put the build on

The build is GitHub's package of commit **`1975ec7c2c2513f91bfa894e0dca44d76d785e8a`** (build **2026-11-15-al**, with today's menu sweep in it). It's one file for every platform. Its build run: https://github.com/Nigelx1/GucciBot/actions/runs/43730050933

1. Open https://github.com/Nigelx1/GucciBot/actions/runs/43730050933 (the **Multi-platform build** run for that commit, green check), scroll down to **Artifacts** and download **GucciBot (all platforms)**. You need to be logged in to GitHub. It's a zip of about 200 MB. Unzip it to get `nigelx1.guccibot.geode`.
2. In the game: Geode's mod list > the gear > **Install From File** > pick the file > restart the game. This is the easy way on both.
   - By hand instead: **Open Mods Folder** in the same place shows the folder. On a Steam Mac it's inside the app: `~/Library/Application Support/Steam/steamapps/common/Geometry Dash/Geometry Dash.app/Contents/geode/mods`. On Android (Geode's launcher) it's `Android/media/com.geode.launcher/game/geode/mods`.
3. Check you have the right build: near the top of Geode's log (below) there's a line starting `[GucciBot] BUILD: 2026-11-15-al`. A later build from today is fine too (-am, -an and so on come after -al); it must be 2026-11-15-ak or newer to have steps 9 and 10.

## 2. The logs to send

Everything this round checks is written to **Geode's log**: the newest file in the `logs` folder next to `mods` (Mac: `.../Geometry Dash.app/Contents/geode/logs`; Android: `Android/media/com.geode.launcher/game/geode/logs`). After a crash, also send the newest file in `crashlogs` in the same place. Send them after the whole round, one set per device.

## 3. The round

| # | What you do | What "good" looks like |
|---|---|---|
| 1 | **Teleports.** Open a level with a random teleport portal. Record a macro through it, then replay it twice. | Both replays go the same way as the recording, through the same exit. The log has one line starting `Random teleport: member ...`. Bad: the replays split up, or the log says `Random teleports can't be seeded on this platform`. |
| 2 | **Checkpoint placement.** In practice mode, record over 3 or 4 checkpoints, dying once or twice on the way. Then replay it from the start. | The replay finishes like the recording did. The log has one line starting `Checkpoint request through ...: placed in the frozen tick`. |
| 3 | **Mac only: the checkpoint-respawn crash** (fixed in build -ei). Practice mode, place a checkpoint, then die and respawn about 10 times, at a few different checkpoints. | No crash. This is the crash you hit in October. |
| 4 | **Android only: GitHub #20** (the practice-death crash). The same as step 3, on the phone. | No crash. Probably the same fix as step 3. |
| 5 | **The per-tick check.** Play any level for a minute or two, with the bot on. | Nothing shows. If a red notification says `... steps in one tick at ... TPS, please report`, macros made on this device would number frames differently from Windows. Note when it came up. |
| 6 | **Calculate** (the page says "untested on this platform"). Record a short macro (a few clicks), then run Calculate on it. | It finishes without a crash, and the windows look like normal ones: a few frames each, not all 0 or 1 frame. A screenshot of the results helps. |
| 7 | **The sub-tick clock line.** In any level, click or tap a few times (bot on or off). | The log has one line starting `SCBF clock check`. It ends in **MATCH** (good) or **NO MATCH** (sub-tick recording would record nothing on this device; Claude fixes it from the numbers in that line). Just send it either way. |
| 8 | **Video Mode** (the Video Mode cards under the JMF Trainer and the Trainer). Just look, nothing to set up. | Each card has a warning, `Video Mode is Windows-only for now`, and its switch can't be turned on. Bad: the switch turns on and ends on a red failure. On the Mac build from section 4 (2026-11-15-bg or newer) the Mac has no warning any more: there, Video Mode can be switched on. On Android the warning says `Video Mode is Windows and Mac only for now`. |
| 9 | **Android only: the Click Trainer's Tap button** (build -aj). On the JMF Trainer or Trainer page, load a macro, turn on the Click bar, and don't open a level. Hold and let go of the big **Tap** button under the bar along with the macro's clicks. | Each tap shows up as a line under the macro's clicks, the way the space bar does on a computer. In a level, playing works as before. |
| 10 | **Android only: the Frame Editor by touch** (build -ak). Open a macro through Macro Tools > Edit a macro > Open, and tap one of its bars. | The panel for the selected bar has **Select this lane** and **Select all** buttons, and both work. The hints talk about tapping, not Ctrl, right click or the mouse wheel. |

If something goes wrong, keep going with the rest if you can, and say which step it was.

## 4. Mac only: rendering (a newer build)

Rendering works on the Mac since build 2026-11-15-bg. This part needs a newer build than the one in section 1: GitHub's package of commit **`a042169fcfaf979b57f5e9455c87c01ef922f232`** (it has builds -be, -bf and -bg in it). Its build run: https://github.com/Nigelx1/GucciBot/actions/runs/43758435429

1. Install it the same way as in section 1 (download **GucciBot (all platforms)** from that run's Artifacts, unzip, Install From File, restart). Near the top of Geode's log the `[GucciBot] BUILD:` line should say 2026-11-15-bg or newer.
2. Open the **Render** page. Good: no red "FFmpeg isn't loaded" note, and the Start button isn't grayed out. If there is a red note, take a screenshot of it, skip the rest and send the log (below).
3. To find the videos easily, type a folder you can see into Render > **Folder**, for example `/Users/<your name>/Desktop`. (Left blank, videos go in GucciBot's `renders` folder inside the game's save folder.)
4. **Render 1, the default settings.** Leave every render setting as it comes (if you've changed any on the Mac before, empty **Codec** and set **Width** and **Height** back to 1920 and 1080). Open **Stereo Madness**, press **Start** on the Render page, play about 10 seconds (dying is fine), then press **Stop**.
5. **Render 2, Showcase.** Press **Showcase: 8K60 lossless** on the Render page, then set **Width** and **Height** to 1920 and 1080 so the file stays small (8K works too, it's just slow and big). Render about 10 seconds of Stereo Madness again, the same way.
6. Open both videos (QuickTime plays the first one, an .mp4; the Showcase one is an .mkv, which QuickTime can't open: use VLC or IINA).

| Check | What "good" looks like |
|---|---|
| The video plays | Both files open and play to the end. |
| Picture | The level, the whole screen, the right way up, in normal colors. Bad: black, frozen, upside down, garbled colors, or only part of the screen blown up to fill the frame. |
| Sound | You hear the music in both. |
| Size | Both are 1920x1080 (QuickTime: Window > Show Movie Inspector; VLC: Tools > Media Information > Codec). Bad: a different size, or the picture squashed into a corner. |
| Length | About as long as you played. |

**If something fails, send:** Geode's newest log (see section 2: the `logs` folder next to `mods`). The render lines in it start with `[RENDERER]` and `[GucciBot] Video encoder`, `[GucciBot] Render shaders` and `[GucciBot] Render stopping`; one line starts `[RENDERER] mac view:` and gives the sizes Claude needs if the picture is the wrong size or zoomed in. After a crash, also the newest file in `crashlogs`. A screenshot of anything red on the Render page helps too.

## 5. The R restart and the Pathfinder (a newer build)

Builds 2026-11-15-bh and -bj came after section 4. This part needs GitHub's package of commit **`6265e7c14a40833f816b62f7fe67f34fa0302c0c`** (build **2026-11-15-bj**; it has everything in sections 1 to 4 too). Its build run: https://github.com/Nigelx1/GucciBot/actions/runs/43760412885

1. Install it the same way as in section 1 (download **GucciBot (all platforms)** from that run's Artifacts, unzip, Install From File, restart). Near the top of Geode's log the `[GucciBot] BUILD:` line should say 2026-11-15-bj or newer.
2. Keep **safe mode on**, as always. Both checks below go in Geode's log (section 2).

| # | What you do | What "good" looks like |
|---|---|---|
| 11 | **Mac only: hold through a restart** (build -bh). Open a level in normal mode with the bot off. Hold the mouse button down and keep holding it, press **R** to restart, and still don't let go. | The player keeps jumping after the restart, as if you had pressed again, until you let go. Bad: it stops jumping after the restart until you click again (that's how it was before). The log has one line starting `[GucciBot] Restart key: kept the held touch through UILayer::handleKeypress`. If the hold drops and that line isn't there, the Mac's R key goes another way: say so and send the log. |
| 12 | **Mac and phone: a cold Pathfinder run on Stereo Madness** (build -bj). The Pathfinder page now says `Untested on this platform` at the top, and Start works. Open **Stereo Madness** with no macro loaded. Open GucciBot's menu > **Pathfinder**, set **Engine** to **Search (old)** and **Search** to **Absense** (the defaults), leave **Remember solutions** off, and press **Start**. Leave it alone; on Windows it takes about 2 minutes, a phone may take a lot longer. | It plays the level by itself, stopping and starting while it thinks, until it reaches 100%. Then it saves a macro named after the level, and replaying that macro finishes the level too. The log has a line starting `[GucciBot]: [AbsPF] stopped: level complete`. Bad: a crash, it gets stuck at one spot for minutes, or the log has many lines with `the simulation did not see that death` (a few is fine; note how many). Send Geode's log either way: the Pathfinder's lines all start with `[AbsPF]`. If it's very slow or the phone gets hot, stop it with **Stop** and send the log anyway. |

That's the whole round: thank you!
