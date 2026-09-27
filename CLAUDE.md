# GucciBot 2.0 — Handoff for a Fresh Session

You are picking up an in-progress Geometry Dash mod from its author, Nigel. Read this whole doc before writing code. Section 0 is the part that has cost the most when ignored.

> **What this document is.** A map, last refreshed 2026-09-27 (build `2026-09-27-ar`, version 2.alpha.3). The source is ground truth: when this doc and the repo disagree, the repo wins — and say so. The Claude Code memory store for this project holds far more history than belongs here; read its index at the start of a session.

---

## 0. How to work on this project

Nigel is the owner and the only person who runs the mod in-game (Juice co-tests, relayed through him). He doesn't read C++. The only evidence that something works is that it worked in the game.

- **Batching is allowed; stacking blind is not.** The original rule here was "one change, build, confirm in-game, then the next", written after a from-scratch rewrite stacked untested changes and broke the project. Nigel overrode the pacing on 2026-07-27 ("all fixes come at once in bulk"). What survived: **one commit per logically separate change**, so a broken batch can be bisected.
- **Build every change** (`.\build_win.bat`) before committing it. It also installs the `.geode` into GD's mods folder, so the build Nigel launches next is the one you just made.
- **Bump `GB_BUILD_LABEL` on every build** (Section 2), then commit and push. Standing authorization; no need to ask each time. Testing the wrong binary has wasted real time here.
- **If you are not sure a change is correct, say "I'm not sure" and why.** Never ship a guess as a fix. A lot of this project's bugs were confident claims nobody checked.
- **Releases wait for Nigel's in-game test** — build, he tests, then version bump and release. Pushing source is not releasing. He can override this for a release (he did for 2.alpha.3); raise it once, then do what he decides.

---

## 1. What GucciBot is

A **Geometry Dash macro bot**, distributed as the Geode mod `nigelx1.guccibot` (Nigel goes by "Nigelx1"). It records inputs frame-perfectly and plays them back, measures click timing windows (Calculate), finds its own solutions (Pathfinder), renders to video, and has practice trainers.

