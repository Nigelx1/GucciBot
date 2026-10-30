# GucciBot 2.0.0-beta.5

> Frame perfect. Ice cold. Brrr.

GucciBot is a Geometry Dash macro bot built on Silicate's physics engine, with a hypercompressed replay format and a full practice-trainer system -- one dedicated tab built around Jupiter My Favourite, and a second general-purpose one that works with any of your own saved macros.

**Website:** [guccibot.net](https://guccibot.net)

## Requirements

- Geometry Dash 2.2081 on Windows, macOS, iOS or Android, Geode 5.10.1+ (macOS, iOS and Android are experimental: see Platforms)
- Don't run **ToastyReplay Lite** or **Silicate** enabled at the same time -- GucciBot can't run live alongside either, so it stands down until the other one is turned off or uninstalled.

## Platforms

Windows is GucciBot's home, and where all of it has been tested. Since 2.0.0-beta.3 it also runs on **macOS, iOS and Android**. It's still experimental there: it has been played on an Android phone, but the rest has had little testing, so if something looks wrong, open an issue. On a phone the menu opens from the **GucciBot** button in the pause menu, fills the screen and scrolls when you drag it; **Menu size** in Settings makes it bigger or smaller. On a Mac the menu key's default is **Option**, and the pause-menu button is there too. On iOS no key reaches GucciBot, so Settings > Keybinds says so instead of offering keys.

What still needs Windows:

- **Pathfinder** (both engines).
- **Rendering** and **Video Mode**, which load FFmpeg from Windows libraries.
- Tick rates under 162 TPS, and **High TPS Precision**.
- The trigger model behind the look-aheads. Elsewhere, path preview, Frame Extrapolation, Prevent Death's look-ahead, Find Best Tick and the Survival Indicator carry moving objects at the speed they have, without the triggers that change them later.
- Seeded random teleports and screen shake, so a macro through a random teleport may not replay off Windows.

---

## GBR6 Hypercompressed Format

- 2 bytes per input. Matches yBot file sizes for human gameplay.
- **Tap encoding** — wave/ship press+release pairs stored in 1 entry instead of 2. 50% smaller for those sections automatically.
- **Autoclicker delta compression** — a steady run of jump clicks (8 or more, each held for the same 1 to 15 ticks and released for the same 1 to 15) is stored as its pattern and a count: 4 bytes for every 65,535 clicks, where each click would otherwise take 2. The bot stores the *description* of the pattern, not the output. Swift clicks, more than one click per hold and the black orb UFO loop are stored click by click.
- Old GucciBot 1.x macros (the BRR format from before GBR6) convert in the game, with the other bots' macros below, and a 1.x macro picked as a Trainer macro loads through the same converter.

## Other Bots' Macros

- **Convert in one click** — drop another bot's macro in the replays folder and **Saved macros** lists it under *From other bots* with its format and a Convert button (or Convert all). **Import a macro** takes one from anywhere instead, a GucciBot macro too. The original stays where it was, and a copy dated the day it was first imported is kept in `replays/imported`, where the picker opens on Windows and macOS; importing the same file again selects the macro it became instead of making a second one.
- **27 formats read**: GDR (Mega Hack, Macrobot, xdBot and more; binary and JSON), GDR2, Silicate (.slc v1 to v3, and anticroom's .scbf1 with its sub-tick clicks), TcBot (.tcm, practice sessions flattened to one run), ToastyReplay (.ttr3), yBot, xdBot, Mega Hack Replay, Echo, OmegaBot 2, ReplayBot, zBot, KD-Bot, Rush, Fembot, xBot, URL, TASBot, plain text, and GucciBot 1.x's old .brrr.
- **Export as** any of 21 of them from a macro's "..." menu, into `replays/exports`, with a note on anything the format can't hold.
- Silicate counts frames one behind GucciBot, so its macros are lined up for you. If another bot's macro dies early after converting, **Shift frames** nudges it a frame or two either way.

## Engine

- Silicate's full physics engine — exact GD 2.2081 offsets, proper TPS bypass, SSB fix, lock delta, frame extrapolation
- Intentional deaths, backwards stepping, mirror inputs, maintain gravity, auto-flip, prevent death
- **Practice checkpoints, captured whole** — placed between physics steps and captured in that same instant, as Silicate does: players, level, RNG and all. A macro recorded through respawns plays back exactly like one recorded straight through.
- **Deterministic levels.** A macro now replays against the level it was recorded on rather than a slightly different one each attempt: object variance, Random triggers, and the teleport and shake RNG are all derived from the macro's own seed, and checkpoints capture and restore every one of them. Levels built around Random triggers are replayable at all for the first time.
- **High TPS Precision** (optional) — GD rounds vertical velocity to a fixed step no matter the tick rate, so running above the rate a macro was recorded at throws away the precision those extra ticks buy. This scales the step with the rate. Off by default, because it changes physics.
- **Lock Delta** (Hacks > Engine, on by default) — GD gets exactly one physics step per tick.
- **Game speed** — typed, anything from 0.01x to 20x (Macro page). **Audio pitch follows speed** (Hacks > Engine, on by default) plays the music and sounds faster or slower with it; off, they keep their normal pitch.
- **Block inputs during playback** (Macro > Recording and playback, on by default, as in Silicate) — while a macro plays, only the macro presses buttons. Off, your own clicks reach the game too.
- **Sub-tick clicks (CBF recording)** (Macro > Recording and playback) — records each click at the point inside the tick you actually pressed it, from the time Windows stamped on the input, and plays it back there. Saved in the macro; older versions still open it, with the clicks on the tick. While you record, the card counts this attempt's inputs: how many were placed inside the tick and how many stayed on its edge. It records on whole ticks while Input FPS is set below the tick rate or Maintain gravity is on, and the card says which. **Sub-tick preview** (Hacks > Prediction, Step through the tick) steps through a tick in splits while frame advancing, showing where your hitbox would be and the hold and release paths from that point.
- **Click Between Frames friendly** — Syzzi's Click Between Frames and Superb Input Precision apply clicks outside the game's normal input path, where a bot records them, so GucciBot pauses them while it's recording or playing and puts them back exactly as they were the moment it's idle.
- **Frame Pacing** (Hacks > Frame pacing) — Real Time, a fixed number of ticks per drawn frame, or a dynamic limit that measures how long a tick takes and aims for a target frame rate. Fixed can **Scale with the tick rate** (Silicate's Use Visual Updates, off by default): the limit is multiplied by the tick rate over GD's FPS cap, so a limit of 10 at 240 TPS and 60 FPS becomes 40, and 1000 TPS keeps full speed.
- **RNG Lock** — record with a fixed seed of your choosing.
- **Prevent Death** (Hacks > Engine) pauses the game on the tick you die, and with Backwards stepping on (Macro > Frame stepper) steps back to the tick before, so you can go again from there. **Stop before a death** (Hacks > Prediction) looks four ticks ahead and pauses just before the hit instead. **Find Best Tick** steps forward trying your next click on every tick and stops on the one that survives longest.
- Hitbox trail dedupes consecutive samples landing on the same on-screen pixel (camera-zoom aware), instead of drawing every sample

## Autoclicker

- Fully independent Hold Ticks / Release Ticks / Clicks Per Hold per player -- no more one shared setting forced onto both
- One-shot **Copy to Player 2** (the *Same as Player 1* row), not a permanent link -- keep tweaking either side afterward
- **Only while holding** — auto-clicks only while you actually hold the jump input yourself: a key, a click or a touch, for player 1 or player 2
- **Swift Clicks** — each click released on the same tick it was pressed
- **Black orb UFO loop** — a fixed five-tick tap-and-hold loop recorded from a black orb UFO spam at 720 TPS

## Practice & Analysis

- Macro diff viewer — compare two replays frame by frame
- Frame-window analyzer ("Calculate") — per-click timing windows measured against the real game engine, rebuilt in 1.8 on anticroom's Silicate analyzer. Two algorithms (Time-Based and Recovery Range), sub-tick CBF measurement that reads windows finer than a single frame, and a full settings tab: how far a shifted input has to survive, how much room to leave before the next one, which inputs to measure, and how much of each frame to spend. Color bands with per-band marker shapes (circle, star, spiral, polygon, as a single outline, an inner ring or filled) or your own image in place of the shape, a pulse on the marker or its label with your own fade-in, hold and fade-out, per-band sounds (the Bells by default, or every band to the Brrr or the Bells in one press), and an in-level legend counting how many clicks landed in each band. **Default Look** draws it all like the counter in NaN GD's videos instead: a plain ring per input in his colors, the number on its left, his 9-10 / 7-8 / 5-6 / 4 / 3 / 2 / 1 legend, and the Brrr or his bells; your own bands stay as they are. Change the look mid-level and the markers of the last few seconds are drawn again in it. **Text in the level** (Calculate > Overlay) puts your own words above any measured input, with GD's color tags, a size and a height, saved with the macro's windows. Results save alongside the macro. **Presets** — Balanced (the strict count, the default), Accuracy (Recovery Range at 2) and Absolute (Accuracy, with each test run past the next input) — set how it counts in one press. **Max window** stops counting an input at N frames and marks it N+. **Copy info** puts the precision and window breakdown on the clipboard in the format NaN GD uses in his video descriptions. It can mute the game while it runs, and **Leg trail** (Advanced) draws each test in the level as it plays. **Dependent Pair Search** re-measures an input with the one before it moved across its own window; **Test** re-runs the last few inputs while you record; and any input under the playhead can be labeled by hand. The whole frame-window trend started with NaN GD, and the L* precision number GucciBot puts on a macro comes from his formula, published at [nandl.pages.dev](https://nandl.pages.dev/#formula). He's careful to say frame windows alone don't determine difficulty, and GucciBot says the same.
- **Suggestions for suspicious clicks** — Calculate counts every click strictly. A tight one right after another input is also counted with Juice's Alignment-Independent method, and when that comes out wider, Windows by hand shows it next to Calculate's count ("my suggestion is x frames", one press to use it) and the Frame Editor flags the click.
- **Manual Frame Windows** (Calculate > Windows by hand) — every input of the macro in one table: type any window yourself, keep Calculate's, or take its suggestion. A window you type stays through the next Calculate run, and Clear goes back to Calculate's own count.
- **Time cap** for Calculate (Calculate > Advanced; off until you set one) — a run that goes on longer stops and keeps what it measured. The in-level box shows how long it has run and roughly how long is left, and a notification says when it's done.
- **Check Macro** — lists anything in a macro that would desync playback: a press while already held, a release with nothing held, actions out of order, bad values
- TPS mid-macro changes, noclip accuracy display, macro trim/merge/surgery
- Bot settings presets, metadata editor, autosave at level end (on by default) and/or on a timer (a macro with no name waits until it has one), and a backup of a macro before a save overwrites it

## Pathfinder

Makes a macro for you. Open a level with nothing recorded, start Pathfinder, and it plays the level itself until it reaches the end -- then saves that run as a macro named after the level.

Two engines, picked at the top of the Pathfinder tab.

**Absense (the default)** — Absent's pathfinder from Absense, ported in whole.

- **Looks before it clicks** — before each input it runs copies of the player ahead through the real level, with Silicate's physics for portals, orbs, pads and dashes and a model of the level's triggers, and plays the idea that lasts.
- **Plays where you can see it** — the level runs in real time while it plans the next stretch, with its progress in the corner.
- **Goes back when it has to** — a dead end sends it back through Backwards Stepping's stored frames to try another way, and further back each time a spot keeps biting.
- **Learns levels** — what got it past a spot is remembered per level and tried first the next time, so searching a level again is faster.
- **Maps the whole level** — **Show the level map** draws the gaps the player fits through, the choices (forks, orbs, pads, portals) and the parts that move; a copy of the player rides the level in the background to time what moves, without holding the game up.
- **Plans a route through it** — **Show the planned route** draws the way through the whole level it would take. **Follow the planned route (AI)** set to **Route ideas** has each decision first try the way GD's physics finds to the route's next gate, and a dead end jump back to where it left the route. Set to **Route drives** (experimental), every decision aims at the next gate's best choice from where the player is (use or skip an orb, touch or avoid a pad or portal) and plays the way physics finds through it, only as far as that gate.
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
- **Ghosts** — the macro's ghost and your own best-attempt ghost, drawn live in the level (in Jupiter My Favourite only). On this tab the macro's ghost needs one bot run first: the built-in macro is inputs only, so load a JMF macro on the Macro page, play it to the end with the bot once, then press **Use it** under Ghosts. Scrub freezes both at one point of the level to study it, and Reset stats clears your best attempt.
- **Segments** — mark and name the hard parts, with per-segment notes, auto-suggestions from click density, export/import codes, and segment looping (real auto-restart via a genuine practice checkpoint).
- **Stats** — attempts this session, session best %, and a death-position heatmap.
- **Synced music** — the level's actual track, seeked to your current frame, muting the game's own audio so it doesn't clash.

## Trainer (Any Macro)

The same toolset as the Jupiter My Favourite Trainer, right next to it as its own tab, but pointed at whichever of your own saved macros you pick instead of one fixed level:

- **Macro picker** — load any of your saved macros in, swap at will.
- **Click Trainer, Ghosts, Segments, Stats** — identical to the JMF tab above, just scoped to your pick.
- **Synced music** — import your own track for it, with an adjustable sync offset (also added to the JMF tab).
- Stats and ghosts activate automatically when you're on the level the macro was recorded on -- or on any level, with a clear heads-up, for macros that don't carry a recorded level name. Picking another macro starts the best-attempt ghost over.

## Survival Indicator

A live green/red readout, right on screen while you play, for whether clicking *right now* survives what's coming:

- 4 styles — Ring, Classic, Converge, Pulse — with adjustable opacity, color, and lookahead window.
- Flash-on-click feedback and a pitch-shifted click cue tied to how tight the margin is.
- Metronome-based calibration per gamemode (the swing has its own), to line the cue up with your actual reaction lead/jitter.
- Accuracy/streak HUD (`Accuracy: N%  Streak: N (Best: N)`) tracked off the indicator's own safe/unsafe calls; it shows with the indicator on, and not while a macro plays.
- Safe mode counts it as a cheat: it shows the way through.

## Click Sounds

- A click when jump is pressed and a release sound when it's let go, from the built-in Clickbot pack or your own (Click sounds page; off until you turn it on).
- Soft sounds for quick re-presses and releases, volume up to 200%, a looping background-noise track, and a separate pack for player 2 in two-player levels.
- It clicks for a macro's inputs too, and optionally while rendering, so the clicks are in the video.
- Your own packs: a folder holding clicks, releases, softClicks and softReleases folders (WAV, MP3, OGG, FLAC or AIFF) in the clickpacks folder. Packs made for other bots usually work as they are.

## Click Indicators & Video Mode

Real click-timing feedback, and a synced video-review overlay on both trainers:

- **Click Indicators** — the macro's clicks coming at you over the level, built around Bogdaner09's free and open-source mod with every one of its options: badges, the player square and line, draining holds, taps and holds, player 2 colors, a timing offset and the rhythm lane. It shows a trainer's macro in that trainer's level, or the loaded macro on its own level, and scores you Perfect / OK / Miss. Safe mode counts it as a cheat: it takes the memory out of memory levels.
- **Video Mode** — a full-screen review overlay (no level needs to be open) riding the click bar's clock, for reviewing a run against the timing data. The Jupiter My Favourite tab plays the built-in showcase video with zero setup; the Trainer tab plays any video you pick. While it plays, its Opacity, Sync offset, Nudge and the Alignment Tool sit in a small **Video settings** window over the top of the video, folded until you open it.
- **Alignment Tool** — scrub the video directly to the frame of the first real click, then snap the sync offset to it in one press instead of nudging a slider by trial and error.

## Rendering

- Full gameplay capture to video via FFmpeg — configurable resolution, FPS, bitrate, codec (left blank, it picks a hardware encoder for your graphics card, and falls back to libx264 if that one won't open), and output extension.
- Audio captured straight from the game's own mix, with music, the game's own sound effects, and the level's SFX-trigger audio each at their own volume for the render — so a showcase can keep the sound a creator built into the level while dropping the death and orb noise the run makes.
- **Separate audio tracks** — optional 4-track output (combined mix, plus music, SFX, and frame-window cues each isolated) instead of one merged track.
- Frame-window markers and cues render into the video too, matching your live tier setup.
- **Fade in and out** — the picture fades up at the start and down at the end, with the audio on the same curve, so a render doesn't cut hard from black or hard to silence. 1.5 seconds each by default; either end can be set to zero.
- **Asynchronous frame capture** — readbacks are pipelined through a ring of GPU buffers with fences, instead of stalling the game every single frame waiting for one to copy out.
- A render holds the view for its whole duration, so a resize, a DPI change or an alt-tab partway through no longer fights the output resolution.
- **Intro card** — an optional title card before the level, inspired by NaN GD: its name, your own lines, and your frame-window bands in their colors, fading in and out over black. Every line is yours to edit (Render > Intro card; a blank field hides that line), and a long level name shrinks so it clears the legend.
- **Hear it while it renders** — optionally plays the render's audio through your speakers as it records.
- **Render the next level I open**, and **leave the level when it finishes** — queue a render from the menu and walk away.
- **Presets** — save your render settings by name; **Showcase** sets up an 8K60 lossless render in one press. A popup says when a render is done, the pixel format is yours to pick, and if FFmpeg is missing the Render page says so with a button to its folder.

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
| BrrrBot | `.icebrrr` | Ice Blue + Navy, with live snow falling behind the menu's cards |
| WakaBot | `.waka` | Grove Green + Black |
| YoungstaBot | `.youngsta` | Black + Red (223) |
| KnockerzBot | `.knockerz` | Teal + Black |

**Make your own** — a full theme editor (Themes page > **New custom theme**), not just a color picker: name, your own file extension, the complete color palette (accent, background, card, both text colors), corner radius and opacity, a pulsing accent and BrrrBot's snow if you want them, custom subtitle and brand tag, three quotes of your own that take turns under the page titles, and an optional Big Brrr track with its own BPM and drop offset. Saved and switchable right alongside the built-in themes.

## Extras

- **Auto safe mode** — on by default. While a cheat is on (a macro, Calculate or Pathfinder playing, noclip, speedhack, a tick rate other than 240, the autoclicker, Prevent Death, frame advance, Force Platformer, or anything that shows the way through: Click Indicators, the Survival Indicator, path preview, the macro path, practice range or a trainer's ghosts), the attempt doesn't count: no completion, no new best, no percentage saved, no stats, achievements, attempts or jumps added, nothing sent to the servers. The Safe mode switch (Hacks > Gameplay) does the same all the time, cheat or not. Start positions work as normal, and GD's Testmode label stays out of the way. Turn it off and a small red dot shows in the top-left corner while a cheat is on, drawn into the game so recordings and renders show it; a green one can show while nothing is.
- **Setup check** — warns about the settings that break GucciBot's engine: GD's Auto-Checkpoints, other mods' practice fixes, Frame Extrapolation in Mega Hack or QOLMod, and other bots loaded alongside. Once per launch, on the Macro page, and when you hit Record or Play. It never changes another mod's settings.
- **Disable bot** — the GucciBot switch at the top of Settings takes GucciBot out of the game without a restart: every hook and patch comes out, and the menu keeps only the switch that brings it back.
- **Frame Editor** — a timeline view of a macro's inputs. Every press is a bar you can drag, stretch or shorten, with undo and redo, an overview strip for long macros, and separate lanes for player 2.
- **Assistant Access** — an optional local server (Settings) for an AI assistant on your computer. It can read what the bot is doing (the frame, the loaded macro, Calculate's windows, Pathfinder's progress, GucciBot's own logs) and drive the game: step it, press jump, switch between recording and playback, load a macro, run Calculate or the Pathfinder, turn on practice mode and place checkpoints, open a level (downloading an online one) or leave it, and save and restart Geometry Dash. Off by default. It listens on 127.0.0.1 only, so other computers can't connect, but it has no password: any program on this computer can send it commands while it's on, so leave it off when you're not using it.
- **HUD** — live readouts in the corner of the level: frame, TPS, position, velocities, rotation, bot state, CPS and Max CPS, plus debug readouts (game tick, speed, gravity, checkpoints, level time, time warp, random states, action index, touching orbs and more).
- **Status bar** — under every page's title: what the bot is doing (recording, playing, calculating, pathfinding, rendering) with the frame, TPS, speed and inputs.
- **Saved macros** — each macro's "..." menu (a right click opens it too) loads, Calculates, renames, deletes or exports it, or moves it to plain .brrr; the list follows the replays folder on its own and tags each macro with the theme it was saved under. **Open folder** is one press away.
- **Hacks** (Hacks page) — Noclip, with a flash when it saves you and a Noclip accuracy readout that can let you die below a percentage you set; Instant Respawn or your own respawn delay (Auto retry); Force Platformer (a cheat); No Mirror, optionally only while recording; Hide attempts and Hide percentage; Input FPS (clicks land on only that many ticks a second); **Macro path** (the loaded macro's path and clicks, drawn in the level: by default only up to a little past the farthest you've gotten with it, and all of it with **Only up to your best** off) and **Practice range** (just its clicks near you), both cheats. **Path preview** (Hacks > Prediction) draws lines ahead of the player: green if you hold, red if you let go (a cheat).
- **Hitboxes** (Macro page) — collision boxes for players, blocks, hazards, orbs and portals, optionally only once you die, with a trail and your own color for each part.
- **Keybinds** (Settings) — 19 actions on keys of your choosing (with a keyboard; not on iOS): the menu, pause / frame advance, step forward and back, start/stop playback, noclip, safe mode, hitboxes, path preview, the autoclicker, an intentional death, Prevent Death, mirror inputs, RNG lock, Compact Mode and more. Click a key, then press the new one; right-click unbinds. A setting you switch by key stays switched after a restart, the same as from its menu switch (RNG lock, whose switch doesn't save either, excepted).
- **Look** (Settings) — text size, a cycling accent color, and animations on or off (switches slide, the JMF page's sky turns, BrrrBot's snow falls), saved as you change them.
- **Update check** — once a launch, from the main menu, against GucciBot's GitHub releases (Settings, with pre-releases optional). If a check ever fails to finish, it switches itself off.
- **Editor tools** — Macro Buffing (lays a macro's path into the level as objects) and Replace All (swap every object of one id for another, undoable).
- **Compact Mode** — a small window instead of the full tabbed one, so the bot stays usable while you're actually playing (Settings > Look, or a key you bind): Off / Rec / Play, a saved macro to load, its name, Save (which keeps a backup of the old file, as the full menu's does, while backups are on) and Calculate, the tick rate and game speed, Pause and step back / forward while a level is open, and a button back to the full menu.
- **BIG BRRRR** — a joke toggle in Settings. You'll know it when you see it.

---

## Credits

- **Nigelx1** — creator and owner of GucciBot; every idea, every theme, every decision is his call
- **Claude** — wrote the code and this page. Essentially the whole codebase, not a euphemism.
- **Juice** — designed the frame-window algorithm GucciBot ran on through 1.7 and the marker shapes in 1.8, and his Alignment-Independent method is back as Calculate's suggestion for suspicious clicks; lead co-tester, ran the mod into the ground on purpose finding the bugs nobody else caught
- **anticroom** — 1.8's Calculate *is* his analyzer: his Silicate frame-window rewrite, ported in near-verbatim and now the whole feature. Sub-tick CBF recording and the render intro card are his work too. Before that, GucciBot's first outside pull request — a real 7-fix accuracy pass. Also one of ToastyReplay's own devs.
- **dihmaster500** — tester, and asked for auto safe mode with its cheat indicator and for the Disable bot switch; his testing caught safe mode breaking start positions
- **DashBlaze** — promo help
- **Isaiah** — promo help
- **Absent** — Absense, another bot built on Silicate. Pathfinder's default engine is his: Absense's pathfinder, with its look-ahead and its model of the level, ported in whole. The assistant server, Check Macro, Replace All, the respawn timer and the black orb autoclicker loop are his ideas too, worked out again here from his source
- **Bogdaner09** — when a paid click-indicator mod turned out to be an exit scam, he's the one who started making the same thing free and open source ([github.com/Bogdaner09/mod](https://github.com/Bogdaner09/mod)), and GucciBot's Click Indicators are built around his. Vibecoded by his own admission, so credit's probably owed to whichever model wrote that too
- **GWDdoS** — optimization help, and the codebase cleanup that got this repo public-ready; also built Astral
- **ToastexGD** — built ToastyReplay, the project GucciBot actually started as and the reason there is a GucciBot at all. His renderer and FFmpeg pipeline carried this mod for most of its life; rendering now runs on Silicate's
- **C0nscious** — implemented NaN GD's L* precision formula in C++ as [Frame Window Counter](https://github.com/hyper-5/frame-window-counter) (MIT), a mod named after NaN's video series; that implementation is the code that reached GucciBot
- **Gucci Mane** — he's the truth. Brrr.

GucciBot is free software under the GNU General Public License v3.0, like Silicate, which it's built on.
