# Calculate Slope-Exit Bug — RESOLVED, this doc is archived history

> This bug is fixed and confirmed (2026-08-13). This document is kept for historical
> record only — its investigation and conclusions are **superseded**, not current guidance.
> Full git history of the investigation is still in this file's prior versions if the
> archaeology ever matters.

**What this doc used to describe:** during Calculate's capture pass, the player exited
slopes with a different launch trajectory than normal play/render, causing drift and
eventual noclip (reproduced on Bloodbath, ship mode, x≈2544, frame≈1718).

**What this doc concluded (as of 2026-07-04), and why it turned out to be a dead end:**
a physics-substep-granularity mismatch between Calculate's slow path (4 fine substeps)
and normal play's fast path (1 coarse substep), with a proposed fix of forcing Calculate
onto the fast path to match. This was investigated further in later sessions and
**disproven** — forcing `useFastLockDelta()` to match the renderer's derivation did NOT
fix it (CALC/REND ended up with identical `steps=`/`fast=` values but `ys=` still
diverged), so substep granularity was not actually the root cause.

**What actually fixed it:** the real fix sidestepped the "why does Calculate's simulation
diverge" question entirely instead of answering it. `MacroPathSample` was extended to
capture full per-frame kinematic state during recording (position, velocity, rotation,
onGround, upsideDown, dashing) — not just position. Calculate's capture pass now
force-applies this recorded ground truth to the real player every frame, after the
physics tick runs, instead of trusting its own re-derivation. Only engages on macros
recorded after 2026-08-11 (v2 sidecar format) — older macros need re-recording to get
the fix.

**Verified 2026-08-13** with real log evidence: `guccibot_slope.log` showed CALC and PLAY
matching exactly at every frame through the historic divergence point, where CALC used
to diverge.

See the Claude Code memory store (`project-guccibot-status`, `project-1-3-frame-window`)
for the full session-by-session history if more detail is ever needed than this summary.
