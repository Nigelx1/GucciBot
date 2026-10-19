# Calculate port audit (started 2026-10-19)

Nigel: "i feel like these 1 frames and stuff is purely a guccibot issue, we need to make sure we ported all of anticrooms stuff right". This audit compares our port of anticroom's frame-window analyzer (`src/analysis/ac/framewindow.cpp`, as of build -ec) against his original (`src/analysis/framewindow.cpp` in his "slc count" drop, `slc count.zip`, 2026-09-26), function by function.

**Method.** Both files are split into functions and compared with whitespace and comments ignored. 94 functions are identical, so there's nothing to check there. 13 are ours alone (logging, the results sidecar, the L* readout). The 30 that differ are below, the measuring core first. Each one gets read side by side. Every difference is classed as a **rename** (the shim maps Silicate names onto ours), **ours on purpose** (a GucciBot addition, with its reason), or a **port fault** (it measures differently than anticroom's, which could make a 1-frame window).

**Saved as it goes.** After each function the finding is written under it, the box is ticked, and the file is committed and pushed. A cut-off session resumes at the first unticked box.

## Outcome (2026-10-19, all 30 checked)

Two port faults, both fixed, neither yet confirmed in-game:

1. **`stepDependent` saved its checkpoint with the attempt's start frame** where our checkpoint wants its own frame. Every dependent leg ran on the wrong clock, died early and measured a window that was too small. Fixed in build -ed. This is the one that can make the 1-frame windows.
2. **The results file dropped the dependent clicks' averaged windows** (`fwstore` mapping, under `saveResults`). Live results were right; after the macro was loaded again, those clicks showed their strict window instead. Fixed in build -ee.

Everything else is a rename the shim needs, logging, or a GucciBot addition with its reason in the code. The measuring logic itself matches anticroom's.

## Checklist: the measuring core (first)

- [x] `isMeasurableInput` (anticroom 278-288, ours 320-330)
- [x] `FrameWindowAnalyzer::stepToward` (anticroom 289-453, ours 331-535)
- [x] `FrameWindowAnalyzer::startRange` (anticroom 702-880, ours 784-969)
- [x] `FrameWindowAnalyzer::checkCaptureAgainstTrail` (anticroom 1261-1293, ours 1350-1382)
- [x] `FrameWindowAnalyzer::restoreToBranch` (anticroom 1628-1694, ours 1734-1800)
- [x] `FrameWindowAnalyzer::beginShift` (anticroom 1728-1848, ours 1834-1955)
- [x] `FrameWindowAnalyzer::advanceSweep` (anticroom 1917-2205, ours 2024-2312)
- [x] `FrameWindowAnalyzer::noteSweepStep` (anticroom 2207-2222, ours 2314-2329)
- [x] `FrameWindowAnalyzer::noteCaptureDeath` (anticroom 2235-2245, ours 2342-2352)
- [x] `FrameWindowAnalyzer::startSubframeProbe` (anticroom 2264-2376, ours 2371-2483)
- [x] `FrameWindowAnalyzer::buildMark` (anticroom 2445-2552, ours 2552-2659)
- [x] `FrameWindowAnalyzer::finishClick` (anticroom 2554-2670, ours 2661-2777)
- [x] `FrameWindowAnalyzer::abortClick` (anticroom 2771-2776, ours 2878-2883)
- [x] `FrameWindowAnalyzer::stepDependent` (anticroom 3406-3517, ours 3588-3699)
- [x] `FrameWindowAnalyzer::finish` (anticroom 2839-2965, ours 2946-3137)
- [x] `FrameWindowAnalyzer::tick` (anticroom 2972-3315, ours 3144-3497)
- [x] `FrameWindowAnalyzer::reportStateDiff` (anticroom 1530-1589, ours 1636-1695)
- [x] `logPlayerState` (anticroom 79-92, ours 112-134)

## Checklist: display, results file, trips (after the core)

