# GucciBot 1.0

> Frame perfect. Ice cold. Brrr.

GucciBot is a Geometry Dash macro bot built on Silicate's physics engine, with a hypercompressed replay format and a full practice-trainer system built specifically around Jupiter My Favourite. This is the first proper public release.

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

## Themes

| Theme | Extension | Colors |
|---|---|---|
| GucciBot | `.brrr` | Gold |
| ToosiiBot (LSU) | `.toosii` | Purple + Gold |
| ToosiiBot (Syracuse) | `.toosii` | Orange + Navy |
| ToosiiBot (Sac State) | `.toosii` | Dark Green + Gold |
| JaBot | `.ja` | Navy + Light Blue |
| GiddeyBot | `.giddey` | Red + White |
| BamBot | `.bam` | Heat Black + Red |
| SexyyBot | `.sexyy` | Hot Pink + Purple |

## Extras

- **BIG BRRRR** — a joke toggle in Settings. You'll know it when you see it.

---

## Credits

- **guccimanefan** — GucciBot, themes, vision, the drip
- **ToastexGD** — ToastyReplay engine base and improvements
- **peony** — Silicate (dropped the source like Gucci drops albums. Brrr.)
- **Claude.ai** — rewrite and feature partner
