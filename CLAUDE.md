# GucciBot v10.0.0 — Handoff for a Fresh Model

You are picking up an in-progress Geometry Dash mod from its author. Read this whole doc before writing a single line of code. The most important section is Section 0; the project broke because that section was ignored, and it will break again if you ignore it too.

> **What this document is, and isn't.** This is reconstructed from the project's development history, not from a live read of the current repository. Treat every file name, field name, and code snippet below as a *strong lead to verify against the actual source on disk*, not as ground truth. The source files are ground truth. When this doc and the repo disagree, the repo wins — and tell the author.

---

## 0. How to work on this project (non-negotiable)

This project is currently unstable. It got that way from a well-intentioned from-scratch rewrite where many changes were stacked on top of each other and the result was only tested at the end. By then it was impossible to tell which change broke what. **Do not repeat this.**

The workflow, every time, no exceptions:

1. **Start from a build that is known to work.** Before touching anything, ask the author (Nigel) which build is the last known-good floor. Do not start from the current broken state and try to patch forward. Make the known-good build your baseline.
2. **Change exactly one thing.**
3. **Build it.** (`.\build_win.bat`)
4. **Have Nigel confirm it in-game.** He is the only tester, and the only evidence that counts is whether it worked in the actual game — not whether the code looks correct.
5. **Only then** move to the next change.

Two more rules:

- **If you are not sure a change is correct, say "I'm not sure" and explain the uncertainty.** Do not ship a guess as if it were a fix. A stacked speculative build is how this project got broken.
- **Bump the build label on every single build** (see Section 2). Half the confusion in this project came from testing the wrong binary.

You will be tempted to "just fix everything at once" because you can see several problems. Resist it. One change, build, confirm, next.

---

## 1. What GucciBot is

GucciBot is a **Geometry Dash macro bot**, distributed as a **Geode mod** (`guccimanefan.guccibot`). It records the player's inputs frame-perfectly and plays them back. The author is **Nigel**, who goes by **"fable"** in the GD community.

It is built on top of three other projects:

- **Silicate** — physics engine (by peony). The reference implementation for the hard parts, especially intentional deaths.
- **TTR (ToastyReplay)** — renderer (by ToastexGD). Handles rendering macros out to video.
- **GBR6** — the macro compression / file format.

**Target:** Geometry Dash 2.2, x64, Windows.

**Versions (verify in source):**
- `MOD_VERSION "10.0.0"`
- `GBR6_VERSION 1`
- `BRR_FORMAT_VERSION 4`
- Geode `5.7.1`

---

## 2. Build & test

- **Build command:** `.\build_win.bat` (run on Windows, builds the x64 mod).
- **Build label:** there is a `GB_BUILD_LABEL` constant in `GucciBot.hpp`. It is printed at startup and shown in the Diagnostics UI. **Change it on every build** (e.g. a date-letter scheme like `2026-06-18-a`) so you and Nigel are always certain which binary is running. This is not optional housekeeping — testing a stale binary wasted real time on this project.
- **Testing:** Nigel builds and tests **directly, in-game, himself.** There is no separate tester. Don't design around an automated test harness that doesn't exist; design around "Nigel runs it and reports what he saw."

---

## 3. Architecture & where things live

**GucciBot source (verify exact paths):** `src/replay_engine.cpp`, `src/brr_format.hpp`, `src/gui.cpp`, `src/physicsbypass.cpp`, `src/safemode.cpp`, plus a checkpoint system file. There is an ImGui-based GUI.

**Reference codebases that should be on disk for diffing:**
- `silicate/` — diff against this for physics / intentional-death logic.
- `ToastyReplay-main/` (TTR) — organized into `src/core/`, `src/hacks/`, `src/tools/`, `src/gui/`. Diff against this for rendering and for the replay engine.

**GucciBot-specific additions** that don't exist in the upstream projects (so don't expect to find them by diffing): CBF recording, `kQueuedCommandMatchTolerance`, `attemptBaseTick`.

### Key classes (reconstructed — verify against current source)

**`GucciUpdater`** — owns timing, the frame counter, intentional-death state, and frame advance/stepping:

```cpp
class GucciUpdater {
public:
    enum class LockDeltaMode { Performance = 0, Accuracy = 1 };

    // --- timing ---
    double   m_tps           = 240.0;
    double   m_speedhack     = 1.0;
    double   m_tpsOverflow   = 0.0;
    bool     m_lockDelta     = true;
    LockDeltaMode m_lockDeltaMode = LockDeltaMode::Performance;
    bool     m_realTime      = false;
    uint32_t m_maxUPR        = 10;
    bool     m_useVisualUpdates = true;
    bool     m_speedhackAudio = true;

    // --- frame advance / stepping ---
    bool     m_paused        = false;
    bool     m_stepOnce      = false;
    bool     m_stepBackwards = false;
    bool     m_backwardsStepping = false;
    uint32_t m_maxBackstepFrames = 120;

    // --- intentional death ---
    bool     m_canDie        = false;  // user toggled "next death is intentional"
    bool     m_expectsDeath  = false;  // playback: a Death action is coming
    bool     m_inputIsDeath  = false;  // came from delayedResetLevel (not fullReset)
    bool     m_fullReset     = false;  // came from fullReset

    // --- visual state ---
    bool     m_onlyRefresh   = false;
    bool     m_shouldRender  = true;
    // ...
};
```

**The macro / replay object** exposes roughly:

