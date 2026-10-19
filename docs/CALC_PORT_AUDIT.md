# Calculate port audit (started 2026-10-19)

Nigel: "i feel like these 1 frames and stuff is purely a guccibot issue, we need to make sure we ported all of anticrooms stuff right". This audit compares our port of anticroom's frame-window analyzer (`src/analysis/ac/framewindow.cpp`, as of build -ec) against his original (`src/analysis/framewindow.cpp` in his "slc count" drop, `slc count.zip`, 2026-09-26), function by function.

**Method.** Both files are split into functions and compared with whitespace and comments ignored. 94 functions are identical, so there's nothing to check there. 13 are ours alone (logging, the results sidecar, the L* readout). The 30 that differ are below, the measuring core first. Each one gets read side by side. Every difference is classed as a **rename** (the shim maps Silicate names onto ours), **ours on purpose** (a GucciBot addition, with its reason), or a **port fault** (it measures differently than anticroom's, which could make a 1-frame window).

**Saved as it goes.** After each function the finding is written under it, the box is ticked, and the file is committed and pushed. A cut-off session resumes at the first unticked box.

## Checklist: the measuring core (first)

- [x] `isMeasurableInput` (anticroom 278-288, ours 320-330)
- [ ] `FrameWindowAnalyzer::stepToward` (anticroom 289-453, ours 331-535)
- [ ] `FrameWindowAnalyzer::startRange` (anticroom 702-880, ours 784-969)
- [x] `FrameWindowAnalyzer::checkCaptureAgainstTrail` (anticroom 1261-1293, ours 1350-1382)
- [x] `FrameWindowAnalyzer::restoreToBranch` (anticroom 1628-1694, ours 1734-1800)
- [x] `FrameWindowAnalyzer::beginShift` (anticroom 1728-1848, ours 1834-1955)
- [ ] `FrameWindowAnalyzer::advanceSweep` (anticroom 1917-2205, ours 2024-2312)
- [x] `FrameWindowAnalyzer::noteSweepStep` (anticroom 2207-2222, ours 2314-2329)
- [x] `FrameWindowAnalyzer::noteCaptureDeath` (anticroom 2235-2245, ours 2342-2352)
- [ ] `FrameWindowAnalyzer::startSubframeProbe` (anticroom 2264-2376, ours 2371-2483)
- [x] `FrameWindowAnalyzer::buildMark` (anticroom 2445-2552, ours 2552-2659)
- [x] `FrameWindowAnalyzer::finishClick` (anticroom 2554-2670, ours 2661-2777)
- [x] `FrameWindowAnalyzer::abortClick` (anticroom 2771-2776, ours 2878-2883)
- [x] `FrameWindowAnalyzer::stepDependent` (anticroom 3406-3517, ours 3588-3699)
- [ ] `FrameWindowAnalyzer::finish` (anticroom 2839-2965, ours 2946-3137)
- [ ] `FrameWindowAnalyzer::tick` (anticroom 2972-3315, ours 3144-3497)
- [ ] `FrameWindowAnalyzer::reportStateDiff` (anticroom 1530-1589, ours 1636-1695)
- [ ] `logPlayerState` (anticroom 79-92, ours 112-134)

## Checklist: display, results file, trips (after the core)

- [ ] `FrameWindowAnalyzer::saveResults` (anticroom 3761-3803, ours 4117-4158)
- [ ] `FrameWindowAnalyzer::loadResults` (anticroom 3805-3866, ours 4160-4221)
- [ ] `FrameWindowAnalyzer::spawnMarker` (anticroom 4209-4279, ours 4564-4677)
- [ ] `FrameWindowAnalyzer::rebuildHud` (anticroom 4437-4518, ours 4926-5007)
- [ ] `FrameWindowAnalyzer::updateDisplay` (anticroom 4624-4723, ours 5113-5212)
- [ ] `FrameWindowAnalyzer::render` (anticroom 4847-4905, ours 5336-5407)
- [ ] `FrameWindowAnalyzer::updatePrecisionReadout` (anticroom 4907-4973, ours 5420-5422)
- [ ] `FrameWindowAnalyzer::applyLabel` (anticroom 5009-5067, ours 5458-5516)
- [ ] `FrameWindowAnalyzer::testPlayhead` (anticroom 5069-5119, ours 5518-5568)
- [ ] `FrameWindowAnalyzer::beginTrip` (anticroom 5128-5151, ours 5577-5600)
- [ ] `FrameWindowAnalyzer::stepTrip` (anticroom 5153-5217, ours 5602-5667)
- [ ] `FrameWindowAnalyzer::endTrip` (anticroom 5219-5242, ours 5669-5692)

## Ours alone (no counterpart; checked only if they touch measuring)

`fwFileLog`, `gucci::fwEngineLog`, `FrameWindowAnalyzer::capturing`, `fwPlayerFieldDump`, `markToJson`, `messageToJson`, `toJson`, `jnum`, `jreal`, `jbool`, `jstr`, `fromJson`, `FrameWindowAnalyzer::updateLStarHud`

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

### `FrameWindowAnalyzer::stepDependent`: PORT FAULT, fixed in the next build

The function's own diff is only the rename and logging, but the identical line below it is the fault. After stepping to `base`, it saves the analyzer's checkpoint with `pf.createCheckpoint(m_cpObject, updater.m_frameOnLastAttempt)` (ours 3642, anticroom 3460). In anticroom's code the second argument is the attempt's start frame, and the checkpoint's own frame is `getFrame()`. In ours it IS the checkpoint's frame, so the checkpoint claims the attempt's start frame instead of `base`. Effects: every restore to it fails the frame check in `restoreToBranch` ("forced-state branch bypassed", `m_restoreFailed`), and `tick` stops Calculate with "Checkpoint restore was bypassed. Stopping." The next pair's `m_saved.m_frameOffset <= base` test is also always true. The other call, in `tick` (ours 3412), was already ported to pass `updater.getFrame()`; this one was missed. Fix: pass `updater.getFrame()` here too. Link to the 1-frame windows, NOT proven: this pass refines clicks whose windows are clamped by a neighbour, and when it never completes, clamped windows stay as measured.
