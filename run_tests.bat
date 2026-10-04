@echo off
REM Offline tests: the GBR6 sub-tick and TPS sections, Check Macro, Macro Tools and the Frame Editor's rules.

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

REM Frame Editor rules (src/tools/edit_core, ported from Absense's macro editor).
cl /nologo /std:c++20 /EHsc /O2 /MD /I src ^
   tools\run_edit_tests.cpp src\tools\edit_core.cpp /Fe:build\run_edit_tests.exe /Fo:build\ || exit /b 1

build\run_edit_tests.exe
exit /b %ERRORLEVEL%
