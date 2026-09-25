# GucciBot 2.alpha.2

> Frame perfect. Ice cold. Brrr.

GucciBot is a Geometry Dash macro bot built on Silicate's physics engine, with a hypercompressed replay format and a full practice-trainer system -- one dedicated tab built around Jupiter My Favourite, and a second general-purpose one that works with any of your own saved macros.

**Website:** [guccibot.net](https://guccibot.net)

## Requirements

- Geometry Dash 2.2081, Windows, Geode 5.10.1+
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
- **Deterministic levels.** A macro now replays against the level it was recorded on rather than a slightly different one each attempt: object variance, Random triggers, and the teleport and shake RNG are all derived from the macro's own seed, and checkpoints capture and restore every one of them. Levels built around Random triggers are replayable at all for the first time.
- **High TPS Precision** (optional) — GD rounds vertical velocity to a fixed step no matter the tick rate, so running above the rate a macro was recorded at throws away the precision those extra ticks buy. This scales the step with the rate. Off by default, because it changes physics.
- Lock Delta has Performance and Accuracy modes again. Accuracy is the default and gives GD one physics step per update; Performance hands it several at once and lets it sub-step, which is what Silicate does.
- **Deterministic levels.** A macro replays against the level it was recorded on, not a slightly different one each time: object variance, Random triggers, teleport and shake RNG are all derived from the macro's own seed, and checkpoints capture and restore every one of them. Levels built on Random triggers are replayable at all for the first time.
- **High TPS Precision** (optional) — GD rounds vertical velocity to a fixed step regardless of tick rate, so running above the rate a macro was recorded at throws away the precision the extra ticks buy. This scales the step with the rate. Off by default; it changes physics.
- Lock Delta has Performance and Accuracy modes again. Accuracy is the default and drives one physics step per update; Performance hands GD several at once and lets it sub-step, which is what Silicate does.
- Hitbox trail dedupes consecutive samples landing on the same on-screen pixel (camera-zoom aware), instead of drawing every sample

## Autoclicker

- Fully independent Hold Ticks / Release Ticks / Clicks Per Hold per player -- no more one shared setting forced onto both
- One-shot "Sync Player 2 to Player 1" copy, not a permanent link -- keep tweaking either side afterward
- Only While Holding — auto-clicks only while you actually hold the jump input

## Practice & Analysis

- Macro diff viewer — compare two replays frame by frame
- Frame-window analyzer ("Calculate") — per-click timing windows measured against the real game engine, rebuilt in 1.8 on anticroom's Silicate analyzer. Two algorithms (Time-Based and Recovery Range), sub-tick CBF measurement that reads windows finer than a single frame, and a full settings tab: how far a shifted input has to survive, how much room to leave before the next one, which inputs to measure, and how much of each frame to spend. Colour bands with per-band marker shapes (circle, star, spiral, polygon, as a single outline, an inner ring or filled), per-band sounds and importable sound packs, and an in-level legend counting how many clicks landed in each band. Results save alongside the macro.
- TPS mid-macro changes, noclip accuracy display, macro trim/merge/surgery
- Bot settings presets, metadata editor, autosave at level end and/or on a timer

## Pathfinder

Makes a macro for you. Open a level with nothing recorded, start Pathfinder, and it plays the level itself until it has a run that reaches the end -- then saves that run as a macro named after the level.

- **Searches where it matters** — it checks, frame by frame, whether pressing would change anything at all. When a death comes long after the mistake behind it (walking off a ledge and dying at the bottom), it looks back past the stretch where no input could have helped instead of burning attempts inside it.
- **Tries the earlier press again first** — when a fix only buys a few frames, it tries different versions of the press before it rather than settling.
- **Real backtracking** — every option it tries at a decision point is ruled out there for good; dead ends restore a real checkpoint further back.
- **Never retries a proven dead end** — a press that has already been shown not to work is skipped outright the next time the search comes back through.
- **Remembers hard spots** — what got past a hazard is saved per level, keyed on the situation you died in rather than on the exact run that led there, and tried first the next time you search that level.
- **Prefers a press you could actually hit** — given two options, it takes the one that also works a frame late over the one that only works frame-perfect.
- **Tries your own presses first** — if there is a recorded macro on the level, its inputs are candidates before anything generated.
- **Widens where it keeps getting bitten** — a spot that has failed several times gets a longer look straight away instead of creeping outward one frame at a time.
- **Proves its own answer** — a solution only counts once it plays from the very start of the level on its own.
- **Agency Map** — an optional overlay showing, as you play, which frames an input could actually change.
- Strongest on Cube-style sections so far. Modes where letting go is its own decision -- Ship, Wave, Robot, Swing -- are the next part of the work.

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
- Audio captured straight from the game's own mix, with music, the game's own sound effects, and the level's SFX-trigger audio each at their own volume for the render — so a showcase can keep the sound a creator built into the level while dropping the death and orb noise the run makes.
- **Split audio tracks** — optional 4-track output (combined mix, plus music, SFX, and frame-window cues each isolated) instead of one merged track.
- Frame-window markers and cues render into the video too, matching your live tier setup.
- Render presets — save and reload full render configurations by name, plus a one-press showcase preset (8K60, lossless x264, FLAC, thread count read from your CPU).
- **Fade in and out** — the picture fades up at the start and down at the end, with the audio on the same curve, so a render doesn't cut hard from black or hard to silence. 1.5 seconds each by default; either end can be set to zero.
- **Asynchronous frame capture** — readbacks are pipelined through a ring of GPU buffers with fences, instead of stalling the game every single frame waiting for one to copy out.
- A render holds the view for its whole duration, so a resize, a DPI change or an alt-tab partway through no longer fights the output resolution.
- **Fade in and out** — the picture fades up at the start and down at the end, with the audio on the same curve, so a render doesn't cut hard from black or to silence. 1.5s each by default, either end can be set to zero.
- **Asynchronous frame capture** — readbacks are pipelined through a ring of GPU buffers with fences instead of stalling the game every frame waiting for each one to copy out.
- Rendering holds the view for the whole render, so a resize, a DPI change or an alt-tab mid-render no longer fights the output resolution.

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

- **Frame Editor** — a timeline view of a macro's inputs. Every press is a bar you can drag, stretch or shorten, with undo and redo, an overview strip for long macros, and separate lanes for player 2.
- **Assistant Access** — an optional local server (Settings) that lets an AI assistant read what the bot is doing: the frame, the loaded macro, Calculate's windows, Pathfinder's progress and GucciBot's own logs, and step the game while something is going wrong. Off by default, your machine only, and it never reaches the network.
- **Compact Mode** — a small corner panel (record/play, save/name/Calculate, macro picker, TPS/speed, frame stepping) instead of the full tabbed window, so the bot stays usable while you're actually playing.
- **BIG BRRRR** — a joke toggle in Settings. You'll know it when you see it.

---

## Credits

- **Nigelx1** — creator and owner of GucciBot; every idea, every theme, every decision is his call
- **Claude** — wrote the code and this page. Essentially the whole codebase, not a euphemism.
- **Juice** — designed the frame-window algorithm GucciBot ran on through 1.7 and the marker shapes in 1.8; lead co-tester, ran the mod into the ground on purpose finding the bugs nobody else caught
- **anticroom** — 1.8's Calculate *is* his analyzer: his Silicate frame-window rewrite, ported in near-verbatim and now the whole feature. Before that, GucciBot's first outside pull request — a real 7-fix accuracy pass. Also one of ToastyReplay's own devs.
- **NaN GD** — the L* precision formula, published at [nandl.pages.dev](https://nandl.pages.dev/#formula). The number GucciBot puts on a macro is his maths
- **C0nscious** — implemented NaN's formula in C++ as [Frame Window Counter](https://github.com/hyper-5/frame-window-counter) (MIT), which is the code that reached GucciBot
- **peony** — Silicate (dropped the source like Gucci drops albums. Brrr.)
- **Absent** — Absense, another bot built on Silicate. Several of Pathfinder's 2.alpha.2 improvements and the assistant server are his ideas, worked out again here from his source
- **ToastexGD** — built ToastyReplay, the project GucciBot actually started as and the reason there is a GucciBot at all. His renderer and FFmpeg pipeline carried this mod for most of its life; rendering now runs on Silicate's
- **GWDdoS** — Astral, and the codebase cleanup that got this repo public-ready
- **Bogdaner09** — Click Indicators inspiration ([github.com/Bogdaner09/mod](https://github.com/Bogdaner09/mod)) — vibecoded by his own admission, so credit's probably owed to whichever model wrote that too
- **Gucci Mane** — he's the truth. Brrr.
