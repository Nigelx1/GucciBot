# GucciBot 1.7

> Frame perfect. Ice cold. Brrr.

GucciBot is a Geometry Dash macro bot built on Silicate's physics engine, with a hypercompressed replay format and a full practice-trainer system -- one dedicated tab built around Jupiter My Favourite, and a second general-purpose one that works with any of your own saved macros.

## Requirements

- Geometry Dash 2.2081, Windows, Geode 5.7.1+
- **ToastyReplay Lite must be installed** (`toastexgd.toastyreplay-lite`) -- it doesn't need to be enabled, just present in your mods folder. GucciBot won't run without it.

---

## GBR6 Hypercompressed Format

- 2 bytes per input. Matches yBot file sizes for human gameplay.
- **Tap encoding** — wave/ship press+release pairs stored in 1 entry instead of 2. 50% smaller for those sections automatically.
- **Autoclicker delta compression** — an autoclicker macro of any length, including octillions of clicks, stores in 4 bytes. The bot stores the *description* of the pattern, not the output.
- Full backward compatibility — old `.brrr` files load automatically.
- Imports both JSON and binary (MessagePack) GDR macros.

## Engine

- Silicate's full physics engine — exact GD 2.2081 offsets, proper TPS bypass, SSB fix, lock delta, frame extrapolation
- Intentional deaths, backwards stepping, mirror inputs, maintain gravity, auto-flip, prevent death
- Complete player-state checkpoint capture
- Hitbox trail dedupes consecutive samples landing on the same on-screen pixel (camera-zoom aware), instead of drawing every sample

## Autoclicker

- Fully independent Hold Ticks / Release Ticks / Clicks Per Hold per player -- no more one shared setting forced onto both
- One-shot "Sync Player 2 to Player 1" copy, not a permanent link -- keep tweaking either side afterward
- Only While Holding — auto-clicks only while you actually hold the jump input

## Practice & Analysis

