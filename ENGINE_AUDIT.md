# Engine audit — GucciBot vs Silicate

**Date:** 2026-09-26 · **Against:** Silicate source (`build.zip`, GPL-3, contains
anticroom's frame-window analyzer) · **Our side:** `engine-port-2.0` @ build `-i`

Nigel asked for a full audit before deciding whether to rip our engine out and
replace it with Silicate's. This is the answer.

**Headline: don't rip it.** The audit found seven real problems, not a rewrite's
worth. Three of them are settings in the menu that are wired to nothing. One is
a memory leak. None of them explains the Congregation slope bug.

**Progress:** §1.1 is fixed (build `-j`). The rest are open. Findings are struck
through as they land, so this doc stays the live punch list.

---

## 0. What this means if you don't read C++

| # | What's wrong | What you'd notice |
|---|---|---|
| 1 | ~~Nineteen engine settings saved in **two different places at once**~~ **FIXED `-j`** | Settings were stuck at defaults until you opened the menu once, then changed |
| 2 | "Back Step Count" does nothing, and backwards stepping never frees memory | GD slowly eats RAM while backwards stepping is on, forever |
| 3 | "High TPS Precision" only does half its job | Precision is better than off, but not as good as it should be |
| 4 | Teleport triggers aren't actually seeded | A level with teleport triggers can replay differently |
| 5 | Four game hooks we never ported | Layout Mode leaves pulse/flash effects on |
| 6 | The renderer has no audio monitoring | No level meter / audio diagnostics during a render |
| 7 | One small gate differs from Silicate's | Scroll Speed Fix applies in one case it shouldn't |

Good news in the same audit:

- **The frame-window analyzer is a complete port.** Zero missing functions. It
  does not need replacing — that idea is off the table.
- **Only one empty stub is left** in the whole codebase (the trail buffer, which
  is deliberate and documented in `shim.hpp`). The "ported as an empty function"
  pattern that caused five bugs earlier has essentially been cleaned out.
- Two things I suspected were broken turned out to be **correct** — see §3.

---

## 1. Confirmed problems

### 1.1 ~~Eleven~~ **Nineteen** engine settings had two sources of truth · **FIXED, build `-j`**

`GucciEngine::initialize()` (`core/engine_core.cpp:2166`, runs at **startup**)
loads from `updater_*` / `replay_*` keys. `MenuInterface::loadSettings()`
(`gui/gui.cpp`, runs when the **menu is first opened**) loads the same fields
from `feat_*` / `eng_*` keys. Whichever runs last wins — and the GUI's runs
later, possibly after a level has already started.

**Corrected during the fix:** the audit's first pass found eleven, because the
detector only matched assignments with a `.` in them. A second pass that also
caught bare field names found **nineteen**. The extra eight:

| field | startup key | menu key | note |
|---|---|---|---|
| `layoutMode` | `hack_layoutMode` | `hack_layout_mode` | |
| `noMirrorEffect` | `hack_noMirror` | `hack_no_mirror` | |
| `audioPitchEnabled` | `hack_audioPitch` (default **false**) | `hack_audio_pitch` (default **true**) | defaults disagreed |
| `autosaveAtLevelEnd` | `autosave_atLevelEnd` (default **false**) | `feat_autosave_end` (default **true**) | defaults disagreed |
| `autosaveAtInterval` | `autosave_atInterval` | `feat_autosave_interval` | |
| `autosaveIntervalSec` | `autosave_interval` (default **60**) | `feat_autosave_interval_sec` (default **180**) | defaults disagreed |
| `replayBackupsEnabled` | `replay_backups` | `feat_replay_backups` | |
| `m_speedhackAudio` | `updater_speedhackAudio` | *(not loaded at all)* | |

The original eleven:

| field | startup key | menu key |
|---|---|---|
| `m_tps` | `updater_tps` | `eng_tick_rate` |
| `m_speedhack` | `updater_speedhack` | `eng_speed` |
| `m_lockDelta` | `updater_lockDelta` | `feat_lock_delta` |
| `m_ssbFix` | `updater_ssbFix` (default **true**) | `feat_scroll_speed_fix` (default **false**) |
| `m_preventDeath` | `updater_preventDeath` | `feat_prevent_death` |
| `m_backwardsStepping` | `updater_backwardsStepping` | `feat_backwards_step` |
| `m_extrapolateFrames` | `updater_extrapolateFrames` | `feat_frame_extrapolation` |
| `m_autoFlipOnDeath` | `updater_autoFlipOnDeath` | `feat_auto_flip` |
| `m_maintainGravity` | `replay_maintainGravity` | `feat_maintain_gravity` |
| `m_mirrorInputs` | `replay_mirrorInputs` | `feat_mirror_inputs` |

Plus `hud_showFrame` / `hud_show_frame` and `hud_showTPS` / `hud_show_tps`.

(`m_lockDeltaMode` looked like a twelfth from the save file, which holds both
`updater_lockDeltaMode` and `feat_lock_delta_mode`. It isn't: nothing in the
current code reads or writes the `feat_` spelling, so it is a dead leftover.)

**This is live, not theoretical.** Nigel's `saved.json` right now contains *both*
families: `updater_lockDelta: true` alongside `feat_lock_delta: true`,
`updater_lockDeltaMode: 0` alongside `feat_lock_delta_mode: 0`,
`hud_showFrame: true` alongside `hud_show_frame: true`. They currently agree by
luck. `updater_ssbFix` is absent, so that one falls back to a default of `true`
while the menu key says `true` — agreeing by luck too.

**And it was worse than "whichever wins".** Checking the write side: all thirteen
`updater_*` / `replay_*` / camelCase keys that `initialize()` read are
**written by nothing at all.** They are read-only ghosts; the values in Nigel's
save are leftovers from a build that used to write them.

So the real behaviour was: **every one of these settings held a stale or default
value from the moment GD launched until the menu was opened for the first time,
and then silently changed.** Launch, go straight into a level, play a macro
without touching the menu, and TPS was 240 regardless of what you had saved.

`m_tps` is the tick rate. It should not have two homes.

**Fixed in build `-j`:** one loader, `GucciEngine::loadEngineSettings()`, called
from `initialize()` at startup. Canonical key = the one that is actually written,
with a fallback to the legacy key when the canonical is absent, so nobody loses a
setting. The nineteen duplicate loads are gone from
`MenuInterface::loadSettings()`, and the three autosave toggles that wrote
`autosave_*` on click now write `feat_autosave_*` like the bulk save does.

Verified against Nigel's live `saved.json` by simulating the new loader against
it: **all 23 fields come out identical** to what the menu loader gave him before.
The change moves *when* settings load, not what they are.

**Deliberately not changed:** three defaults genuinely disagreed between the two
loaders (`m_ssbFix` true/false, `autosaveAtLevelEnd` false/true,
`autosaveIntervalSec` 60/180, `audioPitchEnabled` false/true). The menu loader's
value is the one that won in practice, so the menu loader's default is what the
single loader now uses. Whether those are the *right* defaults is a separate
question — note that Silicate and our own `BotSettingsPreset` both default
`ssbFix` to **true** while the menu defaulted it to **false**.

### 1.2 "Back Step Count" is dead, and backwards stepping leaks

`m_maxBackstepFrames` (default 60) appears in exactly three places: its
declaration (`core/GucciBot.hpp:399`), the GUI slider (`gui/gui.cpp:4059`), and
save/load. **No logic ever reads it.**

There is **no cap on `m_storedFrames` anywhere in the codebase.** Silicate's
`PracticeFix::saveState` (`checkpoint/fix.cpp`) has three guards ours doesn't:

```cpp
// Silicate
if (!m_storedFrames.empty() && m_storedFrames.back().m_frame == frame) { ... return; }   // same-frame dedupe
if (m_maxStoredFrames->inner() == 0) { ... return; }                                      // disabled
while (m_storedFrames.size() >= m_maxStoredFrames->inner()) {                             // eviction
    m_storedFrames.front().m_checkpoint->release();
    m_storedFrames.pop_front();
}
```

Our `GucciPracticeFix::saveCurrent` (`core/engine_core.cpp`) has a
*checkpoint-identity* dedupe instead — which is GucciBot-specific and there for
a real documented reason — but no cap and no eviction.

Why it matters: with Backwards Stepping on, `earlyUpdateMidhook` calls
`saveState` **every single frame**. Each entry is a `SavedCheckpointState`, which
since the 2026-09-22 determinism work carries both players' full state *plus*
the variance table, the persistent item map and the object list. At 240 TPS
that's 240 full level snapshots per second into a deque that never shrinks.

**Fix:** apply `m_maxBackstepFrames` in `saveCurrent` with Silicate's eviction.
One small change, and it makes an existing slider mean something.

### 1.3 `yVelocityRound` midhook missing — High TPS Precision is half-wired

Silicate installs **eight** named midhooks. We install six of them.

| offset | name | ours? |
|---|---|---|
| `0x237A7C` | physDt | yes |
| `0x237DCE` | physStepCount | yes |
| `0x238F6E` | restorePhysDt | yes |
| `0x237E42` | backstepUpdate (we call it `earlyUpdate`) | yes |
| `0x238BAA` | frameUpdate | yes |
| `0x3A3657` | checkpointPlacement | yes |
| `0x38c315` | **yVelocityRound** | **NO** |
| `0x20FEDC` | **teleportRandomOverride** | **NO** |

We *do* have the `PlayerObject::setYVelocity` override with the same
quantisation logic (`hooks/hook_playerobject.cpp:243`). Silicate has **both** —
the override catches calls that go through the setter, the midhook catches GD's
inline y-velocity writes that bypass it. So "High TPS Precision" is on the menu
and does half of what it claims.

### 1.4 `teleportRandomOverride` midhook missing — teleport RNG isn't seeded

`m_teleportRandomState` exists, is stored in `SavedCheckpointState`, and is
restored on checkpoint load. **Nothing ever uses it**, because the midhook at
`base + 0x20FEDC` that would read it was never installed. Same shape as the
bugs we already fixed: the storage half landed, the acting half didn't.

### 1.5 Four hook classes never ported

Silicate hooks 16 GD classes; we hook 12 of the same ones plus four of our own
(`CCKeyboardDispatcher`, `EffectGameObject`, `HardStreak`, `RingObject`).

| class | what Silicate's hook does | impact |
|---|---|---|
| `GJEffectManager` | Layout Mode clears `m_pulseEffectMap` / `m_opacityEffectMap` | our Layout Mode leaves pulse and opacity effects running — cosmetic but wrong |
| `EnhancedGameObject` | routes trigger activation away from fake trajectory players | we guard this via `ownsPreviewPlayer` in several places, so **probably covered** — needs one check |
| `EditorPauseLayer` | `onSaveAndPlay` | editor-only |
| `VideoOptionsLayer` | `onApply` | resolution change during a render |

### 1.6 The renderer has no audio monitoring

`AudioMonitorRing`, `startMonitor`, `stopMonitor`, `monitorReadCallback`,
`writeCallback`, `getBuffer` — **zero references** in our source. Silicate's
`render/dsp.cpp` has the lot. This is a whole feature, not a bug, and it's the
largest single thing we're missing from Silicate.

### 1.7 SSB gate misses one condition

Ours (`core/engine_updater.cpp:353`):

```cpp
m_ssbFix && SLRenderer::get()->isRecording()
```

Silicate's (`bot/updater.cpp:319`):

```cpp
renderer->isRecording() && renderer->m_collectAudio && m_ssbFix->inner()
```

We don't check `m_collectAudio`. So the fix applies during a silent render,
where Silicate skips it.

---

## 2. Things the audit cleared

- **Frame window: complete.** Zero missing functions in `analysis/`. I also
  diffed `stepToward` line by line — identical to Silicate's modulo our extra
  logging. Ours is 4,515 lines vs their 4,042; the difference is our logging,
  L\*, marker shapes, sound packs and CBF. **Replacing it would only delete work
  we want.**
- **Stubs: essentially gone.** One left across the whole codebase — the trail
  buffer `loadSamples`, deliberate and documented in `analysis/ac/shim.hpp`.
- **Midhook offsets: all six we have match Silicate's exactly**, including the
  `ctx.rip += 0x08` advance on `physDt` and the `m_analysisBatch` bypass on
  `physStepCount` / `restorePhysDt`.

## 3. Two false alarms, recorded so nobody re-chases them

- **The SSB fix being render-only is correct.** Silicate gates it on
  `isRecording()` too. It corrects dt while collecting audio; it is not meant to
  run during normal play.
- **`autoFlipOnDeath` is wired.** It has no `performAutoFlipOnDeath` function
  like Silicate's, but the logic is inline at `hooks/hook_playlayer.cpp:700`.

---

## 4. Why this doesn't justify ripping the engine

Seven items, of which four are small and local (§1.1–1.4), two are missing
features rather than defects (§1.5–1.6), and one is a one-line condition (§1.7).

Against that, a replacement costs: **153 `GucciEngine` call sites across 21
files**, and untangling `core/GucciBot.hpp` — 1,265 lines that are simultaneously
the engine *and* the settings bag for noclip, hitboxes, themes, Click Indicators,
the Jupiter trainer and every frame-window setting. Silicate splits those across
`bot/` + `settings/` + per-feature files. Our GBR6 format, 18 themed extensions
and the 26-format converter all hang off the replay system Silicate would
replace with `.slc`.

That is weeks of work to fix seven things we can now fix directly.

**Licensing, separately:** Silicate is **GPL-3**. GucciBot has **no LICENSE file
at all**, which is its own problem — a public repo with merged outside PRs and no
license means nobody can legally use or contribute. We're already a Silicate
derivative, so GPL-3 is almost certainly what we already owe. Worth doing on
purpose rather than by accident.

---

## 5. Recommended order

1. ~~§1.1 settings double-load~~ — **done, build `-j`**
2. §1.2 backstep cap — small, fixes a leak, makes a dead slider work
3. §1.3 + §1.4 the two missing midhooks — each makes an advertised feature real
4. §1.7 one-line SSB condition
5. §1.5 `GJEffectManager` hook; verify `EnhancedGameObject` is covered
6. §1.6 audio monitoring — its own project, lowest urgency
7. Add a LICENSE

**None of this fixes the Congregation slope bug.** That is still open, and the
audit did not find it. What it did find is that the bug is *not* a missing
Silicate mechanism in the paths I checked — so the next move there is the live
one: turn the MCP server on and read `m_hasEverJumped`, `m_lastJumpTime`,
`m_snapDistance` and `m_reverseRelated` at frame 167/168 on both runs.

---

## 6. Method, and how much to trust the numbers

Crude scripted pass, not a C++ frontend: find function-shaped definitions,
brace-match the body, strip comments and literals, then classify the body as
empty / trivial / real. Silicate 1,083 definitions across 115 files; ours 1,102
across 93.

**Measured error rate:** I checked 24 of the "missing" engine functions by hand
with grep. 22 were genuinely absent, 2 were false positives (`runFastLockDelta`
and `bumpPlayer` exist under names the parser didn't match) — about 8%. A
separate spot check found `tryHarvest` reported missing when it is defined at
`render/texture.cpp:189`.

So: **"missing by name" is a lead, not a verdict.** Many of the 129 engine-area
names are correctly absent — `processSlc2`, `processSlc3`, `toFile`, `toBytes`,
`createBackupPath` are all `.slc` file-format handling, and we use GBR6. Every
finding in §1 above was confirmed by hand, by grep and where possible against
Nigel's live `saved.json`. Nothing in §1 rests on the script alone.

*Audit and write-up by Claude (Opus 5).*
