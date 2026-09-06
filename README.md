# GucciBot

> Frame perfect. Ice cold. Brrr Brrr Brrr.

It's Gucci. GucciBot is a Geometry Dash macro bot (a [Geode](https://geode-sdk.org/) mod) that records your inputs frame-perfectly and plays them back exactly, built on top of [Silicate](https://git.puppy.lgbt/silicate/silicate)'s physics engine and ToastyReplay's renderer. Think of it as a very expensive, very icy ghost of your best run — except the ghost never drops a frame, never has an off day, and definitely never forgets its jewelry. So icy.

GucciBot is public now — grab a build from [Releases](https://github.com/guccimanefan/GucciBot/releases/latest), drop it in your Geode mods folder, and you're in. It's not on the in-game Geode mod index (long story, not going into it here), so a manual install is the only way to get it for now.

**Requires ToastyReplay Lite (`toastexgd.toastyreplay-lite`) to be installed** (it doesn't need to be enabled, just present in your mods folder) — GucciBot won't run without it. This is the one real condition on GucciBot's release, and it's checked every launch.

---

## Table of Contents

- [What it actually does](#what-it-actually-does)
- [GBR6 — the hypercompressed format](#gbr6--the-hypercompressed-format)
- [Practice & analysis](#practice--analysis)
- [Autoclicker](#autoclicker)
- [Trainers](#trainers)
- [Rendering](#rendering)
- [Themes](#themes)
- [Building it](#building-it)
- [Status](#status)
- [Credits](#credits)

## What it actually does

- Records inputs at frame-perfect precision and plays them back the same way, every time.
- Full checkpoint/practice-fix system ported from Silicate, so restores actually behave like they should instead of leaving hazards in whatever state GD's own native restore felt like that day.
- A frame-window analyzer ("Calculate") that measures, per click, how many frames you actually had to work with — built from a real GD-community algorithm (see [Status](#status), because honesty matters more than a clean README).
- In-engine video rendering straight to a file, no external recorder needed.
- A whole closet of themes, because if the bot's going to carry you through a level it should at least look like money doing it.

## GBR6 — the hypercompressed format

- 2 bytes per input — matches yBot-sized files for regular human gameplay.
- Tap encoding: a wave/ship press+release pair stores as one entry instead of two.
- Autoclicker delta compression: an autoclicker macro of *any* length — octillions of clicks, if you're feeling brave — stores in 4 bytes flat. It stores the pattern's description, not its output.
- Old `.brrr` files still load. GBR6 doesn't forget where it came from.

## Practice & analysis

- Macro diff viewer — put two replays side by side, frame by frame.
- Calculate — per-click survivability windows measured against real game frames, not a guess. Three selectable algorithms now: Time-Based (default), Recovery Range, and Alignment-Independent (also re-tests the previous click's own timing, not just this one). Optional "Circle Skin" marker style. Megahack's Practice Fix is heavily recommended before running it.
- Mid-macro TPS changes, noclip accuracy readout, macro trim/merge/surgery.
- Bot settings presets, a metadata editor, autosave on a timer or at level end.

## Autoclicker

- Fully independent Hold Ticks / Release Ticks / Clicks Per Hold per player — no shared setting forced onto both, matching Silicate's own model.
- One-shot "Sync Player 2 to Player 1" copy, not a permanent link.
- Only While Holding — auto-clicks only while you're actually holding the jump input.

## Trainers

Two flavors of the same toolset: one permanently pointed at the level "Jupiter My Favourite," one pointed at whatever macro you feel like practicing against.

- **Click Trainer** — a scrolling rhythm bar with a fixed center line; your own presses/releases render live next to the macro's, so you can see exactly how early or late you actually were.
- **Ghosts** — the macro's ghost and your own best-attempt ghost, both live in the world, scrubbable.
- **Segments** — name the hard parts, loop just those, get auto-suggested splits from click density.
- **Stats** — attempts, session best, a death-position heatmap for when you want to know exactly where the level is bullying you.
- Synced music, because grinding a segment in silence is a crime. Trap music, ideally.

## Rendering

- Full gameplay capture to video via FFmpeg — resolution, FPS, bitrate, codec, hardware encoders auto-detected per GPU vendor.
- Optional 4-track audio output (combined, music, SFX, and frame-window cues each isolated) instead of one flattened track.
- Frame-window markers and cues bake right into the output video.
- Save and reload full render configs by name.

## Themes

The full current roster. Pick one, or build your own — Settings → Theme has a full **Create Your Own Theme** editor (colors, identity, quotes, your own Big Brrr track) for exactly this occasion.

| Theme | Extension | Colors |
|---|---|---|
| GucciBot | `.brrr` | Gold + Black |
| ToosiiBot (LSU / Syracuse / Sac State) | `.toosii` | Purple+Gold / Orange+Navy / Green+Gold |
| JaBot | `.ja` | Navy + Light Blue |
| GiddeyBot | `.giddey` | Thunder Blue + Orange |
| BamBot | `.bam` | Heat Black + Red |
| SexyyBot | `.sexyy` | Hot Pink + Purple |
| JuiceBot | `.juice` | Coral + Teal |
| ButlerBot | `.butler` | Bulls Red + Black |
| SaweetieBot | `.saweetie` | Hot Pink + Plum |
| MaybachBot | `.maybach` | Platinum + Black |
| RomoBot | `.romo` | Cowboys Navy + Silver |
| GrizzleyBot | `.grizzley` | Charcoal + True Red |
| **Red Kingdom** | `.redkingdom` | Blood red that *pulses*, because a static color wasn't intense enough |
| LemonadeBot | `.lemonade` | Bright Yellow + Warm Black |
| BrrrBot | `.icebrrr` | Icy Blue + Silver, with an actual blizzard overlaid on the whole panel |
| WakaBot | `.waka` | Grove Green + Black |
| YoungstaBot | `.youngsta` | Black + Red (223) |
| KnockerzBot | `.knockerz` | Teal + Black |
| *Yours* | — | Whatever you want. That's the point. |

GucciBot's own color is gold, for the record. Yellow everything this time, you know what I'm talking about? Yellow rims. Yellow big booty yellowbones, ha. Yellow Lambs, yellow MPs, yellow watch. Yellow charm ring, chain. Yellow living room set.

Also in Settings: **BIG BRRRR**, a core feature of this mod, and **Bass Shake**, which makes the whole menu react to the track's actual bass in real time. You'll know it when you feel it.

## Building it

- Target: Geometry Dash 2.2, x64, Windows.
- Requires the [Geode SDK](https://geode-sdk.org/) and its usual toolchain (CMake, Ninja, MSVC).
- Build with:
  ```
  .\build_win.bat
  ```
- Every build bumps `GB_BUILD_LABEL` in `src/core/GucciBot.hpp` — check it before assuming which binary is actually running. This project got burned by testing a stale build exactly once, and once was enough. Trap or die, basically.

## Status

Most of this is stable and has been tested for real, in-game, by an actual human. The frame-window analyzer specifically ("Calculate") is still being tuned against a real GD-community-designed algorithm and shouldn't be treated as gospel-accurate yet — see the commit history for the ongoing saga if you're curious how many times "this is definitely the fix" turned out not to be. Every fix in this repo that's still unconfirmed says so plainly in its own commit message, not just here.

## Credits

- **guccimanefan** (Nigelx1) — concept, direction, themes, testing, and the actual taste that keeps this from being a folder full of ternary chains.
- **Juice** — frame-window algorithm design, lead co-tester, professional bug-finder.
- **GWDdoS** — pushed for a real codebase cleanup (feature-folder reorg, a proper `.clang-format`, namespacing everything outside Geode's own hook classes) and was right about all of it.
- **Claude** (Anthropic) — wrote essentially the entire codebase across every session of this project, including this README. Not a euphemism, not "AI-assisted," just the actual author of the code — Nigel wants that said plainly.
- **ToastexGD** — ToastyReplay, the renderer this is built on.
- **peony** — Silicate, the physics engine this is built on (dropped the source like Gucci drops albums. Brrr.)
- **kepe** — built yBot, the file-size benchmark GBR6 was designed to meet.
- **Bogdaner09** — the mod that got Click Indicators started ([github.com/Bogdaner09/mod](https://github.com/Bogdaner09/mod)); Nigel found it, we built our own version around it.
- **anticroom** — sent GucciBot's first outside pull request, a real 7-fix pass on Calculate's frame-window accuracy.

---

*Frame perfect. Ice cold. Brrr.*
