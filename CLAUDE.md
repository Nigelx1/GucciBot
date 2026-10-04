# GucciBot 2.0 — Handoff for a Fresh Session

You are picking up an in-progress Geometry Dash mod from its author, Nigel. Read this whole doc before writing code. Section 0 is the part that has cost the most when ignored.

> **What this document is.** A map, last refreshed 2026-10-03 (build `2026-10-03-cn`, version 2.0.0-beta.2). The source is ground truth: when this doc and the repo disagree, the repo wins — and say so. The Claude Code memory store for this project holds far more history than belongs here; read its index at the start of a session.

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
- **Absense** (Absent's Silicate port): its **pathfinder is ported in whole** (`src/absense/`: the planner, its trajectory copies, the World trigger model, Silicate's physics layer for the copies) and is the default Pathfinder engine; GucciBot's own search stays as Classic. Also ideas from it: the MCP server, Check Macro, Replace All.
- **ToastyReplay** (ToastexGD) is what GucciBot started as. On 2026-09-30 Toast withdrew permission to use his code (TTR's main repo has no licence), and build -bz (385a5f7) removed every TTR-derived line. **Never port, copy or read ToastyReplay code again** (neither repo, nor the deleted files in this repo's history). Rebuilds come from Silicate (GPL-3, credit peony), Absense, or are written fresh; check new code with the ttrcheck/ttrruns tools (memory: project_ttr_removal). GucciBot stands down if ToastyReplay Lite or Silicate is *enabled* alongside it (`src/core/standdown.hpp`): it says why and, for that launch, installs none of its midhooks or patches (Silicate patches the same 13 addresses).

**Target:** GD 2.2081, Windows x64, Geode 5.10.1. **Versions (check source):** `MOD_VERSION "2.0.0-beta.2"`, `GBR6_VERSION 1`, `BRR_FORMAT_VERSION 4`.

**Branches:** `engine-port-2.0` is where 2.0 lives. `master` was fast-forwarded to it on 2026-10-02 (to take the TTR code off the default branch), so master is 2.0 code now; keep the two level. The 1.8 release download still exists as a tag/release.

---

## 2. Build & test

- `.\build_win.bat` — builds and installs. Look for `Build complete` in its output; don't trust an exit code that went through a pipe.
- `.\run_tests.bat` — four offline suites: frame-editor rules, the GBR6 format (sub-tick, TPS changes, Left/Right buttons), Check Macro, macro ops (trim/merge/diff). Compiled `/O2 /MD` to match the mod (this toolchain fast-fails with cl's defaults).
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
src/ui/         the ImGui interface, rewritten 2026-10-01: ui.hpp (all the rest of the mod sees), look.*
                (palette, fonts), kit.* (cards/rows/controls), themes.*, shell.cpp (window, pages,
                overlay, saved settings, the ImGuiCocos draw hook that also ticks anticroom's analyzer),
                pages/ (one file per page or card)
src/absense/    Absense's pathfinder, ported whole: pathfinder/, trajectory/ (its copies of the
                player), world/ (trigger model), physics/ (Silicate's collisions for the copies),
                compat/ (Silicate names -> GucciBot), glue.hpp (the only header GUI/MCP include),
                judge.* (play a script for real, catch the death, put the game back)
src/mcp/        Assistant Access server and tools (incl. restart_game/open_level/quit_level,
                tick_probe, sim_vs_real: a session can rebuild and test without Nigel)
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

## 6. Current state (2026-10-02)

- **2.0.0-beta.2** is the latest pre-release (it still contains TTR code; whether to pull old downloads is Nigel's call). Stable ("latest") is still 1.8.
- **Build -bz removed all TTR-derived code; the rebuild is done (2026-10-03, builds -ca..-cn, all UNTESTED in-game).** Rebuilt fresh or from Silicate/Absense/anticroom: themes, autoclicker, click sounds, hitboxes, the fork service (src/analysis/trajectory.* on Absense's copies: path preview, Frame Extrapolation, Prevent Death look-ahead, Find Best Tick, sub-tick preview, Agency Map, Classic ranking), HUD, frame editor, Calculate page (+ settings persistence), JMF Trainer + Trainer, Survival Indicator + calibration, noclip accuracy, Video Mode, Macro Tools, Editor Tools, Assistant Access switch, presets, and menu controls for every engine setting. **Not rebuilt:** the legacy BRR reader (no allowed source for the format, no known users). README's License section records that ToastyReplay's source was used up to 2.0.0-beta.2 - agreed with ToastexGD, keep it.
- **Roadmap (Nigel, 2026-10-03):** his in-game test of the rebuild -> release 2.0.0-beta.3 -> the AI pathfinder (an AI that drives the controller inside the simulator) -> frame-window analyzer fixes -> 2.0.0. Promo plan in memory: project_promo_vouches.
- **Open:** the Bloodbath "Michigun UFO" pathfinder stall (every idea dies on the same tick; escalation past its cap repeats cached searches - see memory project_pathfinder); GitHub issues #8, #11, #13, #14 (#14's cause fixed in -by); restarting right after a completed replay skips the next attempt's level end; the judge leaves the one-step flag armed.
- Claude sees plan usage live (hooks; memory reference_usage_tracking). The memory store has the detail on each: read `MEMORY.md` there first.

## 7. Other docs in the repo

`ENGINE_AUDIT.md` is current. `CALC_SLOPE_EXIT.md` is superseded history. `about.md` (packaged into the mod) and `README.md` describe features and credits; keep their credits identical to the in-game Credits tab and the website.
