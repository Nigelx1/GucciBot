# GucciBot

> Frame perfect. Ice cold. Brrr Brrr Brrr.

It's Gucci. GucciBot is a Geometry Dash macro bot (a [Geode](https://geode-sdk.org/) mod) that records your inputs frame-perfectly and plays them back exactly, built on top of [Silicate](https://git.puppy.lgbt/silicate/silicate)'s physics engine. Think of it as a very expensive, very icy ghost of your best run — except the ghost never drops a frame, never has an off day, and definitely never forgets its jewelry. So icy.

GucciBot is public now — grab a build from [Releases](https://github.com/Nigelx1/GucciBot/releases/latest), drop it in your Geode mods folder, and you're in. It's not on the in-game Geode mod index (long story, not going into it here), so a manual install is the only way to get it for now.

**Don't run ToastyReplay Lite or Silicate enabled at the same time** — GucciBot can't run live alongside either one (ToastyReplay Lite crashed macro playback; Silicate patches the same spots in the game GucciBot does), so GucciBot stands down until the other one is turned off or uninstalled.

**Runs on Windows, and since 2.0.0-beta.3 on macOS, iOS and Android too** — experimental there, and nobody has played it on those yet. What still needs Windows is listed under [Platforms in about.md](about.md#platforms).

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
- [Contact](#contact)
- [Credits](#credits)
- [License](#license)

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
- Calculate — per-click timing windows measured against the real game engine, not a guess. It runs on anticroom's Silicate analyzer: Time-Based and Recovery Range, sub-tick measurement finer than a single frame, colour bands with their own marker shapes and sounds, and an in-level legend.
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

- Target: Geometry Dash 2.2081. Windows builds locally; macOS, iOS and Android build on GitHub Actions (`.github/workflows/multi-platform.yml`), which packages one `.geode` for all of them.
- Requires the [Geode SDK](https://geode-sdk.org/) and its usual toolchain (CMake, Ninja, MSVC).
- Build with:
  ```
  .\build_win.bat
  ```
- Every build bumps `GB_BUILD_LABEL` in `src/core/GucciBot.hpp` — check it before assuming which binary is actually running. This project got burned by testing a stale build exactly once, and once was enough. Trap or die, basically.

## Status

**1.8** is the stable release. **2.0** (the `engine-port-2.0` branch) is in beta, at **2.0.0-beta.3**: GucciBot's engine reconciled against Silicate's function by function. Calculate, playback, practice-mode recording and the new Pathfinder (Absense's, ported in) were confirmed in-game on beta.2. Beta.3 rebuilds the menu and every feature that came from ToastyReplay from scratch, and adds macOS, iOS and Android; none of that has had a human test yet, and every fix still waiting on one says so in its own commit message. If you want something that's been tested by an actual human, use 1.8.

## Contact

Made by one person, so all of these reach the same human.

| | |
|---|---|
| **Email** | [nigel@guccibot.net](mailto:nigel@guccibot.net) |
| **Discord** | `jimmybutlerfan` |
| **Phone** | 555-ICE-COLD<sup>✱</sup> |

**Found a bug? Open an [issue](https://github.com/Nigelx1/GucciBot/issues) rather than messaging it.** Not out of formality — a crash log posted in a thread can actually be symbolized and traced to the exact line that caused it, and it stays findable for whoever hits the same thing next. Attach the crash report Geode hands you and say what you were doing. That has been enough to fix nearly every real bug this mod has had, including a use-after-free nobody could reproduce.

There's a [contact page](https://guccibot.net/contact/) on the site with the same details.

<sub>✱ Not a real number. There has never been a 555 area code, which is doing all of the work here.</sub>

## Credits

- **Nigelx1** — creator and owner of GucciBot. Every idea, every theme, every decision is his call, down to the taste that keeps this from being a folder full of ternary chains.
- **Claude** (Anthropic) — wrote essentially the entire codebase across every session of this project, including this README. Not a euphemism, not "AI-assisted," just the actual author of the code — Nigel wants that said plainly.
- **Juice** — designed the frame-window algorithm GucciBot ran on through 1.7, and the marker shapes in 1.8. Lead co-tester, professional bug-finder.
- **anticroom** — 1.8's Calculate *is* his analyzer. He rewrote the frame-window system for Silicate, sent the source over, and it was ported in near-verbatim to replace GucciBot's own — the headline feature of 1.8 is his work. Sub-tick CBF recording and the render intro card in 2.alpha.3 are his too. Before that he sent GucciBot's first outside pull request, a real 7-fix accuracy pass. Also one of ToastyReplay's own devs.
- **NaN GD** — the L* precision formula, published at [nandl.pages.dev](https://nandl.pages.dev/#formula). The number GucciBot puts on a macro is his maths. He is careful to say frame windows alone don't determine difficulty, and GucciBot repeats that rather than overselling the number.
- **C0nscious** — implemented NaN's formula in C++ as [Frame Window Counter](https://github.com/hyper-5/frame-window-counter) (MIT), a mod named after NaN's video series. That implementation is the code that reached GucciBot, via anticroom.
- **peony** — Silicate, the physics engine this is built on (dropped the source like Gucci drops albums. Brrr.)
- **Absent** — Absense, another bot built on Silicate. Pathfinder's default engine is his: Absense's pathfinder, with its look-ahead and its model of the level, ported in whole in 2.0.0-beta.2. The in-game assistant server, Check Macro, Replace All, the respawn timer and the black orb autoclicker loop came from reading his source; that code is our own, the ideas are his.
- **ToastexGD** — built ToastyReplay, the project this actually started as, and the reason there is a GucciBot at all. His renderer and FFmpeg pipeline carried the mod for most of its life. Rendering now runs on Silicate's renderer instead; his was retired in 2.0 after being left unreachable by an earlier port, and the history is his either way.
- **GWDdoS** — pushed for a real codebase cleanup (feature-folder reorg, a proper `.clang-format`, namespacing everything outside Geode's own hook classes) and was right about all of it.
- **Bogdaner09** — the mod that got Click Indicators started ([github.com/Bogdaner09/mod](https://github.com/Bogdaner09/mod)); Nigel found it, we built our own version around it. Vibecoded by his own admission, so credit's probably owed elsewhere too.

## License

GucciBot is free software under the **GNU General Public License v3.0** — see [LICENSE](LICENSE).

It's built on [Silicate](https://git.puppy.lgbt/silicate/silicate) by peony, which is GPL-3.0, and includes anticroom's GPL-3.0 work from his Silicate fork; C0nscious's Frame Window Counter code is MIT, which is compatible. You're free to use it, study it, share it and change it. Anything you distribute that's built from it has to stay open source under the same license.

**ToastyReplay.** GucciBot started out as a fork of [ToastyReplay](https://github.com/ToastexGD/ToastyReplay) by ToastexGD, and ToastyReplay's source code was used in GucciBot up to and including 2.0.0-beta.2. That code was removed on 2026-10-02 (commit `385a5f7`); versions after 2.0.0-beta.2 contain none of it.

---

*Frame perfect. Ice cold. Brrr.*