```cpp
Action getNextInput(uint32_t frame);
std::optional<slc::Action> getCurrentQueuedInput() const;
std::optional<slc::Action> getNextQueuedInput() const;

void   advanceInputIndex() { m_inputIndex++; }
size_t getInputIndex() const { return m_inputIndex; }

void   onReset(uint32_t newFrame);
void   onExit() { m_inputIndex = 0; }

// File I/O — saves as BRR with an slc payload
void   load(const std::filesystem::path& path);
void   save(const std::filesystem::path& path, bool noOverwrite = false);
std::filesystem::path getCurrentPath() const;
void   backupExisting(const std::filesystem::path& path);
void   createBackup();
```

---

## 4. The frame-counting model (get this exactly right)

This is the conceptual core, and a lot of bugs trace back to getting it wrong.

- **Frames are absolute.** Frame 0 is the **start of the level, always.** It is never anything else.
- **A given point in the level always maps to the same absolute frame.** At 240 TPS, a 10-second level is ~2400 frames, so 1% of that level is ~frame 24, 2% is ~frame 48, and so on. (Exact numbers depend on level length; the principle is what matters.)
- **Respawning must NOT reset the counter to 0.** When the player respawns at a checkpoint, the frame should be the checkpoint's frame, not 0. Only the level start is frame 0.

**Known bug class here:** the counter was resetting to 0 on every respawn, producing `respawn@0` on every death even when the player died at frame 233. The underlying cause in one instance was a stored-frames array being empty when read, so the respawn-frame computation fell back to 0.

**Related ordering bug (the "233→107" bug):** in `handleResetWithCheckpoints`, the code checked `m_canDie` *before* checking `m_savedCheckpoints`, which caused checkpoint respawns to be mislabeled as full restarts. When working in this area, watch the order in which reset conditions are evaluated.

---

## 5. Current broken state (in priority order)

### P1 — Intentional deaths are broken

**This is the big one, and the key insight is architectural:**

- **The recording side has always worked** and was in GucciBot before any recent work: `intentionalDeathPending`, the GUI toggle, and the `deathFrames` array being populated in `safemode.cpp`. Marking a death as intentional while recording = fine.
- **The playback side was never natively part of GucciBot.** Silicate plays intentional deaths back correctly because Silicate was *built from scratch around that feature*; the death flows through its whole architecture. GucciBot only ever had the recording scaffolding. Every attempt to bolt playback onto a system that wasn't designed for it is what has caused the pain — including the recent rewrite.

**Current symptom (Nigel's words):** on an intentional-death run, the action count doesn't increase until the actual death (but increases normally on non-int-death runs); and on playback, only the run containing the intentional death plays.

**Where the logic lives in Silicate (study this before reimplementing):** the death flows through `BotUpdater`, `PracticeFix`, `delayedResetLevel`, `checkIfResetWasExpected`, `handleResetWithCheckpoints`, and the `slc::Action::ActionType::Death` action type. The relevant `GucciUpdater` flags are `m_canDie` (user armed an intentional death), `m_expectsDeath` (playback knows a Death action is coming), `m_inputIsDeath` (reset came from `delayedResetLevel`), and `m_fullReset` (reset came from `fullReset`).

**Recommendation:** do not try to re-bolt this in one big pass. Get a build where recording works and playback is in its original (non-intentional-death) state, confirm that's stable, and then add playback **one piece at a time** with a test after each.

### P2 — Renders are broken (250 bytes)

Render output comes out as **~250 bytes** — i.e. essentially empty. This has never been diagnosed or fixed. This is on the **TTR renderer** side. Start by confirming what a *correct* render's size/structure should look like and where the 250-byte file is actually being written, before changing renderer logic.

### P3 — Frame-window analyzer ("Calculate") — unverified

The analyzer measures per-click timing windows by running async probes across real game frames. It was rewritten in a prior session using a **probe-atom + `Mode::Playing`** approach (build `2026-06-18-a`). During that rewrite a real bug was fixed — `saveCurrent()` during capture was pushing stale entries into `m_savedCheckpoints`, causing incorrect frame-offset restores and "moves 0.0 / dies every probe" behavior — **but the rewrite was never verified to actually work.** Treat it as unproven. **Read `ANALYZER_HANDOFF.md` in the repo before touching this.**

### P4 — Calculate UI display bug

The Calculate result message renders **only ~5 characters per line.** This is almost certainly an ImGui text-width / wrapping / buffer-sizing issue in the GUI, not a logic bug in the analysis itself. Cheap to investigate once the analyzer (P3) is actually working; not worth chasing while P3 is unproven.

---

## 6. What changed recently (so you don't walk into it)

A from-scratch rewrite was attempted using both the GucciBot and Silicate source ("the greatest bot possible from these two codebases"). It was a large, multi-file, speculative change set, and it was effectively tested only at the end. That is the proximate cause of the current instability. The intentional-death playback work was part of this and was at one point scrapped back to GucciBot's original behavior, then reattempted.

The lesson the author has already drawn, and the one this handoff is built around: **revert to a known-good build and move one change at a time.** Please honor it.

---

## 7. Existing docs in the repo

- `ANALYZER_HANDOFF.md` — read before touching the frame-window analyzer.
- `CLAUDE_CODE_CONTEXT.md` — prior context handoff.

Read both before working in their areas.

---

## 8. Your first three actions

1. **Ask Nigel which build is the last known-good floor.** Don't start anywhere else. Make it the baseline and bump `GB_BUILD_LABEL`.
2. **Pick exactly one bug** — P1 (intentional deaths) or P2 (renders) are the highest-value. **Read the actual current source for it.** Do not trust this document's code snippets as the live state; they are reconstructed and may be stale.
3. **Make one change, build, have Nigel confirm in-game, and only then continue.**

If at any point you find yourself about to make a second change before the first was confirmed working in-game — stop. That is the exact failure mode that produced this handoff.
