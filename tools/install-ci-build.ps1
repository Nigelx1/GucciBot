# Puts GitHub's build of a commit into Geometry Dash, after checking it's
# complete. That build is the universal package: every platform plus every
# resource, the renderer's FFmpeg DLLs and macOS dylibs included. It's the file a release ships
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
#
# The GD 2.2091 (Geode 6) Windows package, from multi-platform-2209.yml, goes
# only into one of our own GD 2.2091 TEST copies, never into a real game:
#
#   .\install_ci_build.bat -Gd2209 -Folder <test copy> [-Commit <commit>]
#
# The folder must hold GD2209.exe or GD2209b.exe and no GeometryDash.exe (GD
# and Geode pick the save folder from the exe's name, so a GeometryDash.exe
# would share the real save), must not be in a Steam library, and must have
# Geode 6 in it. The package must say geode 6 and gd win 2.2091 and nothing
# else. A copy stays at build\ci\nigelx1.guccibot2209.geode. Without -Gd2209
# the script does what it always did and reads only multi-platform.yml.

param([string]$Commit = "", [switch]$Gd2209, [string]$Folder = "")
$ErrorActionPreference = "Stop"
$Repo = "Nigelx1/GucciBot"
$Workflow = "multi-platform.yml"
$Artifact = "GucciBot (all platforms)"
$Root = Split-Path $PSScriptRoot -Parent

# GitHub's run of $Workflow for $Commit to download: the newest one that built
# successfully, waiting while none has yet and one is still building. If every
# run finished without success, the newest one comes back and the caller says
# why. A commit can have several runs (a workflow_dispatch of the same commit).
# ConvertFrom-Json in Windows PowerShell 5.1 passes a JSON array down the
# pipeline as one object, so the list is unrolled here: until 2026-11-18 a
# second run made $runs[0] the whole list, and gh run download got every run id.
function Wait-CommitRun([string]$NoRunYet) {
    $deadline = (Get-Date).AddMinutes(30)
    while ($true) {
        $json = (gh run list --repo $Repo --workflow $Workflow --commit $Commit --json databaseId,status,conclusion) -join "`n"
        $runs = @(if ($json) { ($json | ConvertFrom-Json) | ForEach-Object { $_ } })
        $ok = @($runs | Where-Object { $_.status -eq "completed" -and $_.conclusion -eq "success" })
        if ($ok.Count -gt 0) { return $ok[0] }
        $busy = @($runs | Where-Object { $_.status -ne "completed" })
        if ($runs.Count -gt 0 -and $busy.Count -eq 0) { return $runs[0] }
        if ($busy.Count -gt 0) { Write-Host "  still building ($($busy[0].status))..." }
        else { Write-Host "  $NoRunYet" }
        if ((Get-Date) -gt $deadline) { throw "no finished build of $Workflow for $short after 30 minutes" }
        Start-Sleep -Seconds 30
    }
}