- [x] `FrameWindowAnalyzer::saveResults` (anticroom 3761-3803, ours 4117-4158)
- [x] `FrameWindowAnalyzer::loadResults` (anticroom 3805-3866, ours 4160-4221)
- [x] `FrameWindowAnalyzer::spawnMarker` (anticroom 4209-4279, ours 4564-4677)
- [x] `FrameWindowAnalyzer::rebuildHud` (anticroom 4437-4518, ours 4926-5007)
- [x] `FrameWindowAnalyzer::updateDisplay` (anticroom 4624-4723, ours 5113-5212)
- [x] `FrameWindowAnalyzer::render` (anticroom 4847-4905, ours 5336-5407)
- [x] `FrameWindowAnalyzer::updatePrecisionReadout` (anticroom 4907-4973, ours 5420-5422)
- [x] `FrameWindowAnalyzer::applyLabel` (anticroom 5009-5067, ours 5458-5516)
- [x] `FrameWindowAnalyzer::testPlayhead` (anticroom 5069-5119, ours 5518-5568)
- [x] `FrameWindowAnalyzer::beginTrip` (anticroom 5128-5151, ours 5577-5600)
- [x] `FrameWindowAnalyzer::stepTrip` (anticroom 5153-5217, ours 5602-5667)
- [x] `FrameWindowAnalyzer::endTrip` (anticroom 5219-5242, ours 5669-5692)

## Ours alone (no counterpart; checked only if they touch measuring)

`fwFileLog`, `gucci::fwEngineLog`, `FrameWindowAnalyzer::capturing`, `fwPlayerFieldDump`, `markToJson`, `messageToJson`, `toJson`, `jnum`, `jreal`, `jbool`, `jstr`, `fromJson`, `FrameWindowAnalyzer::updateLStarHud`

`markToJson`/`fromJson` stand in for glaze, so they were checked field by field against the structs: they missed the three dependent fields (see `saveResults`). `messageToJson`, `toJson` and the rest of `fromJson` cover every field.

## Findings

(One entry per function, in checklist order.)

### `isMeasurableInput`: rename

