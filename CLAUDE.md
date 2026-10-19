# GucciBot 2.0 — Handoff for a Fresh Session

You are picking up an in-progress Geometry Dash mod from its author, Nigel. Read this whole doc before writing code. Section 0 is the part that has cost the most when ignored.

> **What this document is.** A map, last refreshed 2026-10-13 (build `2026-10-13-da`, version 2.0.0-beta.4). The source is ground truth: when this doc and the repo disagree, the repo wins — and say so. The Claude Code memory store for this project holds far more history than belongs here; read its index at the start of a session.

---

## 0. How to work on this project

Nigel is the owner and the only person who runs the mod in-game (Juice co-tests, relayed through him). He doesn't read C++. The only evidence that something works is that it worked in the game.

- **Batching is allowed; stacking blind is not.** The original rule here was "one change, build, confirm in-game, then the next", written after a from-scratch rewrite stacked untested changes and broke the project. Nigel overrode the pacing on 2026-07-27 ("all fixes come at once in bulk"). What survived: **one commit per logically separate change**, so a broken batch can be bisected.
- **Build every change** (`.\build_win.bat`) before committing it. It also installs the `.geode` into GD's mods folder, so the build Nigel launches next is the one you just made.
- **Bump `GB_BUILD_LABEL` on every build** (Section 2), then commit and push. Standing authorization; no need to ask each time. Testing the wrong binary has wasted real time here.
- **If you are not sure a change is correct, say "I'm not sure" and why.** Never ship a guess as a fix. A lot of this project's bugs were confident claims nobody checked.
- **Releases wait for Nigel's in-game test** — build, he tests, then version bump and release. Pushing source is not releasing. He can override this for a release (he did for 2.alpha.3, beta.3 and beta.4); raise it once, then do what he decides.

---

## 1. What GucciBot is

A **Geometry Dash macro bot**, distributed as the Geode mod `nigelx1.guccibot` (Nigel goes by "Nigelx1"). It records inputs frame-perfectly and plays them back, measures click timing windows (Calculate), finds its own solutions (Pathfinder), renders to video, and has practice trainers.

- **Engine:** Silicate, by peony (GPL-3). GucciBot's engine is a port of it, reconciled function by function in 2.0.
- **Calculate (frame-window analyzer), sub-tick CBF recording (SCBF), the render intro card:** anticroom's, from his Silicate fork, ported in near-verbatim.
- **Absense** (Absent's Silicate port): its **pathfinder is ported in whole** (`src/absense/`: the planner, its trajectory copies, the World trigger model, Silicate's physics layer for the copies) and is the default Pathfinder engine; GucciBot's own search stays as Classic. Also ideas from it: the MCP server, Check Macro, Replace All.
- **ToastyReplay** (ToastexGD) is what GucciBot started as. On 2026-09-30 Toast withdrew permission to use his code (TTR's main repo has no licence), and build -bz (385a5f7) removed every TTR-derived line. **Never port, copy or read ToastyReplay code again** (neither repo, nor the deleted files in this repo's history). Rebuilds come from Silicate (GPL-3, credit peony), Absense, or are written fresh; check new code with the ttrcheck/ttrruns tools (memory: project_ttr_removal). GucciBot stands down if ToastyReplay Lite or Silicate is *enabled* alongside it (`src/core/standdown.hpp`): it says why and, for that launch, installs none of its midhooks or patches (Silicate patches the same 13 addresses).

**Target:** GD 2.2081, Geode 5.10.1 (Nigel runs 5.11.1 fine). Windows x64 is home (tested in-game by Nigel); macOS, iOS and Android since beta.3, built by CI (`.github/workflows/multi-platform.yml`). The Android build has run on a phone (Nigel, 2026-10-13); macOS and iOS have had little testing. `GB_NATIVE_ENGINE` (`src/core/platform.hpp`) marks what needs the Windows engine; memory project_multiplatform has the rest. **Versions (check source):** `MOD_VERSION "2.0.0-beta.4"`, `GBR6_VERSION 1`, `BRR_FORMAT_VERSION 4`.

**Branches:** work happens on `multiplatform`; every push to it makes GitHub build the universal package (Section 2). `master` and `engine-port-2.0` are fast-forwarded to it at each release (last: beta.4, 2026-10-13), so all three are 2.0 code; keep them level. `master` became 2.0 code on 2026-10-02, to take the TTR code off the default branch. The 1.8 release download still exists as a tag/release.

---

## 2. Build & test

- **One universal build (Nigel, 2026-10-12).** Every push to `multiplatform` makes GitHub build ONE `.geode`: all five platforms plus every resource. The renderer's FFmpeg DLLs come from the release asset in [guccibot-build-files](https://github.com/Nigelx1/guccibot-build-files), and the build checks their SHA-256. That file is THE build:
  - releases ship it as-is, with no more swapping in a local Windows binary;
  - testers get it;
  - `.\install_ci_build.bat` puts it in Nigel's game after you push. It waits about 8 minutes for the build, checks the 5 binaries and 7 DLLs, and keeps a copy in `build\ci\`.
