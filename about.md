# GucciBot 6.0

> Frame perfect. GBR6. Brrr.

GucciBot 6.0 is a complete rebuild on Silicate's engine with a brand new hypercompressed replay format. Built for all four bot disciplines simultaneously.

---

## What's new in 6.0

**GBR6 Hypercompressed Format**
- 2 bytes per input. Matches yBot file sizes for human gameplay.
- **Tap encoding** — wave/ship press+release pairs stored in 1 entry instead of 2. 50% smaller for those sections automatically.
- **Autoclicker delta compression** — an autoclicker macro of any length, including octillions of clicks, stores in 4 bytes. The bot stores the *description* of the pattern, not the output. Nobody else has this publicly.
- Full backward compatibility — old `.brrr` files load automatically.

**Engine (from 5.0)**
- Silicate's full physics engine — 5 midhooks at exact GD 2.2081 offsets, proper TPS bypass, SSB fix, lock delta, frame extrapolation
- Intentional deaths that actually work — full Silicate `PracticeFix` flow
- Backwards stepping
- Mirror inputs, maintain gravity, auto-flip, prevent death
- Complete `SavedPlayerCheckpoint` state capture (the most complete in any public bot)

**Features (from 5.0)**
- Macro diff viewer — compare two replays frame by frame, up to 500 diffs
- TPS mid-macro changes — change TPS during recording, stored in the replay
- Noclip accuracy display with optional threshold
- Convert legacy macros to BRR with one click
- Bot settings presets — save and load your full config as a named preset
- Metadata editor — rename and re-author any macro
- Autosave at level end and/or on a timer

---

## The 4 disciplines

| Discipline | Focus | GucciBot 6.0 |
|---|---|---|
| General | Render settings, polish, features | ✅ Top tier |
| ILL | Gameplay gimmicks, accuracy | ✅ Top tier |
| SLL | Efficiency, accuracy | ✅ Competitive (Silicate engine) |
| PPLL | Hyper efficiency | ✅ Best in class (GBR6 + autoclicker delta) |

---

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

---

## Credits

- **guccimanefan** — GucciBot, themes, vision, the drip
- **ToastyExGD** — ToastyReplay engine base and improvements
- **peony** — Silicate (dropped the source like Gucci drops albums. Brrr.)
- **Claude.ai** — 5.0 and 6.0 rewrite partner
