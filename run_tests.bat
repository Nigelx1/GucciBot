@echo off
REM Runs the frame editor's rule tests outside Geometry Dash.
REM The same cases also run at startup in-game and show up in the
REM Diagnostics panel -- this is just the fast way to check them.

setlocal
set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist %VCVARS% (
    echo Could not find vcvars64.bat -- edit the path at the top of this file.
    exit /b 1
)

call %VCVARS% >nul 2>&1
if not exist build mkdir build

cl /nologo /std:c++20 /EHsc /I src /DGB_EDIT_CORE_TEST_STANDALONE ^
   tools\run_edit_tests.cpp /Fe:build\run_edit_tests.exe /Fo:build\ || exit /b 1

build\run_edit_tests.exe
exit /b %ERRORLEVEL%