if ($Gd2209) {
    # 0. Where it goes, checked before anything is downloaded
    if (-not $Folder) { throw "-Gd2209 needs -Folder <a GD 2.2091 test copy>" }
    if (-not (Test-Path -LiteralPath $Folder -PathType Container)) { throw "no folder $Folder" }
    $dir = (Resolve-Path -LiteralPath $Folder).ProviderPath.TrimEnd('\')
    if ((Get-Item -LiteralPath $dir -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw "refusing $dir`: it's a link to another folder"
    }
    if (Test-Path -LiteralPath (Join-Path $dir "GeometryDash.exe")) {
        throw "refusing $dir`: it has GeometryDash.exe, which shares the real save folder (only GD2209.exe / GD2209b.exe test copies)"
    }
    if ($dir -match '\\steamapps\\') { throw "refusing $dir`: it's in a Steam library, not a test copy" }
    $exes = @("GD2209.exe", "GD2209b.exe" | Where-Object { Test-Path -LiteralPath (Join-Path $dir $_) })
    if ($exes.Count -ne 1) { throw "refusing $dir`: it needs exactly one of GD2209.exe or GD2209b.exe (found $($exes.Count))" }
    $exe = $exes[0]
    if (-not (Test-Path -LiteralPath (Join-Path $dir "Geode.dll"))) { throw "refusing $dir`: no Geode in it" }
    $mods = Join-Path $dir "geode\mods"
    if (-not (Test-Path -LiteralPath $mods -PathType Container)) { throw "refusing $dir`: no geode\mods folder" }

    $Workflow = "multi-platform-2209.yml"
    $Artifact = "GucciBot GD 2.2091 (Geode 6, Windows)"
    if (-not $Commit) { $Commit = (git -C $Root rev-parse HEAD).Trim() }
    $short = $Commit.Substring(0, [Math]::Min(7, $Commit.Length))
    Write-Host "GucciBot: GitHub's GD 2.2091 build of $short, for the test copy $dir ($exe)"

    # 1. GitHub's run for this commit, waiting while it builds
    $run = Wait-CommitRun "no GD 2.2091 build for $short yet (gh workflow run $Workflow --ref <branch>) - waiting..."
    if ($run.conclusion -ne "success") {
        throw "GitHub's GD 2.2091 build of $short didn't succeed ($($run.conclusion)): gh run view $($run.databaseId) --repo $Repo"
    }

    # 2. Download it and check it: the Windows binary, the seven DLLs, and a
    #    mod.json for Geode 6 and GD 2.2091 on Windows only
    $tmp = Join-Path $env:TEMP "guccibot-ci2209-$short"
    if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
    gh run download $run.databaseId --repo $Repo --name $Artifact --dir $tmp
    $geode = @(Get-ChildItem $tmp -Recurse -Filter "*.geode")
    if ($geode.Count -ne 1) { throw "the download should hold one .geode, it holds $($geode.Count)" }
    $geode = $geode[0]
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [System.IO.Compression.ZipFile]::OpenRead($geode.FullName)
    try {
        $names = @($zip.Entries | ForEach-Object { $_.FullName })
        $entry = $zip.GetEntry("mod.json")
        if (-not $entry) { throw "the package has no mod.json" }
        $reader = New-Object System.IO.StreamReader($entry.Open())
        try { $mod = $reader.ReadToEnd() | ConvertFrom-Json } finally { $reader.Dispose() }
    } finally { $zip.Dispose() }
    $need = @("nigelx1.guccibot.dll") +
            @(@("avcodec-62.dll", "avdevice-62.dll", "avfilter-11.dll", "avformat-62.dll", "avutil-60.dll",
                "swresample-6.dll", "swscale-9.dll") | ForEach-Object { "resources/nigelx1.guccibot/$_" })
    $missing = @($need | Where-Object { $names -notcontains $_ })
    if ($missing.Count -gt 0) { throw "incomplete package, missing: $($missing -join ', ')" }
    $gdKeys = @($mod.gd.PSObject.Properties | ForEach-Object { $_.Name })
    if (-not ("$($mod.geode)" -match '^6\.') -or $gdKeys.Count -ne 1 -or $mod.gd.win -ne "2.2091") {
        throw "this package isn't the GD 2.2091 Windows one (geode $($mod.geode), gd $($gdKeys -join ','))"
    }
    Write-Host ("  complete: Windows binary, 7 renderer DLLs, geode {0}, gd win {1} ({2:N0} MB)" -f $mod.geode, $mod.gd.win, ($geode.Length / 1MB))

    # 3. A copy for sharing, under a name that can't be taken for the 2.2081 file
    $keep = Join-Path $Root "build\ci"
    New-Item -ItemType Directory -Force $keep | Out-Null
    Copy-Item $geode.FullName (Join-Path $keep "nigelx1.guccibot2209.geode") -Force
    Write-Host "  copy kept: build\ci\nigelx1.guccibot2209.geode"

    # 4. Into the test copy, while its game isn't running
    $name = [IO.Path]::GetFileNameWithoutExtension($exe)
    # Only a game started from this folder counts (other copies' games may
    # run); one whose path can't be read counts too, to be safe.
    $live = @(Get-Process -Name $name -ErrorAction SilentlyContinue |
              Where-Object { -not $_.Path -or $_.Path.StartsWith("$dir\", [StringComparison]::OrdinalIgnoreCase) })
    if ($live.Count -gt 0) {
        Remove-Item $tmp -Recurse -Force
        throw "$exe is running from $dir - close it and run this again (the copy in build\ci is ready)"
    }
    Copy-Item $geode.FullName (Join-Path $mods "nigelx1.guccibot.geode") -Force
    Remove-Item $tmp -Recurse -Force
    Write-Host "  installed: $mods"
    exit 0
}
if ($Folder) { throw "-Folder is only for -Gd2209 (a GD 2.2091 test copy)" }

if (-not $Commit) { $Commit = (git -C $Root rev-parse HEAD).Trim() }
$short = $Commit.Substring(0, [Math]::Min(7, $Commit.Length))
Write-Host "GucciBot: GitHub's build of $short"

# 1. GitHub's run for this commit, waiting while it builds
$run = Wait-CommitRun "no build for $short yet (pushed to master?) - waiting..."
if ($run.conclusion -ne "success") {
    throw "GitHub's build of $short didn't succeed ($($run.conclusion)): gh run view $($run.databaseId) --repo $Repo"
}

# 2. Download it and check it: the five platform binaries, the renderer's seven DLLs
#    and its seven macOS dylibs (packed since build 2026-11-15-be)
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
            "swresample-6.dll", "swscale-9.dll",
            "libavcodec.62.dylib", "libavdevice.62.dylib", "libavfilter.11.dylib", "libavformat.62.dylib",
            "libavutil.60.dylib", "libswresample.6.dylib", "libswscale.9.dylib") |
            ForEach-Object { "resources/nigelx1.guccibot/$_" })
$missing = @($need | Where-Object { $names -notcontains $_ })
if ($missing.Count -gt 0) { throw "incomplete package, missing: $($missing -join ', ')" }
Write-Host ("  complete: 5 platforms, 7 renderer DLLs, 7 renderer dylibs ({0:N0} MB)" -f ($geode.Length / 1MB))

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
