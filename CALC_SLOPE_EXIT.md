# Calculate Slope-Exit Bug — Current State

> **Read this before working on the Calculate / frame-window analyzer slope bug.**
> Supersedes the dt/calcSsb/force-1-step reasoning in older notes for *this specific symptom*.
> Last updated 2026-07-04. Owner: Nigel (in-game tester). Author of this doc: Claude.

## The precise symptom (Nigel, confirmed in-game, reproducible)

During the **Calculate capture pass**:
- The player **correctly enters and rides** slopes. (Contact/ride is fine.)
- It **exits the slope ride DIFFERENTLY than it should** — different launch trajectory off the slope.
- That exit divergence → downstream drift → desync → eventual noclip.

**Calculate-ONLY.** Normal playback and render exit the same slopes correctly. Has been this
way for a long time ("always has been"). Reproduced again 2026-07-04.

## RULED OUT — do NOT re-investigate these

- **Mod conflicts.** Only MegaHack enabled among physics-capable mods; `peony.silicate`,
  `astralteam.astral`, `syzzi.click_between_frames` are present but **disabled**. MegaHack
  physics toggles (speedhack/noclip/hitbox/autopilot) off. Not the cause.
- **GD version.** Running **2.2081**, matches `mod.json`. Hardcoded midhook/patch offsets
  are the correct version.
- **Binary patches.** All 4 apply cleanly (no `[GucciBot] N of 4 binary patches FAILED`).
- **dt / step-count / fixed-vs-variable dt.** DEFINITIVELY ruled out by the renderer:
  `renderer.hpp:78` → `getDt() = 1.f / m_settings.m_fps`, and `m_fps` defaults to **60**
  (`renderer.hpp:24`, `renderer.cpp:170`). So the working renderer drives
  `m_pScheduler->update(1.0f / 60.0f)` — the **exact same dt, exact same 4-steps-per-frame
  burst** as Calculate (`engine_updater.cpp:500`) — yet the renderer exits slopes correctly
  and Calculate doesn't. Real-dt (`-d`) and fixed-dt (`-e`) were both slope-tested and both
  failed, because **dt was never the lever**. (Confirm Nigel's `render_fps` setting; conclusion
  holds for any fixed fps the renderer uses.)
- **Frame advance / 1/240 single-stepping.** Wouldn't help — same reason (renderer disproves
  the dt layer entirely). Do not rebuild the capture as a frame advance to fix *this* bug.
- **"m_isOnGround=false → no slope snap → fall through."** WRONG frame. Nigel confirms the
  player **does** touch and ride the slope. The `ground=0` log readings are a symptom/
  transient, not the cause. The cause is at the **exit**, not at contact.

## The locus

The **slope-EXIT transition** — the frame(s) where the player leaves the slope. Something
about how the Calculate capture handles that transition differs from play/render, producing
a different launch velocity/position.

## Remaining candidates (in order)

1. **`PlayLayer::createCheckpoint()` called at every click during capture**
   (`engine_core.cpp:1173`). This is the ONE concrete game-state-touching action the capture
   does that play/render never do mid-run. If a click (jump/launch input) lands at or near
   the slope exit, checkpoint creation at that exact frame could perturb the launch state
   (velocity/position) → different exit trajectory. **Not yet tested by gating it.**
   - Follow-up: `overrideCheckpointPlacement` midhook (`hook_gjbasegamelayer.cpp:22-25`,
     installed `:406`) redirects checkpoint placement to `queueCheckpoint()`. Verify that
     path doesn't perturb the live player on the exit frame.
2. **drawScene Calculate branch** (`engine_updater.cpp:499-506`) does
   `m_pRunningScene->visit()` where the SLRenderer branch (`:455-475`) does
   `sl->update(pl)` + `sl->displayPreview()`. Likely render-only (visit is draw traversal),
   not physics — low priority, but it is a structural difference.
3. **`fwTick()` per-frame interleaving** (`engine_updater.cpp:400`) — runs only during
   Calculate, inside `frameUpdateMidhook`. Its only non-logging action during capture is the
   `createCheckpoint` from #1, so this reduces to #1.

## The logging gap (process failure Nigel flagged 2026-07-04)

> "each conversation doesn't fix anything and logs too little, which makes further
> conversations feel like being back to square 1."

Prior sessions logged sparse per-frame data, found a symptom, failed to fix it, and the next
session re-derived everything. **Fix:** log comprehensively AND durably.

