# GucciBot 2.0.0-beta.2

> Frame perfect. Ice cold. Brrr.

GucciBot is a Geometry Dash macro bot built on Silicate's physics engine, with a hypercompressed replay format and a full practice-trainer system -- one dedicated tab built around Jupiter My Favourite, and a second general-purpose one that works with any of your own saved macros.

**Website:** [guccibot.net](https://guccibot.net)

## Requirements

- Geometry Dash 2.2081, Windows, Geode 5.10.1+
- Don't run **ToastyReplay Lite** enabled at the same time -- the two can't both run live, so GucciBot stands down until it's turned off or uninstalled.

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
- **Practice checkpoints, captured whole** — placed between physics steps and captured in that same instant, as Silicate does: players, level, RNG and all. A macro recorded through respawns plays back exactly like one recorded straight through.
- **Deterministic levels.** A macro now replays against the level it was recorded on rather than a slightly different one each attempt: object variance, Random triggers, and the teleport and shake RNG are all derived from the macro's own seed, and checkpoints capture and restore every one of them. Levels built around Random triggers are replayable at all for the first time.
- **High TPS Precision** (optional) — GD rounds vertical velocity to a fixed step no matter the tick rate, so running above the rate a macro was recorded at throws away the precision those extra ticks buy. This scales the step with the rate. Off by default, because it changes physics.
- Lock Delta has Performance and Accuracy modes again. Accuracy is the default and gives GD one physics step per update; Performance hands it several at once and lets it sub-step, which is what Silicate does.
- **Sub-tick clicks (CBF Recording)** — records each click at the point inside the tick you actually pressed it, from the time Windows stamped on the input, and plays it back there. Saved in the macro; older versions still open it, with the clicks on the tick. **Sub-tick Frame Advance** steps through a tick in splits while frame advancing, showing where your hitbox would be and the hold and release paths from that point.
- **Click Between Frames friendly** — Syzzi's Click Between Frames and Superb Input Precision apply clicks outside the game's normal input path, where a bot records them, so GucciBot pauses them while it's recording or playing and puts them back exactly as they were the moment it's idle.
- **Frame Pacing** — Real Time, a fixed number of ticks per drawn frame, or a dynamic limit that measures how long a tick takes and aims for a target frame rate.
- **RNG Lock** — record with a fixed seed of your choosing.
- **Prevent Death** can look ahead with the trajectory and stop before a death instead of stepping back after it. **Find Best Tick** steps forward trying your next click on every tick and stops on the one that survives longest.
- Hitbox trail dedupes consecutive samples landing on the same on-screen pixel (camera-zoom aware), instead of drawing every sample

## Autoclicker

- Fully independent Hold Ticks / Release Ticks / Clicks Per Hold per player -- no more one shared setting forced onto both
- One-shot "Sync Player 2 to Player 1" copy, not a permanent link -- keep tweaking either side afterward
- Only While Holding — auto-clicks only while you actually hold the jump input
- **Swift Clicks** — each click released on the same tick it was pressed
- **Auto Black Orb UFO** — a fixed five-tick tap-and-hold loop recorded from a black orb UFO spam at 720 TPS

## Practice & Analysis

- Macro diff viewer — compare two replays frame by frame
- Frame-window analyzer ("Calculate") — per-click timing windows measured against the real game engine, rebuilt in 1.8 on anticroom's Silicate analyzer. Two algorithms (Time-Based and Recovery Range), sub-tick CBF measurement that reads windows finer than a single frame, and a full settings tab: how far a shifted input has to survive, how much room to leave before the next one, which inputs to measure, and how much of each frame to spend. Colour bands with per-band marker shapes (circle, star, spiral, polygon, as a single outline, an inner ring or filled), per-band sounds (the Bells by default) and importable sound packs, and an in-level legend counting how many clicks landed in each band. Results save alongside the macro. **Dependent Pair Search** re-measures an input with the one before it moved across its own window; **Test** re-runs the last few inputs while you record; and any input under the playhead can be labelled by hand.
- **Check Macro** — lists anything in a macro that would desync playback: a press while already held, a release with nothing held, actions out of order, bad values
- TPS mid-macro changes, noclip accuracy display, macro trim/merge/surgery
- Bot settings presets, metadata editor, autosave at level end and/or on a timer

## Pathfinder

Makes a macro for you. Open a level with nothing recorded, start Pathfinder, and it plays the level itself until it reaches the end -- then saves that run as a macro named after the level.

Two engines, picked at the top of the Pathfinder tab.

**Absense (the default)** — Absent's pathfinder from Absense, ported in whole.

- **Looks before it clicks** — before each input it runs copies of the player ahead through the real level, with Silicate's physics for portals, orbs, pads and dashes and a model of the level's triggers, and plays the idea that lasts.
- **Plays where you can see it** — the level runs in real time while it plans the next stretch, with its progress in the corner.
- **Goes back when it has to** — a dead end sends it back through Backwards Stepping's stored frames to try another way, and further back each time a spot keeps biting.
- **Learns levels** — what got it past a spot is remembered per level and tried first the next time, so searching a level again is faster.
- **Holds a height** — GucciBot's addition: in ship, wave, UFO and swing it also tries holding the player at a spread of heights, the way you'd thread a ship through a gap.
- **Starts how you like** — from wherever the level is, straight from the pause menu, or with **Start from the beginning** on, from frame 0 after a Full Restart.

**Classic** — GucciBot's own search, still there.

- **Death is the signal** — it runs until GD's own collision kills it, then looks back from there for the press that gets further, skipping stretches where no input could have mattered.
- **Real backtracking** — every option it tries at a decision point is ruled out there for good; dead ends restore a real checkpoint further back.
- **Proves its own answer** — a solution only counts once it plays from the very start of the level on its own.
- **Agency Map** — an optional overlay showing, as you play, which frames an input could actually change.
- Strongest on Cube-style sections.

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
- **Intro card** — an optional title card before the level: its name, your own lines, and your frame-window bands in their colours, fading in and out over black.
- **Hear it while it renders** — optionally plays the render's audio through your speakers as it records.
- **Render the next level I open**, and **leave the level when it finishes** — queue a render from the menu and walk away.

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
- **HUD** — live readouts in the corner of the level: frame, TPS, position, velocities, rotation, bot state, plus debug readouts (game tick, speed, gravity, checkpoints, level time, time warp, random states, action index, touching orbs and more).
- **Editor tools** — Macro Buffing (lays a macro's path into the level as objects) and Replace All (swap every object of one id for another, undoable).
- **Compact Mode** — a small corner panel (record/play, save/name/Calculate, macro picker, TPS/speed, frame stepping) instead of the full tabbed window, so the bot stays usable while you're actually playing.
- **BIG BRRRR** — a joke toggle in Settings. You'll know it when you see it.

---

## Credits

- **Nigelx1** — creator and owner of GucciBot; every idea, every theme, every decision is his call
- **Claude** — wrote the code and this page. Essentially the whole codebase, not a euphemism.
- **Juice** — designed the frame-window algorithm GucciBot ran on through 1.7 and the marker shapes in 1.8; lead co-tester, ran the mod into the ground on purpose finding the bugs nobody else caught
- **anticroom** — 1.8's Calculate *is* his analyzer: his Silicate frame-window rewrite, ported in near-verbatim and now the whole feature. Sub-tick CBF recording and the render intro card are his work too. Before that, GucciBot's first outside pull request — a real 7-fix accuracy pass. Also one of ToastyReplay's own devs.
- **NaN GD** — the L* precision formula, published at [nandl.pages.dev](https://nandl.pages.dev/#formula). The number GucciBot puts on a macro is his maths
- **C0nscious** — implemented NaN's formula in C++ as [Frame Window Counter](https://github.com/hyper-5/frame-window-counter) (MIT), which is the code that reached GucciBot
- **peony** — Silicate (dropped the source like Gucci drops albums. Brrr.)
- **Absent** — Absense, another bot built on Silicate. Pathfinder's default engine is his: Absense's pathfinder, with its look-ahead and its model of the level, ported in whole. The assistant server, Check Macro, Replace All, the respawn timer and the black orb autoclicker loop are his ideas too, worked out again here from his source
- **ToastexGD** — built ToastyReplay, the project GucciBot actually started as and the reason there is a GucciBot at all. His renderer and FFmpeg pipeline carried this mod for most of its life; rendering now runs on Silicate's
- **GWDdoS** — Astral, and the codebase cleanup that got this repo public-ready
- **Bogdaner09** — Click Indicators inspiration ([github.com/Bogdaner09/mod](https://github.com/Bogdaner09/mod)) — vibecoded by his own admission, so credit's probably owed to whichever model wrote that too
- **Gucci Mane** — he's the truth. Brrr.

GucciBot is free software under the GNU General Public License v3.0, like Silicate, which it's built on.
