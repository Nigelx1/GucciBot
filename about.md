# GucciBot 1.4

> Frame perfect. Ice cold. Brrr.

GucciBot is a Geometry Dash macro bot built on Silicate's physics engine, with a hypercompressed replay format and a full practice-trainer system -- one dedicated tab built around Jupiter My Favourite, and a second general-purpose one that works with any of your own saved macros.

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

## Practice & Analysis

- Macro diff viewer — compare two replays frame by frame
- Frame-window analyzer ("Calculate") — per-click timing windows across real game frames
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

## Extras

- **Compact Mode** — a small corner panel (record/play, save/name/Calculate, macro picker, TPS/speed, frame stepping) instead of the full tabbed window, so the bot stays usable while you're actually playing.
- **BIG BRRRR** — a joke toggle in Settings. You'll know it when you see it.

---

## Credits

- **guccimanefan** (Nigelx1) — concept, direction, themes, testing
- **Juice** — frame-window algorithm design, lead co-tester
- **Claude** — wrote the code and this page. Essentially the whole codebase, not a euphemism.
- **ToastexGD** — ToastyReplay engine base and improvements
- **peony** — Silicate (dropped the source like Gucci drops albums. Brrr.)
