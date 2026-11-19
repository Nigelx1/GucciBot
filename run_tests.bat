@echo off
REM Offline tests: the GBR6 sub-tick and TPS sections, Check Macro, Macro Tools, the random teleport's pick, other bots' formats, the menu art's PNG reader, Share my windows' picture and macro file, Hitborders' shapes, the AI pilot's networks, Ship Steering's controller, Save's rule, the queues, the Trainer lane's rules, Possible positions' math and the Frame Editor's rules.

setlocal
set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist %VCVARS% (
    echo Could not find vcvars64.bat -- edit the path at the top of this file.
    exit /b 1
)

call %VCVARS% >nul 2>&1
if not exist build mkdir build

REM GBR6's sub-tick and TPS sections, and the encoder keeping buttons. /O2 /MD to match the mod's build.
cl /nologo /std:c++20 /EHsc /O2 /MD /I src ^
   tools\run_gbr6_tests.cpp src\core\gbr6_format.cpp /Fe:build\run_gbr6_tests.exe /Fo:build\ || exit /b 1

build\run_gbr6_tests.exe || exit /b 1

REM Macro problem check (Absense's editor Problems, over GucciBot's actions).
cl /nologo /std:c++20 /EHsc /O2 /MD /I src ^
   tools\run_macrocheck_tests.cpp /Fe:build\run_macrocheck_tests.exe /Fo:build\ || exit /b 1

build\run_macrocheck_tests.exe || exit /b 1

REM Macro Tools: trim, merge, diff and TPS changes (src/tools/macro_ops).
cl /nologo /std:c++20 /EHsc /O2 /MD /I src ^
   tools\run_macroops_tests.cpp src\tools\macro_ops.cpp src\core\gbr6_format.cpp /Fe:build\run_macroops_tests.exe /Fo:build\ || exit /b 1

build\run_macroops_tests.exe || exit /b 1

REM The random teleport's pick (src/core/teleport_pick.hpp), which seeded teleports use off Windows.
cl /nologo /std:c++20 /EHsc /O2 /MD /I src ^
   tools\run_teleport_tests.cpp /Fe:build\run_teleport_tests.exe /Fo:build\ || exit /b 1

build\run_teleport_tests.exe || exit /b 1

REM Other bots' formats (src/tools/macro_convert): every writer round-trips. zlib comes from the
REM mod's own build, so build the mod once first.
cl /nologo /std:c++20 /EHsc /O2 /MD /I src /I build\_deps\zlib-src /I build\_deps\zlib-build ^
   tools\run_convert_tests.cpp src\tools\macro_convert.cpp src\core\gbr6_format.cpp build\_deps\zlib-build\zlibstatic.lib ^
   /Fe:build\run_convert_tests.exe /Fo:build\ || exit /b 1

build\run_convert_tests.exe || exit /b 1

REM JupiterBot's art read the way the mod reads it (src/ui/png), as in resources/jupiter and as the
REM packager stores it in the .geode, against the tables that place it (src/ui/pages/jupiter_art.hpp);
REM SolarBot's, as in resources/solar, against its own (src/ui/pages/solar_art.hpp);
REM GucciBot's mark, as in resources/brand and as packaged (src/ui/brand.cpp), with the halving
REM ui/art does before a texture (png::halve, fit, drawnBounds);
REM and the reader against Pillow on every kind of PNG (tools/testdata/png, from
REM tools/make_png_tests.py). zlib as above.
cl /nologo /std:c++20 /EHsc /O2 /MD /I src /I build\_deps\zlib-src /I build\_deps\zlib-build ^
   tools\run_art_tests.cpp src\ui\png.cpp build\_deps\zlib-build\zlibstatic.lib ^
   /Fe:build\run_art_tests.exe /Fo:build\ || exit /b 1

build\run_art_tests.exe || exit /b 1

REM Share my windows' picture (src/ui/share_draw) and the PNG writer it saves with (png::encode): round
REM trips through the reader, and sample cards (dark, light, no score) drawn with the menu's fonts, written
REM to build\share_sample_*.png to look at. zlib as above; stb_truetype is imgui's copy.
cl /nologo /std:c++20 /EHsc /O2 /MD /I src /I build\_deps\zlib-src /I build\_deps\zlib-build /I build\_deps\imgui-src ^
   tools\run_share_tests.cpp src\ui\share_draw.cpp src\ui\png.cpp build\_deps\zlib-build\zlibstatic.lib ^
   /Fe:build\run_share_tests.exe /Fo:build\ || exit /b 1

