@echo off
setlocal EnableDelayedExpansion

echo.
echo  GucciBot -- Windows Build
echo  Frame perfect. Ice cold. Brrr.
echo.

:: ---- 1. Locate Geode SDK --------------------------------------------------
if not defined GEODE_SDK (
    set "C1=%LOCALAPPDATA%\Geode\sdk"
    set "C2=%APPDATA%\Geode\sdk"
    set "C3=%USERPROFILE%\geode-sdk"

    if exist "!C1!\CMakeLists.txt" ( set "GEODE_SDK=!C1!" & goto :found_sdk )
    if exist "!C2!\CMakeLists.txt" ( set "GEODE_SDK=!C2!" & goto :found_sdk )
    if exist "!C3!\CMakeLists.txt" ( set "GEODE_SDK=!C3!" & goto :found_sdk )

    echo ERROR: GEODE_SDK is not set and the SDK was not found.
    echo.
    echo Install the Geode SDK first:
    echo   geode sdk install
    echo.
    echo Or set it manually:
    echo   set GEODE_SDK=C:\path\to\sdk
    echo   build_win.bat
    echo.
    exit /b 1
)

:found_sdk
echo Found Geode SDK: %GEODE_SDK%
echo.

:: ---- 1.5. Initialize MSVC x64 build environment ---------------------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "delims=" %%i in ('"%VSWHERE%" -latest -property installationPath') do set "VS_PATH=%%i"
    if exist "!VS_PATH!\VC\Auxiliary\Build\vcvarsall.bat" (
        call "!VS_PATH!\VC\Auxiliary\Build\vcvarsall.bat" amd64 >nul 2>&1
        echo Initialized MSVC x64 environment.
    ) else (
        echo WARNING: vcvarsall.bat not found at !VS_PATH!. Build may fail.
    )
) else (
    echo WARNING: vswhere.exe not found. Run from a Developer Command Prompt if build fails.
)
echo.

:: ---- 2. Check dependencies -------------------------------------------------
where cmake >nul 2>&1
if errorlevel 1 (
    echo cmake not found. Download from cmake.org/download
    exit /b 1
)

where ninja >nul 2>&1
if errorlevel 1 (
    echo ninja not found. Download ninja-win.zip from github.com/ninja-build/ninja/releases
    exit /b 1
)

:: ---- 3. Configure ----------------------------------------------------------
set BUILD_DIR=build
:: Wipe stale compiler-detection cache so CMake re-probes with the active MSVC env.
:: _deps\ is left intact so CPM packages are not re-downloaded.
if exist "%BUILD_DIR%\CMakeCache.txt" del /f /q "%BUILD_DIR%\CMakeCache.txt"
for /d %%V in ("%BUILD_DIR%\CMakeFiles\*") do (
    if exist "%%V\CMakeCXXCompiler.cmake" del /f /q "%%V\CMakeCXXCompiler.cmake"
)
cmake -S . -B %BUILD_DIR% -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
if errorlevel 1 (
    echo.
    echo CMake configure failed.
    exit /b 1
)

:: ---- 4. Build --------------------------------------------------------------
cmake --build %BUILD_DIR% --config RelWithDebInfo
if errorlevel 1 (
    echo.
    echo Build failed.
    exit /b 1
)

:: ---- 5. Find and copy the .geode file -------------------------------------
set FOUND=0
for /r %BUILD_DIR% %%F in (*.geode) do (
    copy "%%F" . >nul
    echo.
    echo Build complete! Brrr.
    echo Output: %%~nxF
    echo.
    echo To install, copy %%~nxF to:
    echo   %APPDATA%\GeometryDash\geode\mods\
    echo.
    set FOUND=1
    goto :done
)

:done
if %FOUND%==0 (
    echo Build succeeded but no .geode file was found.
    echo Check the %BUILD_DIR%\ folder manually.
)