- **Engine:** Silicate, by peony (GPL-3). GucciBot's engine is a port of it, reconciled function by function in 2.0.
- **Calculate (frame-window analyzer), sub-tick CBF recording (SCBF), the render intro card:** anticroom's, from his Silicate fork, ported in near-verbatim.
- **Ideas from Absense** (Absent's Silicate port): Pathfinder improvements, the MCP server, Check Macro, Replace All.
- **ToastyReplay** (ToastexGD; Nigel co-owns it) is what GucciBot started as. ToastyReplay Lite must be installed (not enabled) for GucciBot to run.

**Target:** GD 2.2081, Windows x64, Geode 5.10.1. **Versions (check source):** `MOD_VERSION "2.alpha.3"`, `GBR6_VERSION 1`, `BRR_FORMAT_VERSION 4`.

**Branches:** `engine-port-2.0` is where 2.0 lives. `master` is still 1.8 (the stable release) and fast-forwards once 2.0 is confirmed.

---

## 2. Build & test

- `.\build_win.bat` — builds and installs. Look for `Build complete` in its output; don't trust an exit code that went through a pipe.
- `.\run_tests.bat` — three offline suites: frame-editor rules, the GBR6 sub-tick section, Check Macro. Compiled `/O2 /MD` to match the mod (this toolchain fast-fails with cl's defaults).
- **`GB_BUILD_LABEL`** is at the top of `src/core/GucciBot.hpp`: line 3 is `#define GB_BUILD_LABEL \`, line 4 the string. Scheme: `YYYY-MM-DD-x` then `-aa`, `-ab`... within a day, plus a plain-English description of what changed. Edit it by finding the line that ends in a backslash, never by searching for the label's closing text (a bad scan once deleted ~300 lines of this header). No `"` or `\` in the text.
- **In-game logs** live in `%LOCALAPPDATA%\GeometryDash\geode\mods\nigelx1.guccibot\` (`guccibot_slope.log`, `guccibot_fw.log`, ...). Read them yourself instead of asking Nigel to paste.
- **Assistant Access** (Settings tab) is an MCP server on `127.0.0.1:8790`, off by default: a session can read game and player state, step the game and run Calculate or Pathfinder live.

---

## 3. Where things live

```
src/core/       GucciBot.hpp (engine + settings bag), engine_core.cpp (settings loader,
                practice fix, save/load), engine_updater.cpp (tick loop, midhooks, drawScene),
                action_types.hpp (gb::Action incl. m_subtick), gbr6_format.* (the macro format)
src/hooks/      $modify hooks per GD class; respawn.* (custom respawn delay)
src/analysis/   trajectory.* (forks for path preview, Frame Extrapolation, Pathfinder probe,
                sub-tick preview), pathfinder.*, ac/ (anticroom's analyzer + shim.hpp mapping
                Silicate names onto GucciBot's)
src/replay/     scbf_input.* (live sub-tick recorder), subtick_preview.*
src/render/     renderer.* (async PBO ring, encode thread), dsp.* (audio capture + preview),
                intro.*, texture.*
src/trailbuf/   Silicate's trail buffer (Macro Buffing, spikes)
src/gui/        the ImGui interface (gui.cpp is very large), frame editor
src/mcp/        Assistant Access server and tools
src/tools/      pure, testable cores (edit_core, macro_check), Replace All, self-check
tools/          offline test programs run by run_tests.bat
ENGINE_AUDIT.md the 2026-09-26/27 audit against Silicate: every finding and how it closed
```

---

## 4. The frame model (get this exactly right)

- **Frames are absolute.** Frame 0 is the start of the level, always. A point in the level maps to the same frame on every attempt.
- **Respawning must not reset the counter to 0.** A checkpoint respawn resumes at the checkpoint's frame.
- **GucciBot is one frame ahead of Silicate.** It records an input at `getFrame() + 1` and looks inputs up at `frame + 1`; Silicate uses `frame` for both. Keep the two consistent when porting anything that keys on frames (the CBF engine's arm/capture only need to agree with each other).
- In `handleResetWithCheckpoints`, **the order of the reset checks is load-bearing** (the "233→107" bug came from checking `m_canDie` before the saved checkpoints).
- GD's player physics runs in **sixtieths of a second**: `PlayerObject::update(dt)` takes a 240 TPS tick as `0.25`, not `1/240`. Any fork you step by hand needs `60 / tps`.

---

## 5. Reference code

- **Silicate:** https://git.puppy.lgbt/silicate/silicate (self-hosted, not GitHub; it moved from git.silicate.dev). Use `curl` on raw URLs; web fetch tools only return summaries. Local copies Nigel has handed over live in `~/Downloads` (`build.zip`, anticroom's `slc count.zip`, `Absense-source-*.zip`). Grep any drop from anticroom for harassment before porting it — an earlier one contained abuse aimed at peony.
- **Click Between Frames:** github.com/theyareonit/Click-Between-Frames. GucciBot pauses it (and Superb Input Precision) while recording or playing, and restores it after.
- **The recurring bug here is the half-port:** the storage or GUI half of a Silicate mechanism landed and the acting half didn't. It shows up as an empty body, a setting nothing reads, a hook that only calls the original, a value computed and thrown away, or a midhook missing its `rip` skip. When something "does nothing", check Silicate's version first, not last.

---

## 6. Current state (2026-09-27)

- **2.alpha.3** is released as a GitHub pre-release, at Nigel's call without an in-game smoke test. Stable ("latest") is still 1.8.
- **Open:** GitHub issues #7–#10 (#9/#10 are macros losing inputs; Click Between Frames is the leading suspect, unconfirmed); a LICENSE file (GucciBot contains GPL-3 Silicate code); the Congregation slope bug in Calculate (narrowed to one GD routine on one frame); Pathfinder v2 step 4.
- The memory store has the detail on each: read `MEMORY.md` there first.

## 7. Other docs in the repo

`ENGINE_AUDIT.md` is current. `CALC_SLOPE_EXIT.md` is superseded history. `about.md` (packaged into the mod) and `README.md` describe features and credits; keep their credits identical to the in-game Credits tab and the website.
