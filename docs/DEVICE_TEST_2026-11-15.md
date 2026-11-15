# Device test, Sunday 2026-11-15 (Mac and Android)

About an hour, on your Apple-silicon Mac and your Android phone. Nothing here needs code. Do the round on both unless a step says Mac or Android only. Keep **safe mode on**, as usual.

## 1. Put the build on

The build is GitHub's package of commit **`f880d41a332074113550279dd378057eb2ad8029`** (build **2026-11-15-c**). It's one file for every platform. Its build run: https://github.com/Nigelx1/GucciBot/actions/runs/43712723806

1. Open https://github.com/Nigelx1/GucciBot/actions/runs/43712723806 (the **Multi-platform build** run for that commit, green check), scroll down to **Artifacts** and download **GucciBot (all platforms)**. You need to be logged in to GitHub. It's a zip of about 200 MB. Unzip it to get `nigelx1.guccibot.geode`.
2. In the game: Geode's mod list > the gear > **Install From File** > pick the file > restart the game. This is the easy way on both.
   - By hand instead: **Open Mods Folder** in the same place shows the folder. On a Steam Mac it's inside the app: `~/Library/Application Support/Steam/steamapps/common/Geometry Dash/Geometry Dash.app/Contents/geode/mods`. On Android (Geode's launcher) it's `Android/media/com.geode.launcher/game/geode/mods`.
3. Check you have the right build: near the top of Geode's log (below) there's a line starting `[GucciBot] BUILD: 2026-11-15-c`. A later build (a later label) is fine too, as long as it's 2026-11-15-c or newer.

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
| 8 | **Video Mode** (the Video Mode cards under the JMF Trainer and the Trainer). Just look, nothing to set up. | Each card has a warning, `Video Mode is Windows-only for now`, and its switch can't be turned on. Bad: the switch turns on and ends on a red failure. |

If something goes wrong, keep going with the rest if you can, and say which step it was. That's the whole round: thank you!