build\run_share_tests.exe || exit /b 1

REM Share my windows' macro file (src/tools/share_bundle): the .zip with a macro and its sidecars packs and
REM unpacks byte for byte, and a damaged, cut, oversized or path-laden one is refused. zlib as above.
cl /nologo /std:c++20 /EHsc /O2 /MD /I src /I build\_deps\zlib-src /I build\_deps\zlib-build ^
   tools\run_bundle_tests.cpp src\tools\share_bundle.cpp build\_deps\zlib-build\zlibstatic.lib ^
   /Fe:build\run_bundle_tests.exe /Fo:build\ || exit /b 1

build\run_bundle_tests.exe || exit /b 1

REM Hitborders' shapes (src/tools/hitborder_geom): each object's collision shape grown by the player's,
REM against point-by-point copies of GD's collision tests (rects, rotated boxes, saws, blocks, slopes).
cl /nologo /std:c++20 /EHsc /O2 /MD /I src ^
   tools\run_hitborder_tests.cpp src\tools\hitborder_geom.cpp /Fe:build\run_hitborder_tests.exe /Fo:build\ || exit /b 1

build\run_hitborder_tests.exe || exit /b 1

REM The AI pilot's networks (src/ai/net) and its record expansion (src/ai/obs_core) against the
REM trainer's golden set in tools/ai/golden (python tools/ai/export.py golden): outputs within 1e-5,
REM the same decisions, the expansion bit for bit, broken weight files refused; self-training (src/ai/train):
REM the GBNN writer round-trips, the gradient matches a finite difference, the loss falls on a toy set.
cl /nologo /std:c++20 /EHsc /O2 /MD /I src ^
   tools\run_ai_net_tests.cpp src\ai\net.cpp src\ai\obs_core.cpp src\ai\train.cpp /Fe:build\run_ai_net_tests.exe /Fo:build\ || exit /b 1

build\run_ai_net_tests.exe || exit /b 1

REM Ship Steering's controller (src/analysis/ac/steer): the motion model's fit, handback from off the
REM line, the dwell between taps, the same taps twice, and no model from too few samples.
cl /nologo /std:c++20 /EHsc /O2 /MD /I src ^
   tools\run_steer_tests.cpp src\analysis\ac\steer.cpp /Fe:build\run_steer_tests.exe /Fo:build\ || exit /b 1

build\run_steer_tests.exe || exit /b 1

REM Save's rule (src/tools/save_target): write at once to a new file or the macro's own, ask before
REM replacing another macro's file, and the name Save as offers.
cl /nologo /std:c++20 /EHsc /O2 /MD /I src ^
   tools\run_savetarget_tests.cpp /Fe:build\run_savetarget_tests.exe /Fo:build\ || exit /b 1

build\run_savetarget_tests.exe || exit /b 1

REM The queues (src/tools/queue_core): the level IDs the Pathfinder queue takes, the macro names it saves
REM under, and the saved state a restart reads back.
cl /nologo /std:c++20 /EHsc /O2 /MD /I src ^
   tools\run_queue_tests.cpp /Fe:build\run_queue_tests.exe /Fo:build\ || exit /b 1

build\run_queue_tests.exe || exit /b 1

REM The Trainer lane's rules (src/trainers/lane_rules.hpp): a note's tier from its window, the early and
REM late allowance, the judgment and which note a press answers.
cl /nologo /std:c++20 /EHsc /O2 /MD /I src ^
   tools\run_lane_tests.cpp /Fe:build\run_lane_tests.exe /Fo:build\ || exit /b 1

build\run_lane_tests.exe || exit /b 1

REM Possible positions (src/tools/positions_core): the window from Calculate's mark, the shifts and
REM scripts the copies run, which part of a run counts, the envelope (cap, pinch, wide through a later
REM click), thinning that never narrows, culling and the fill.
cl /nologo /std:c++20 /EHsc /O2 /MD /I src ^
   tools\run_positions_tests.cpp src\tools\positions_core.cpp /Fe:build\run_positions_tests.exe /Fo:build\ || exit /b 1

build\run_positions_tests.exe || exit /b 1

REM Frame Editor rules (src/tools/edit_core, ported from Absense's macro editor).
cl /nologo /std:c++20 /EHsc /O2 /MD /I src ^
   tools\run_edit_tests.cpp src\tools\edit_core.cpp /Fe:build\run_edit_tests.exe /Fo:build\ || exit /b 1

build\run_edit_tests.exe
exit /b %ERRORLEVEL%
