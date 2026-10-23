# Puts GitHub's build of a commit into Geometry Dash, after checking it's
# complete. That build is the universal package: every platform plus every
# resource, the renderer's FFmpeg DLLs included. It's the file a release ships
# and the one testers get, so Nigel's game runs exactly what everyone else does
# (Nigel, 2026-10-12).
#
#   .\install_ci_build.bat            the commit checked out here (HEAD)
#   .\install_ci_build.bat <commit>   another one
#
# Waits while GitHub is still building (a push to master takes about 8
# minutes). Keeps a copy at build\ci\nigelx1.guccibot.geode for releases and
# sharing. Needs the GitHub CLI (gh), signed in. Won't install while Geometry
# Dash is running.

param([string]$Commit = "")
$ErrorActionPreference = "Stop"
$Repo = "Nigelx1/GucciBot"
$Workflow = "multi-platform.yml"
$Artifact = "GucciBot (all platforms)"
$Root = Split-Path $PSScriptRoot -Parent

if (-not $Commit) { $Commit = (git -C $Root rev-parse HEAD).Trim() }
$short = $Commit.Substring(0, [Math]::Min(7, $Commit.Length))
Write-Host "GucciBot: GitHub's build of $short"

# 1. GitHub's run for this commit, waiting while it builds
$deadline = (Get-Date).AddMinutes(30)
while ($true) {
    $runs = @(gh run list --repo $Repo --workflow $Workflow --commit $Commit --json databaseId,status,conclusion | ConvertFrom-Json)
    if ($runs.Count -gt 0 -and $runs[0]) {
        $run = $runs[0]
        if ($run.status -eq "completed") { break }
        Write-Host "  still building ($($run.status))..."
    } else {
        Write-Host "  no build for $short yet (pushed to master?) - waiting..."
    }
    if ((Get-Date) -gt $deadline) { throw "no finished build for $short after 30 minutes" }
    Start-Sleep -Seconds 30
}
if ($run.conclusion -ne "success") {
    throw "GitHub's build of $short didn't succeed ($($run.conclusion)): gh run view $($run.databaseId) --repo $Repo"
}

# 2. Download it and check it: the five platform binaries and the renderer's seven DLLs
$tmp = Join-Path $env:TEMP "guccibot-ci-$short"
if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
gh run download $run.databaseId --repo $Repo --name $Artifact --dir $tmp
$geode = Get-ChildItem $tmp -Recurse -Filter "nigelx1.guccibot.geode" | Select-Object -First 1
if (-not $geode) { throw "the download has no nigelx1.guccibot.geode" }
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead($geode.FullName)
try { $names = @($zip.Entries | ForEach-Object { $_.FullName }) } finally { $zip.Dispose() }
$need = @("nigelx1.guccibot.dll", "nigelx1.guccibot.dylib", "nigelx1.guccibot.ios.dylib",
          "nigelx1.guccibot.android32.so", "nigelx1.guccibot.android64.so") +
        @(@("avcodec-62.dll", "avdevice-62.dll", "avfilter-11.dll", "avformat-62.dll", "avutil-60.dll",
            "swresample-6.dll", "swscale-9.dll") | ForEach-Object { "resources/nigelx1.guccibot/$_" })
$missing = @($need | Where-Object { $names -notcontains $_ })
if ($missing.Count -gt 0) { throw "incomplete package, missing: $($missing -join ', ')" }
Write-Host ("  complete: 5 platforms, 7 renderer DLLs ({0:N0} MB)" -f ($geode.Length / 1MB))

# 3. A copy for releases and sharing
$keep = Join-Path $Root "build\ci"
New-Item -ItemType Directory -Force $keep | Out-Null
Copy-Item $geode.FullName (Join-Path $keep "nigelx1.guccibot.geode") -Force
Write-Host "  copy kept: build\ci\nigelx1.guccibot.geode"

# 4. Install it where Geode's CLI says the game is (its current profile)
if (Get-Process GeometryDash -ErrorAction SilentlyContinue) {
    throw "Geometry Dash is running - close it and run this again (the copy in build\ci is ready)"
}
$cfg = Get-Content (Join-Path $env:LOCALAPPDATA "Geode\config.json") -Raw | ConvertFrom-Json
$prof = @($cfg.profiles | Where-Object { $_.name -eq $cfg.'current-profile' })[0]
if (-not $prof) { throw "Geode's CLI has no current profile (geode profile add ...)" }
$mods = Join-Path (Split-Path $prof.'gd-path' -Parent) "geode\mods"
Copy-Item $geode.FullName (Join-Path $mods "nigelx1.guccibot.geode") -Force
Remove-Item $tmp -Recurse -Force
Write-Host "  installed: $mods"
