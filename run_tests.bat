@echo off
REM Offline tests: the GBR6 sub-tick section, Check Macro and the Frame Editor's rules.

setlocal
set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist %VCVARS% (
    echo Could not find vcvars64.bat -- edit the path at the top of this file.
    exit /b 1
)

call %VCVARS% >nul 2>&1
if not exist build mkdir build

REM GBR6's sub-tick section (SCBF offsets). /O2 /MD to match the mod's build.
cl /nologo /std:c++20 /EHsc /O2 /MD /I src ^
   tools\run_gbr6_tests.cpp src\core\gbr6_format.cpp /Fe:build\run_gbr6_tests.exe /Fo:build\ || exit /b 1

build\run_gbr6_tests.exe || exit /b 1

REM Macro problem check (Absense's editor Problems, over GucciBot's actions).
cl /nologo /std:c++20 /EHsc /O2 /MD /I src ^
   tools\run_macrocheck_tests.cpp /Fe:build\run_macrocheck_tests.exe /Fo:build\ || exit /b 1

build\run_macrocheck_tests.exe || exit /b 1

REM Frame Editor rules (src/tools/edit_core, ported from Absense's macro editor).
cl /nologo /std:c++20 /EHsc /O2 /MD /I src ^
   tools\run_edit_tests.cpp src\tools\edit_core.cpp /Fe:build\run_edit_tests.exe /Fo:build\ || exit /b 1

build\run_edit_tests.exe
exit /b %ERRORLEVEL%