- `.\build_win.bat` — a quick local Windows build. It installs too, but it's not the shipped file. Look for `Build complete` in its output; don't trust an exit code that went through a pipe.
- `.\run_tests.bat` — four offline suites: frame-editor rules, the GBR6 format (sub-tick, TPS changes, Left/Right buttons), Check Macro, macro ops (trim/merge/diff). Compiled `/O2 /MD` to match the mod (this toolchain fast-fails with cl's defaults).
- **`GB_BUILD_LABEL`** is at the top of `src/core/GucciBot.hpp`: line 3 is `#define GB_BUILD_LABEL \`, line 4 the string. Scheme: `YYYY-MM-DD-x` then `-aa`, `-ab`... within a day, plus a plain-English description of what changed. Edit it by finding the line that ends in a backslash, never by searching for the label's closing text (a bad scan once deleted ~300 lines of this header). No `"` or `\` in the text.
- **In-game logs** live in `%LOCALAPPDATA%\GeometryDash\geode\mods\nigelx1.guccibot\` (`guccibot_slope.log`, `guccibot_fw.log`, ...). Read them yourself instead of asking Nigel to paste.
- **Assistant Access** (Settings tab) is an MCP server on `127.0.0.1:8790`, off by default: a session can read game and player state, step the game and run Calculate or Pathfinder live.

---

## 3. Where things live

```
src/core/       GucciBot.hpp (engine + settings bag), engine_core.cpp (settings loader,
                practice fix, save/load), engine_updater.cpp (tick loop, midhooks, drawScene),
                action_types.hpp (gb::Action incl. m_subtick), gbr6_format.* (the macro format),
                bot_switch.* (Disable bot), setup_check.* (the setup check), standdown.hpp,
                platform.hpp + portable_tps.* (what runs off Windows)
src/hacks/      safemode.* (auto safe mode, the cheat dot, the owner lock: only a hash is in the
                code, never the password), keybinds.cpp (hotkeys), autoclicker.*, hitboxes.*,
                hud.*, noclip_accuracy.*, indicator.* (Survival Indicator)
src/trainers/   JMF Trainer + Trainer (trainer_core.*), ghosts, calibration, Video Mode
src/audio/      BIG BRRRR, click sounds, speedhack audio
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
                overlay, saved settings, the Credits page, the ImGuiCocos draw hook that also ticks
                anticroom's analyzer), pages/ (one file per page or card), touch.* (phone drag-to-
                scroll), pause_button.cpp (the pause-menu button on phones and Mac)
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

## 6. Current state (2026-10-13)

- **2.0.0-beta.4** is the latest pre-release (2026-10-13): auto safe mode (dihmaster500's ask), the setup check, the Disable bot switch, the phone menu, the Mac menu key, renderer files that fill themselves in, hotkeys that remember. It was the first release shipped as GitHub's universal package, unchanged. Nigel tried builds -cp..-cx in-game; -cy (renderer self-heal) and -cz (hotkeys) went out untested at his call. Stable ("latest") is still 1.8.
- **Roadmap to 2.0 (Nigel, 2026-10-13):**
  1. AI pathfinder.
  2. Calculate tweaks: flag suspicious clicks, with suggestions.
  3. Port every feature to Mac, Android and iOS.
  4. Minor features and bugs.
  5. A how-to and a VirusTotal report.
  6. A round of polish.

  Then the release and the promo video. **The live checklist for everything left before 2.0, in Nigel's order (2026-10-19: credits, minor features and bugs, Calculate, AI pathfinder, phone/Mac parity, the standalone copy, how-to, polish), is `docs/ROAD_TO_2_0.md`: tick its boxes as things land.** The history behind each item is in memory project_road_to_2_0. Next: R0 of AI_PATHFINDER_PLAN.md: measure the simulator, then check it on Bloodbath (a known-good route through it, and why the World switches off there). Before 2.0 he also wants everything still Windows-only ported to Mac/iOS/Android, except the pathfinder, which is being remade. The frame-window fixes follow his design: Calculate counts strictly as now; for suspicious clicks it also counts Claude's way, and the frame editor flags that click with "my suggestion is x frames". The promo video comes out alongside 2.0.
- **The TTR-free rebuild (builds -ca..-cn) shipped in beta.3.** Most of it has had no feature-by-feature human test; Nigel says that can wait. Not rebuilt: the legacy BRR reader (dropped) and render presets (rebuild later). README's License section records that ToastyReplay's source was used up to 2.0.0-beta.2 - agreed with ToastexGD, keep it.
- **Before 2.0 too (Nigel, 2026-10-13):** a feature-by-feature test of the rebuild, following up reporters' logs, and the feature requests (Eclipse speedhack override, moving wave spam #25, fbmod .flp in the converter #19). **Known bugs:** Mirror Inputs queues inputs into the list it's looping over (src/hooks/hook_gjbasegamelayer.cpp, about line 572); restarting right after a completed replay skips the next attempt's level end. The current pathfinder's open items (the Bloodbath "Michigun UFO" stall, the judge's one-step flag) are moot: the AI pathfinder replaces it. The 12 open GitHub issues get reply drafts later (Claude drafts, Nigel posts).
- Claude sees plan usage live (hooks; memory reference_usage_tracking). The memory store has the detail on each: read `MEMORY.md` there first.

## 7. Other docs in the repo

`docs/ROAD_TO_2_0.md` is the checklist to 2.0 (above). `AI_PATHFINDER_PLAN.md` is the plan for the AI pathfinder (2026-10-13). It's a route planner that reads the whole level, plans a route of decisions (waypoints) start to finish, and has GD's physics play each leg, trying every combination and proving dead ends. It backjumps to the decision that caused a proven dead end. No training; an optional trained decider later. `docs/AI_PATHFINDER_RESEARCH.md` holds the Bloodbath evidence and both full designs. `ENGINE_AUDIT.md` is current. `CALC_SLOPE_EXIT.md` is superseded history. `about.md` (packaged into the mod) and `README.md` describe features and credits; keep their credits identical to the in-game Credits tab and the website.