### Logging to add (next build)
A comprehensive per-frame log, fired during `isPlaying()` for **both PLAY and CALC**, written
to a **dedicated file** (e.g. `guccibot_slope.log`, not lost in `geode.log`):

    [SLOPE] {PLAY|CALC} f=<frame> x=<pos.x> y=<pos.y> xs=<playerSpeed> ys=<yVelocity>
            rot=<rotation> g=<isOnGround> flip=<isUpsideDown> dash=<isDashing>
            mode=<cube|ship|ball|ufo|wave|robot|spider> hold=<0|1>
            steps=<estimatedStepCount> ovf=<tpsOverflow> respawn=<respawnTimer>
            ckpt=<1 if a checkpoint was created this frame else 0>

Then **diff PLAY vs CALC at the slope-exit frame** → the first diverging field names the
mechanism (e.g. `ys` differs at exit with `ckpt=1` the frame before → createCheckpoint
perturbed the launch).

Existing logs to keep using / extend: `[DIAG]` (`engine_updater.cpp:370-395`) and
`[CAP-F]` (`engine_core.cpp:1159`). They already capture most fields but are interleaved
into `geode.log` and have no `ckpt` marker — extend, don't replace.

## Immediate next action

1. Bump `GB_BUILD_LABEL`, add the `[SLOPE]` logger above (both PLAY and CALC), build.
2. Nigel: play Bloodbath normally once (PLAY lines), then run Calculate (CALC lines).
3. Diff the two at the first slope exit. First diverging field = the mechanism.
4. Mechanism → the one-line (or few-line) fix. One change, build, Nigel confirms in-game.

## Key code references (verify against current source)

- Capture setup (mirrors renderer, looks correct): `engine_core.cpp:1070-1136`.
- Capture phase (`fwTick` Capturing case, `createCheckpoint` at clicks): `engine_core.cpp:1153-1212`.
- Calculate drawScene branch (fixed 1/60 scheduler update): `engine_updater.cpp:499-506`.
- Renderer drawScene branch (works, same dt): `engine_updater.cpp:455-475`.
- Renderer dt definition: `src/render/renderer.hpp:78` (`1/m_fps`, default 60).
- `calculateSteps` / `runSlowLockDelta` (shared by render + Calculate): `engine_updater.cpp:54-100, 149-192`.
- `overrideCheckpointPlacement` midhook: `hook_gjbasegamelayer.cpp:22-25, 406`.

## Fable 5 second opinion (2026-07-04) — strongest hypothesis, NOT yet confirmed

Asked Claude Fable 5 for a second opinion (better web/GD-binding access). It independently converged on
`createCheckpoint()` as the likely cause and gave the most mechanistic lead yet. Treat as the prime
hypothesis; **confirm with the `ckpt` marker in the [SLOPE] log before rewriting** (this project was
burned by the calcSsb theory, which was also confident + specific + wrong).

**Fable 5's mechanism (two parts, either sufficient):**
- (a) Constructing a `CheckpointObject` snapshots a `PlayerCheckpoint` (~185 fields) that *reads the
  live player's slope-launch state* — and the surrounding auto-checkpoint bookkeeping
  (`m_shouldTryPlacingCheckpoint`, `m_quickCheckpointMode`) participates, so the call is not a pure read.
- (b) The `queueCheckpoint()` midhook redirect (`hook_gjbasegamelayer.cpp:22-25`) changes *when* in the
  frame checkpoint work runs relative to the 4×(1/240) physics substeps. Slope-exit launch reads carried
  floats, so a reorder on the launch frame changes the exit velocity. The renderer never queues checkpoints.

**The decisive GD fields (the slope-exit launch set), per Fable 5 from the Geode 2.2074 PlayerCheckpoint
layout — VERIFY each against 2.2081 / camila314's `gdp` 2.2 decomp before copying:** `m_isOnSlope`,
`m_wasOnSlope`, `m_slopeVelocity`, `m_yVelocityBeforeSlope`, `m_slopeStartTime`, `m_slopeAngle`,
`m_slopeAngleRadians`, `m_slopeRotation`, `m_currentSlopeYVelocity`, `m_currentSlope`, `m_currentSlope2`,
`m_currentPotentialSlope`, `m_isCollidingWithSlope`, `m_isCurrentSlopeTop`, `m_objectSnappedTo`,
`m_snapDistance`, `m_lastGroundObject`, `m_preLastGroundObject`, `m_yVelocity`, `m_yVelocityUnrounded`,
`m_groundYVelocity`, `m_isOnGround`/`m_isOnGround3`, `m_jumpBuffered`/`m_wasJumpBuffered`/
`m_stateJumpBuffered`, `m_isDashing`/`m_dashStartTime`. Launch computed in
`PlayerObject::collidedWithSlopeInternal` (2.2074 win `0x38f810`) + `getModifiedSlopeYVel()`.