`slc::Action::ActionType` became `slc::ActionType` (the shim's type). Same cases.

### `FrameWindowAnalyzer::checkCaptureAgainstTrail`: logging only

`log::warn` became our `FWWARN` (the analyzer's own log, which also writes guccibot_fw.log). Same logic.

### `FrameWindowAnalyzer::noteSweepStep`: logging only

`log::warn` became `FWWARN`. Same logic.

### `FrameWindowAnalyzer::noteCaptureDeath`: logging only

`log::warn` became `FWWARN`. Same logic.

### `FrameWindowAnalyzer::abortClick`: logging only

`log::warn` became `FWWARN`. Same logic.

### `FrameWindowAnalyzer::restoreToBranch`: rename (correct only if the checkpoint frame is the current frame)

Silicate's `m_saved.m_frame` became our `m_saved.m_frameOffset`, and `log::warn` became `FWWARN`. Silicate's `PracticeFix::createCheckpoint(obj, attemptStartFrame)` sets `m_frame` to `getFrame()` itself. Ours, `GucciPracticeFix::createCheckpoint(cp, frameOffset)`, stores its second argument as `m_frameOffset`, and the practice fix passes `getFrame()` (hook_playlayer.cpp:74). So this rename holds only where callers pass the current frame. See `stepDependent`.

### `FrameWindowAnalyzer::beginShift`: ours on purpose (logging)

One added line, `g_traceLastFrame = UINT32_MAX;`, resets a trace-log marker (framewindow.cpp:44, used by the per-frame trace around 434-442). Measuring is unchanged.

### `FrameWindowAnalyzer::buildMark`: logging only

`log::warn` became `FWWARN`. Same logic.

### `FrameWindowAnalyzer::finishClick`: logging only

Two `log::warn` became `FWWARN`. Same logic.

### `FrameWindowAnalyzer::stepDependent`: PORT FAULT, fixed in build -ed (6b3ddf4)

The function's own diff is only the rename and logging, but the identical line below it is the fault. After stepping to `base`, it saves the analyzer's checkpoint with `pf.createCheckpoint(m_cpObject, updater.m_frameOnLastAttempt)` (ours 3642, anticroom 3460). In anticroom's code the second argument is the attempt's start frame, and the checkpoint's frame is read from the updater. In ours, the second argument IS the checkpoint's frame (`m_frameOffset`), and the restore path writes it back into the frame counter.

Effect, as `tick`'s own port comment describes for the capture's checkpoint: a restore to this checkpoint puts the player where it was at `base`, but sets the clock to the attempt's start. The frame check in `restoreToBranch` passes, because the clock was set to the same wrong frame. Every dependent leg then replays the macro's clicks at the wrong frames, dies early, and measures a window that is far too small. The next pair's `m_saved.m_frameOffset <= base` test is also always true.

`tick`'s checkpoint was ported to pass `updater.getFrame()`, with a comment explaining why; this second call was missed. Fix: pass `updater.getFrame()` here too. **This is a direct mechanism for too-small windows on dependent clicks, a strong candidate for Nigel's 1-frame windows.** Not yet confirmed in-game.

### `FrameWindowAnalyzer::advanceSweep`: logging only

Two `log::warn` became `FWWARN`. Same logic.

### `FrameWindowAnalyzer::startSubframeProbe`: logging only

`log::warn` became `FWWARN`. Same logic.

### `FrameWindowAnalyzer::reportStateDiff`: logging only

`log::warn` became `FWWARN`. Same logic.

### `logPlayerState`: logging only

The same line also goes to guccibot_fw.log (fwFileLog). Added while diagnosing the Congregation start-speed bug.

### `FrameWindowAnalyzer::stepToward`: ours on purpose (trace logging), measuring unchanged

Adds a per-frame `[fw][trace]` line comparing each nominal leg with the capture, and a warning when the frame jumps by more than one inside a leg. The `!m_pathDiverged` test moved from the outer condition to the inner one, so the trace keeps logging after a divergence; `m_pathDiverged` is set exactly as before. Two `log::warn` became `FWWARN`.

### `FrameWindowAnalyzer::startRange`: renames + logging

Silicate's `SLValue` settings (`m_preventDeath->inner()`, `m_tps->inner()`, `m_canDie->inner()`) became GucciBot's plain fields. `std::string(levelName)` for the log. Two `logPlayerState` calls around the reset (Congregation diagnosis). Same logic.

### `FrameWindowAnalyzer::tick`: logging + the correct checkpoint-frame port

`log::warn` became `FWWARN` (five places). The capture's checkpoint passes `updater.getFrame()` instead of `m_frameOnLastAttempt`, with a comment explaining why: our checkpoint has one frame field, filled from the argument and written back to the frame counter on restore, and the attempt's start made every leg die and every window measure 0. This is the correct port, and the model for the `stepDependent` fix.

### `FrameWindowAnalyzer::finish`: ours on purpose, measuring unchanged

Three GucciBot additions, each with its reason in the code:
- a run whose capture fell back to noclip is reported as failed, because every window would be pinned to the sweep limit (Congregation);
- the macro's shifted frames are put back exactly as before, now counted and logged, with a warning if they can't be;
- `m_running` and the stage are cleared before the hand-back reset, so onReset runs under normal rules and a frame-0 first input isn't skipped.

Also `m_canDie->inner()` became the plain field, and the summary line is written to guccibot_fw.log unconditionally.

### `FrameWindowAnalyzer::updatePrecisionReadout`: replaced on purpose

anticroom's two-label precision readout is replaced by GucciBot's `updateLStarHud` (ours only, framewindow.cpp ~4811), which shows L* in NaN's published form and kicks the solver itself. As the comment left in its place says, it still feeds the solver the TPS the results were measured at (`resultsTps`). No measuring involved.

### `FrameWindowAnalyzer::applyLabel`: rename

`m_tps->inner()` became the plain field. (The splitter's line ranges for ours run a few lines into the neighbouring function; the real difference is this one line.)

### `FrameWindowAnalyzer::testPlayhead`: rename (correct)

Practice checkpoints' `m_frame` became `m_frameOffset`. That's right here, because the practice fix saves them with `updater.getFrame()` (hook_playlayer.cpp:74).

### `FrameWindowAnalyzer::saveResults`: rename, but the writer it calls was a PORT FAULT, fixed in build -ee

anticroom writes the results with glaze, which serialises every field of `fwstore::StoredMark` by reflection. Ours calls `fwstore::toJson` (matjson, Geode's own), which names each field by hand, and its comment warns that a field missing from the mapping is dropped silently. Three were missing: `dependent`, `dependentMin` and `dependentMax`, the dependent-click pass's averaged window (they came with the slc count drop, after the mapping was written). `saveResults` copied them into the struct; `markToJson` never wrote them.

Effect: live, a dependent click shows `~X`, its window averaged over the previous click's window, and that average also picks its colour tier, its circle size and its L* input (`displayWindow`, `spawnMarker`, the L* inputs). Results follow the macro (engine_core.cpp `saveAcResults`/`loadAcResults`), so once the macro was saved and loaded again the average came back as 0 and the same click showed its strict window instead, with a different number and colour. Fixed in build -ee: the three fields are written and read. Results files from before -ee still load (the three read as 0, as before); measuring again brings the averages back.

### `FrameWindowAnalyzer::loadResults`: rename (the read side of the saveResults fault, fixed in -ee)

`glz::read_json` became `fwstore::fromJson`. The copies into each mark are the same, the three dependent fields included, but `fromJson` never read those three from the file. Fixed with saveResults in build -ee.

### `FrameWindowAnalyzer::spawnMarker`: ours on purpose (drawing only), measuring unchanged

Three GucciBot additions, all in how the marker is drawn:
- the Circle skin setting (off by default) sizes the ring by the click's window;
- a tier can carry one of Juice's shapes, and without one it is anticroom's two-pass ring exactly;
- build -dt: a mark saved with no position (0,0) takes the macro path's position at its frame, or isn't drawn (issue #26: one marker "in the ground", none where the clicks were).

### `FrameWindowAnalyzer::rebuildHud`: ours on purpose (the HUD scale setting)

anticroom's HUD has a fixed scale of 0.6; ours reads the HUD scale setting (`frameWindow.hudScale`, at least 0.1). Nothing else differs.

### `FrameWindowAnalyzer::updateDisplay`: rename

`updater.m_tps->inner()` became the plain field. Same logic.

### `FrameWindowAnalyzer::render`: ours on purpose (when the windows show), measuring unchanged

Build -dv: the windows show only while a macro plays (`Bot::get()->isPlaying()` added to `want`), because they stayed over the player's own attempts after playback was turned off (Nigel, 2026-10-17). Our L* readout's two labels are removed along with the HUD, and when the precision readout is off. `m_tps->inner()` became the plain field.

### `FrameWindowAnalyzer::beginTrip`: rename

`m_backwardsStepping->inner()` became the plain field (twice). Same logic.

### `FrameWindowAnalyzer::stepTrip`: rename (correct)

Silicate's `practiceFix().m_maxStoredFrames` became our `updater.m_maxBackstepFrames` (the Back Step Count setting). Checked that they mean the same thing: both cap the frames the practice fix keeps for stepping back, 0 keeps none, and the oldest drop out first (Silicate checkpoint/fix.cpp:146-151, ours engine_core.cpp:159-167). `m_backwardsStepping->inner()` became the plain field.

### `FrameWindowAnalyzer::endTrip`: rename

`m_backwardsStepping->inner()` became the plain field. Same logic.
