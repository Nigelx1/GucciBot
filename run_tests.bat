@echo off
REM Offline tests: the GBR6 sub-tick and TPS sections, Check Macro, Macro Tools, the random teleport's pick, other bots' formats, the menu art's PNG reader, Hitborders' shapes and the Frame Editor's rules.

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

REM Hitborders' shapes (src/tools/hitborder_geom): each object's collision shape grown by the player's,
REM against point-by-point copies of GD's collision tests (rects, rotated boxes, saws, blocks, slopes).
cl /nologo /std:c++20 /EHsc /O2 /MD /I src ^
   tools\run_hitborder_tests.cpp src\tools\hitborder_geom.cpp /Fe:build\run_hitborder_tests.exe /Fo:build\ || exit /b 1

build\run_hitborder_tests.exe || exit /b 1

REM Frame Editor rules (src/tools/edit_core, ported from Absense's macro editor).
cl /nologo /std:c++20 /EHsc /O2 /MD /I src ^
   tools\run_edit_tests.cpp src\tools\edit_core.cpp /Fe:build\run_edit_tests.exe /Fo:build\ || exit /b 1

build\run_edit_tests.exe
exit /b %ERRORLEVEL%