**Fable 5's recommended fix (Stage 2 — only after confirmation):** Replace the per-frame
`createCheckpoint()`→`queueCheckpoint()` in capture with a hand-rolled struct copy of the PlayerObject
physics fields (the set above) for BOTH players + the minimal GJBaseGameLayer game-state, restored by
writing fields back directly — NEVER round-tripping through `createCheckpoint`/`loadFromCheckpoint`
(which mutate live state and pull in triggers/effects/audio). Silicate's architecture is the reference:
`src/checkpoint/` (practice fix) is separate from `src/physics/` + `src/trajectory/` (sim), and its
trajectory is "NOT physics changing" per its author. **Caveat: Fable 5 did NOT read Silicate's source
line-by-line** — confirm `src/trajectory/` actually snapshots fields directly (or sims via `src/physics/`)
rather than calling `createCheckpoint`.

**How we confirm (no new build — uses the [SLOPE] logger already shipped):** Nigel runs PLAY Bloodbath +
CALC Bloodbath; diff at the slope-exit frame; read the `ckpt` column.
- CALC diverges at `ckpt=1` (or frame after), diverging field = `ys` → Fable 5 confirmed → Stage 2.
- CALC diverges at `ckpt=0` → checkpoint exonerated → the diverging field names the real cause; chase that.
- Ambiguous → run Fable 5's Stage 1 (gate the `createCheckpoint` call during capture, rebuild, see if the
  slope-exit now matches the renderer).

**Threshold note (Fable 5):** if after Stage 2 the PlayerObject state is byte-identical to the renderer
at the exit but the launch STILL differs, the remaining suspect is `GJBaseGameLayer`/`GJGameState`
(global timer, object-activation, effect/trigger state) the manual copy omits — expand the snapshot.

## ACTUAL MECHANISM FOUND (2026-07-04) — via [SLOPE] PLAY-vs-CALC diff

**Fable 5's checkpoint hypothesis is REFUTED by data.** The [SLOPE] logger captured a full PLAY + CALC
run on Bloodbath. PLAY and CALC match (within 0.01) from f=1 → f=1719. First divergence is at **f=1718**,
and `ckpt=0` there (nearest checkpoints f=1671 and f=1910 — runs stayed identical 49 frames after 1671,
so checkpoints don't perturb). The checkpoint is innocent.

**The real mechanism — a substep-granularity-sensitive launch collision:**
At f=1718 the player (ship mode, x≈2544) hits a **y-launch object**. PLAY catches the launch, CALC misses it:

| frame | PLAY ys | CALC ys | note |
|---|---|---|---|
| 1717 | 3.148 | 3.148 | match |
| 1718 | 4.734 | 3.249 | PLAY got +1.485 launch impulse, CALC got 0 (gravity only) |
| 1719+ | 4.835… | 3.350… | +1.485 gap holds constant (same gravity, offset trajectories) |

PLAY rides up; CALC falls behind → drift → noclip. Position still matches at f=1718–1719 (the launch
sets velocity for the NEXT frame), which is why it presents as "rides the slope, exits wrong."

**Root cause = physics substep granularity differs between PLAY and CALC (visible in steps/ovf all run):**
- **PLAY = fast path** `runFastLockDelta` → **1 coarse substep of ~1/60** (`steps=1`, `ovf` floats nonzero).
- **CALC = slow path** `runSlowLockDelta` → **4 fine substeps of 1/240** (`steps=4`, `ovf=0`).

Same total dt, different subdivision. At f=1718's launch object the two granularities resolve the
collision differently. This is the fast-vs-slow divergence, pinpointed to one frame + one object.

**THE DECISIVE QUESTION (pending Nigel, in-game):** does the RENDERER catch the f=1718 launch?
Render Bloodbath, scrub to x≈2544 (ship y-launch ~y=235). If render rides it → CALC's slow path has a
fixable difference from the renderer's slow path (diff those two). If render falls short too → 4-fine is
genuinely wrong here → fix = make CALC use the fast path (force-fast, 1 coarse substep = match PLAY).
Force-fast was tried once (`-b`) but BEFORE the steps=1 capture bug was fixed (`2026-07-02-b`), so that
failure was likely confounded — re-trying cleanly now (with [SLOPE] logger confirming PLAY==CALC) is the
test. `useFastLockDelta` is at `engine_updater.cpp:48-51` (the one-line gate to add `|| gb->fwAnalyzing`).

**Logger data location:** `C:\Users\goofy\AppData\Local\GeometryDash\geode\mods\guccimanefan.guccibot\guccibot_slope.log`
