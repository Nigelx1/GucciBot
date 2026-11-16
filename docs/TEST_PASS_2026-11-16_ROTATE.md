# In-game test pass, 2026-11-16: rotate triggers in the trigger model

Claude's unattended runs on the night of 2026-11-15 into 2026-11-16 for the rotate lane (ROTATE_PLAN's minimum port: the default mode only). Landed as builds 2026-11-16-n (the World runs rotate triggers, behind the test switch pf_rotate, OFF by default) and 2026-11-16-o (the diagnostics the checks used).

- **Games:** the "deco" test copy (GDDeco), started through testcopy/assist only: safe mode forced and checked, muted, minimized and off screen, BelowNormal, closed in a finally. Nigel's game, saves and mods folder were never touched. Every test macro (zz_gbtest_*) was deleted afterward.
- **Cold:** a fresh game per level, no macro loaded, from 0%, Remember solutions off; every 100% got a cold replay.
- **One run per level** unless it says otherwise. Wall-clock times vary with machine load (other lanes built and tested at the same time).
- **Evidence** (in the session scratchpad, lanes\rotate\): RESULTS.md, PROGRESS.md, ladder\results\*.json, ladder\abspf_*.log, svr\results*.json, gr\grtail.json.

## Exactness (switch on)

| Test | Result | Numbers |
|---|---|---|
| Slaughterhouse: uid 15527, 3 more of group 133, group 134's center; 720 ticks from 12 ticks before x 2973 | PASS | 0 bad ticks bit for bit for all 5 (switch off: wrong on all 706 ticks they move). Oriented box identical on 719/720 (the other tick is before the copy came near enough to write it). Re-run: identical. |
| Slaughterhouse mid-rotate start (700 ticks); through the tick-3326 death the copy used to miss (600 ticks) | PASS | 0 bad ticks; real and simulated runs agree |
| Restores | PASS | 0 restore misses in over 20 million checks across all runs |
| Transform chain (50 angles at the level's first run start, one at every run start) | PASS | 50/50 bit-identical; 0 run starts with another transform |
| Slaughterhouse x 16713 UFO stretch, 5-minute search | PASS | on reached x 17412, off x 17311 |
| Slaughterhouse x 19940, groups 803/805 | PASS | the 9 watched objects that moved: 0 bad ticks |
| Slaughterhouse x 14024-14595 merged pairs | not tested | those groups have no members in the level |
| GRIEF x 21420+, rotated hazards | PARTIAL, not a pass | real and simulated die on the same tick on the same (unrotated) object, on and off; the watched hazards never moved in the real game, so no rotated hazard was compared. I'm not sure why. |
| A rotate with no center, lock rotation, merged pairs, the replace path | offline self-test only | not seen in a level tick by tick |

## The search (cold runs)

| Level | Off (default) | On |
|---|---|---|
| Slaughterhouse | 100% (master: 965 s, 1 death the copy missed) | 100%, 1369 s, 0 missed deaths, cold replay complete (40-minute cap: 93.46%) |
| GRIEF | 38.80%, three runs, same decisions | 26.13% (its first 106 decisions equal off's; it got less far before the 5-minute stuck rule) |
| Bloodbath, The Nightmare | 100% | 100%, decisions identical to off |
| Forest Temple | 83.99% | 83.99%, decisions identical |
| Speed | Slaughterhouse 17465 simulated ticks/s | 17008; GRIEF's World costs about 7-9 microseconds more per tick |

**Switch off against master:** the decisions were identical on all 7 levels against master e80ae92 (Stereo Madness, Base After Base, Clutterfunk, The Nightmare, Stalemate, Bloodbath, Slaughterhouse). After the rebase onto 2026-11-16-m and the three review fixes, they were identical again on Stereo Madness (87 lines) and Base After Base (105 lines).

**After the review fixes, switch on:** a 10-minute Slaughterhouse run: self-test passed, 50/50, 20479 run starts with 0 differing transforms and 0 chain-parent changes, 0 restore misses, 0 missed deaths, 0 error lines.

## Review fixes (in -n and -o)

1. A paused search's resume now also spots objects the real game only turned in place (it compared positions only).
2. sim_vs_real's exact watch reads Windows-only layouts only on Windows.
3. Rotate goes off for the level if the layer or scene above the rotate chain ever changes between run starts. I'm not sure GD ever moves them during play; in the Slaughterhouse run it never fired.

## Verdict

Off for 2.0. Nothing found the simulation wrong, but with it on the search is slower (Slaughterhouse) or gets less far (GRIEF). The Slaughterhouse cost is in the search, not the World: it now sees a death it used to miss and needs about twice the search to get around it.
