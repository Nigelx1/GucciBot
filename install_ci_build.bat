@echo off
:: GitHub's build of this commit - every platform plus the renderer's FFmpeg
:: DLLs, the same file a release ships - checked and installed into Geometry
:: Dash. A copy stays in build\ci\. See tools\install-ci-build.ps1.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\install-ci-build.ps1" %*
exit /b %ERRORLEVEL%