- Macro diff viewer — compare two replays frame by frame
- Frame-window analyzer ("Calculate") — per-click timing windows across real game frames, with three selectable algorithms: Time-Based (default), Recovery Range, and the new Alignment-Independent (also re-tests the previous click's own timing, not just this one). Optional "Circle Skin" marker style.
- TPS mid-macro changes, noclip accuracy display, macro trim/merge/surgery
- Bot settings presets, metadata editor, autosave at level end and/or on a timer

## Nigel's Jupiter My Favourite Trainer

A dedicated practice tab built entirely around one level, auto-loaded and ready the moment you open the mod:

- **Click Trainer** — a rhythm bar with a fixed line at the center; the macro's click/hold windows scroll through it at constant real-time speed. Pause, resume, reset, and skim by dragging the bar directly.
- **Your own clicks, live** — every press and release (click, spacebar, up arrow, W) shows up as its own white line alongside the macro's marks, for direct rhythm comparison. Optional looping with a fresh comparison each lap.
- **Click deviation readout** — how many frames early or late your last click was, compared to the macro.
- **Ghosts** — the macro's ghost, and your own best-attempt ghost, both rendered live in the game world. Scrub preview lets you freeze either at any point in the level to study it.
- **Segments** — mark and name the hard parts, with per-segment notes, auto-suggestions from click density, export/import codes, and segment looping (real auto-restart via a genuine practice checkpoint).
- **Stats** — attempts this session, session best %, and a death-position heatmap.
- **Synced music** — the level's actual track, seeked to your current frame, muting the game's own audio so it doesn't clash.

## Trainer (Any Macro)

The same toolset as the Jupiter My Favourite Trainer, right next to it as its own tab, but pointed at whichever of your own saved macros you pick instead of one fixed level:

- **Macro picker** — load any of your saved macros in, swap at will.
- **Click Trainer, Ghosts, Segments, Stats** — identical to the JMF tab above, just scoped to your pick.
- **Synced music** — import your own track for it, with an adjustable sync offset (also added to the JMF tab).
- Stats and ghosts activate automatically when you're on the level the macro was recorded on -- or on any level, with a clear heads-up, for macros that don't carry a recorded level name.

## Survival Indicator

A live green/red readout, right on screen while you play, for whether clicking *right now* survives what's coming:

- 4 styles — Ring, Classic, Converge, Pulse — with adjustable opacity, colour, and lookahead window.
- Flash-on-click feedback and a pitch-shifted click cue tied to how tight the margin is.
- Metronome-based calibration per gamemode, to line the cue up with your actual reaction lead/jitter.
- Accuracy/streak HUD (`Accuracy: N%  Streak: N (Best: N)`) tracked off the indicator's own safe/unsafe calls.

## Click Indicators & Video Mode

Real click-timing feedback and a synced video-review overlay, both built around the Jupiter My Favourite Trainer's click bar:

- **Real scoring** — every real press/release you make against the macro's own click bar is matched to its nearest unanswered click and scored Perfect / OK / Miss, with the last timing delta shown live.
- **Video Mode** — a full-screen review overlay (no level needs to be open) that plays your own footage back riding the exact same click-bar clock as the macro, for reviewing recorded runs against the timing data. The Jupiter My Favourite Trainer ships a real showcase video built in, working with zero setup.
- **Alignment Tool** — scrub the video directly to the frame of the first real click, then snap the sync offset to it in one press instead of nudging a slider by trial and error.

## Rendering

- Full gameplay capture to video via FFmpeg — configurable resolution, FPS, bitrate, codec (hardware encoders auto-detected per GPU vendor), and output extension.
- Audio captured straight from the game's own mix, with music and SFX volumes independently adjustable for the render.
- **Split audio tracks** — optional 4-track output (combined mix, plus music, SFX, and frame-window cues each isolated) instead of one merged track.
- Frame-window markers and cues render into the video too, matching your live tier setup.
- Render presets — save and reload full render configurations by name.

## Themes

| Theme | Extension | Colors |
|---|---|---|
| GucciBot | `.brrr` | Gold |
| ToosiiBot (LSU) | `.toosii` | Purple + Gold |
| ToosiiBot (Syracuse) | `.toosii` | Orange + Navy |
| ToosiiBot (Sac State) | `.toosii` | Dark Green + Gold |
| JaBot | `.ja` | Navy + Light Blue |
| GiddeyBot | `.giddey` | Thunder Blue + Orange |
| BamBot | `.bam` | Heat Black + Red |
| SexyyBot | `.sexyy` | Hot Pink + Purple |
| JuiceBot | `.juice` | Coral + Teal |
| ButlerBot | `.butler` | Bulls Red + Black |
| SaweetieBot | `.saweetie` | Hot Pink + Plum |
| MaybachBot | `.maybach` | Platinum + Black |
| RomoBot | `.romo` | Silver + Navy |
| GrizzleyBot | `.grizzley` | Flare Red + Steel Black |
| Red Kingdom | `.redkingdom` | Pulsing Red + Black |
| LemonadeBot | `.lemonade` | Lemonade Yellow + Black |
| BrrrBot | `.icebrrr` | Ice Blue + Navy, with a live snow overlay |
| WakaBot | `.waka` | Grove Green + Black |
| YoungstaBot | `.youngsta` | Black + Red (223) |
| KnockerzBot | `.knockerz` | Teal + Black |

**Make your own** — a full theme editor, not just a color picker: name, your own file extension, the complete color palette (accent, background, card, both text colors), corner radius and opacity, custom subtitle and brand tag, your own quotes on the Replay/Tools/Credits pages, and an optional Big Brrr track with its own BPM and drop offset. Saved and switchable right alongside the built-in themes.

## Extras

- **Compact Mode** — a small corner panel (record/play, save/name/Calculate, macro picker, TPS/speed, frame stepping) instead of the full tabbed window, so the bot stays usable while you're actually playing.
- **BIG BRRRR** — a joke toggle in Settings. You'll know it when you see it.

---

## Credits

- **Nigelx1** — creator and owner of GucciBot; every idea, every theme, every decision is his call
- **Claude** — wrote the code and this page. Essentially the whole codebase, not a euphemism.
- **Juice** — frame-window algorithm design, lead co-tester, ran the mod into the ground on purpose finding the bugs nobody else caught
- **anticroom** — frame-window accuracy fixes, GucciBot's first outside pull request (also one of ToastyReplay's own devs)
- **peony** — Silicate (dropped the source like Gucci drops albums. Brrr.)
- **ToastexGD** — built ToastyReplay, the project GucciBot actually started as before the Silicate migration; the renderer, the FFmpeg pipeline, and the whole recording system running today are still his, exactly as built
- **GWDdoS** — Astral, and the codebase cleanup that got this repo public-ready
- **Bogdaner09** — Click Indicators inspiration ([github.com/Bogdaner09/mod](https://github.com/Bogdaner09/mod)) — vibecoded by his own admission, so credit's probably owed to whichever model wrote that too
- **Gucci Mane** — he's the truth. Brrr.
- **anticroom** — frame-window accuracy fixes, GucciBot's first outside pull request
