# OcarinaVR installer (Windows). Builds the game with Longshot from YOUR OWN copy, with the VR mode in it.
#
#   powershell -ExecutionPolicy Bypass -File install.ps1 -Game "C:\path\your-game.nup"
#   powershell -ExecutionPolicy Bypass -File install.ps1 -Romfs "C:\path\romfs" -Exefs "C:\path\exefs"
#   powershell -ExecutionPolicy Bypass -File install.ps1 -CoreOnly      (after an update: rebuilds retraced.dll only)
#
# What it does, all inside this folder (nothing is installed on the system):
#   1. downloads the Longshot Builder of the release this mod was written for (it carries Longshot's source, the very
#      files the Builder builds from), the build tools its tools.lock names (llvm-mingw, Python, cpm, third_party) and
#      Khronos's OpenXR loader into _build\downloads, checks each file's SHA-256, and lays them out as the Builder's own
#      setup does, in _build\LongshotBuilder\tools;
#   2. adds this mod's hook lines to that source (hooks.json, tools\apply-hooks.ps1);
#   3. builds the game from your copy with Longshot's own script (10 to 45 minutes, about 60 GB free needed);
#   4. builds retraced.dll, copies the OpenXR loader, and reads Link's arms and skeleton from your game's files.
# No game file is downloaded, and none leaves your PC.
param(
    [string]$Game = '',
    [string]$Romfs = '',
    [string]$Exefs = '',
    [string]$Language = 'en-US',
    [string]$SwordHand = '',      # right or left (asked in a window when started by a double-click)
    [switch]$CoreOnly
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$build = Join-Path $root '_build'
$dl = Join-Path $build 'downloads'
$tools = Join-Path $build 'LongshotBuilder\tools'   # (Longshot's source, with the build tools in it: the Builder's layout)
$out = Join-Path $root 'game'

# Longshot v3.1.1 (a2dc538). The Builder's SHA-256 is the release's SHA256SUMS; the tools' are its tools.lock's, with
# their URLs in its order (Longshot's copy from the CDN, the same from git.magicbean.dev, then upstream). dir: where it
# goes (a tool: the Builder's own place for it, tools.lock's "dest" under LongshotBuilder\tools); from: the folder
# inside the archive that becomes it (tools.lock's "prefix"; '': the whole archive, unpacked into dir). The Builder
# comes first: the tools go inside its folder.
$longshotVersion = 'v3.1.1'
$pkg = 'MagicBeanDev/generic'
$cdn = @('https://git-cdn.magicbean.dev/api/packages', 'https://git.magicbean.dev/api/packages')
function Mirrors([string]$name, [string]$version, [string]$sha, [string]$upstream = '') {
    $u = @($cdn | ForEach-Object { "$_/$pkg/$name-windows-x64/$version/$sha-$name.zip" })
    if ($upstream) { $u += $upstream }
    return $u
}
$parts = @(
    @{ name = 'builder'; file = "LongshotBuilder-$longshotVersion-windows-x64.zip"; sha = 'e476b89469e17732d96adf17343077f58484478812b869cd0b6b3f9ee33c15fe'; dir = $build; from = ''
       urls = @("https://git.magicbean.dev/MagicBeanDev/Longshot/releases/download/$longshotVersion/LongshotBuilder-$longshotVersion-windows-x64.zip") },
    @{ name = 'llvm-mingw'; file = 'llvm-mingw-20261104-ucrt-x86_64.zip'; sha = '552191b5754c49a5a20723276a6fc56541ea1922c4c50b7b669022333fc1b40f'; dir = (Join-Path $tools 'llvm-mingw'); from = 'llvm-mingw-20261104-ucrt-x86_64'
       urls = (Mirrors 'llvm-mingw' '20261104' '552191b5754c49a5a20723276a6fc56541ea1922c4c50b7b669022333fc1b40f' 'https://github.com/mstorsjo/llvm-mingw/releases/download/20261104/llvm-mingw-20261104-ucrt-x86_64.zip') },
    @{ name = 'python'; file = 'longshot-python-c0eb2e3fbaba.zip'; sha = 'c0eb2e3fbabae82cb7d3dc61ec4b0e9b4b854269d9c38c3bbcc5a08e4497bd12'; dir = (Join-Path $tools 'python'); from = 'HookshotBuilder\tools\python'
       urls = (Mirrors 'python' '3.14-hookshot-v2.1.0' 'c0eb2e3fbabae82cb7d3dc61ec4b0e9b4b854269d9c38c3bbcc5a08e4497bd12' 'https://github.com/NotRexed/Hookshot/releases/download/builder-parts/HookshotBuilder-python-c0eb2e3fbaba.zip') },
    @{ name = 'cpm'; file = 'longshot-cpm-4eeae3a8d501.zip'; sha = '4eeae3a8d50118dd5c15f2c18dcdc452fbf1dfafceeffcad3bca97aadffe829b'; dir = (Join-Path $tools 'cpm'); from = 'cpm'
       urls = (Mirrors 'cpm' 'hookshot-v2.1.0-dav1d-1.5.4' '4eeae3a8d50118dd5c15f2c18dcdc452fbf1dfafceeffcad3bca97aadffe829b') },
    @{ name = 'third-party'; file = 'longshot-third-party-f6fabdbd9436.zip'; sha = 'f6fabdbd94360f28761aa784bde9eb107a77e23a1457a1632f558ce8cdfa859c'; dir = (Join-Path $tools 'third_party'); from = 'third_party'
       urls = (Mirrors 'third-party' 'hookshot-v2.1.0' 'f6fabdbd94360f28761aa784bde9eb107a77e23a1457a1632f558ce8cdfa859c') },
    @{ name = 'openxr'; file = 'OpenXR.Loader.1.1.65.nupkg'; sha = '05638c526f07ba3a5f1084bbc3308d654d5167f844ffa1e35515325cb72e0c34'; dir = (Join-Path $build 'openxr'); from = ''
       urls = @('https://github.com/KhronosGroup/OpenXR-SDK/releases/download/release-1.1.65/OpenXR.Loader.1.1.65.nupkg') }
)

# (Windows' own tar and curl, by their full path: another tar on the PATH, Git's for one, does not unpack zip files)
$tar = Join-Path $env:SystemRoot 'System32\tar.exe'
$curl = Join-Path $env:SystemRoot 'System32\curl.exe'
if (-not (Test-Path $tar) -or -not (Test-Path $curl)) { throw 'Windows 10 (1803 or newer) or Windows 11 is needed: tar.exe and curl.exe were not found in System32.' }

function Say([string]$t) { Write-Host ''; Write-Host "== $t" -ForegroundColor Cyan }
function Fetch([string[]]$urls, [string]$file) {
    foreach ($url in $urls) {   # (the mirrors in order: the next one only when a download fails)
        & $curl -L --fail --silent --show-error --retry 3 -o $file $url
        if ($LASTEXITCODE -eq 0) { return }
        Remove-Item -LiteralPath $file -ErrorAction SilentlyContinue
    }
    throw "download failed: $($urls -join ' ; ')"
}
function Unpack([string]$file, [string]$dir) {
    New-Item -ItemType Directory -Force $dir | Out-Null
    & $tar -xf $file -C $dir
    if ($LASTEXITCODE -ne 0) { throw "could not unpack $file" }
}

if (-not $CoreOnly -and -not $Game -and -not ($Romfs -and $Exefs)) {
    # (started by a double-click: the game file is asked for in a window)
    Add-Type -AssemblyName System.Windows.Forms
    $dlg = New-Object System.Windows.Forms.OpenFileDialog
    $dlg.Title = 'OcarinaVR: choose YOUR copy of the game (.nup file, version 1.1.1)'
    $dlg.Filter = 'Game file (*.nup)|*.nup|All files (*.*)|*.*'
    if ($dlg.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { throw 'No game file chosen. See README.md.' }
    $Game = $dlg.FileName
    if (-not $SwordHand) {
        $ask = [System.Windows.Forms.MessageBox]::Show("Which hand holds the sword?`n`nYes: your RIGHT hand (most players).`nNo: your LEFT hand, as Link himself holds it.`n`nYou can change it later in game" + [char]92 + "retraced.ini (vr_left_handed).",
                                                       'OcarinaVR: sword hand', 'YesNo', 'Question')
        $SwordHand = if ($ask -eq [System.Windows.Forms.DialogResult]::No) { 'left' } else { 'right' }
    }
}
if ($Game -and -not (Test-Path -LiteralPath $Game)) { throw "game file not found: $Game" }
New-Item -ItemType Directory -Force $dl | Out-Null

Say "Longshot $longshotVersion and its build tools (downloaded once, checked by SHA-256)"
foreach ($p in $parts) {
    $file = Join-Path $dl $p.file
    $done = Join-Path $build ".unpacked-$($p.name)"   # (the SHA-256 of what was unpacked: another one is unpacked again)
    if ((Test-Path $done) -and ((Get-Content -LiteralPath $done) -eq $p.sha) -and (Test-Path $p.dir)) { "  $($p.name): ready"; continue }
    if (-not (Test-Path $file)) { "  $($p.name): downloading"; Fetch $p.urls $file }
    $sha = (Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash.ToLower()
    if ($sha -ne $p.sha) { Move-Item -LiteralPath $file -Destination ($file + '.bad') -Force; throw "$($p.file): wrong SHA-256 ($sha); the file was set aside as .bad, run the installer again" }
    "  $($p.name): unpacking"
    if ($p.from) {   # (only the archive's folder `from` becomes dir, as the Builder's setup does)
        $tmp = Join-Path $build "unpack-$($p.name)"
        if (Test-Path $tmp) { Remove-Item -LiteralPath $tmp -Recurse -Force }
        Unpack $file $tmp
        $src = Join-Path $tmp $p.from
        if (-not (Test-Path $src)) { throw "$($p.file): no folder $($p.from) inside" }
        if (Test-Path $p.dir) { Copy-Item (Join-Path $src '*') $p.dir -Recurse -Force } else { Move-Item -LiteralPath $src -Destination $p.dir }
        Remove-Item -LiteralPath $tmp -Recurse -Force
    } else {
        Unpack $file $p.dir
    }
    Set-Content -LiteralPath $done -Value $p.sha -Encoding ascii
}
$py = Join-Path $tools 'python\python.exe'
$mingw = Join-Path $tools 'llvm-mingw'
$cpm = Join-Path $tools 'cpm'
$xr = Join-Path $build 'openxr'
foreach ($need in (Join-Path $tools 'nx2\make_game.py'), $py, (Join-Path $mingw 'bin'), $cpm, (Join-Path $xr 'include')) {
    if (-not (Test-Path $need)) { throw "a build tool is not where it was expected: $need" }
}

if (-not $CoreOnly) {
    Say 'The VR mode''s hook lines'
    & (Join-Path $root 'tools\apply-hooks.ps1') -Longshot $tools
    if (-not $?) { throw 'the hook lines could not be put in' }

    Say 'Building the game from your copy (10 to 45 minutes; you can use your PC meanwhile)'
    # (as the Longshot Builder runs it, builder/LongshotBuilder/Runner.cs: its own compiler, headers and Python, no SDK
    # folder of the PC's)
    $env:PYTHONIOENCODING = 'utf-8'
    $env:LONGSHOT_MINGW = $mingw
    $env:RX_CPM = $cpm
    $env:LONGSHOT_KEEP_BUILD_FILES = '1'   # (a later build compiles only what changed)
    $env:LONGSHOT_NO_UPDATE_CHECK = '1'
    foreach ($v in 'LONGSHOT_NGX_SDK', 'LONGSHOT_FFX_SDK', 'LONGSHOT_XESS_SDK', 'LONGSHOT_MSVC') { Remove-Item "env:$v" -ErrorAction SilentlyContinue }
    $a = @('-I', 'nx2\make_game.py', '--work', (Join-Path $build 'work'), '--log', (Join-Path $build 'build.log'), '--out', $out, '--language', $Language)
    if ($Game) { $a += @('--nup', (Resolve-Path -LiteralPath $Game).Path) } else { $a += @('--romfs', (Resolve-Path -LiteralPath $Romfs).Path, '--exefs', (Resolve-Path -LiteralPath $Exefs).Path) }
    Push-Location $tools
    try { & $py @a; $code = $LASTEXITCODE } finally { Pop-Location }
    if ($code -ne 0) { throw "the game did not build (see _build\build.log)" }
}
if (-not (Test-Path (Join-Path $out 'longshot.exe'))) { throw "no game in $out yet: run the installer with your game first" }

Say 'The VR mode itself (retraced.dll)'
$vk = Get-ChildItem (Join-Path $cpm 'vulkanheaders') -Directory | Sort-Object Name | Select-Object -Last 1
& (Join-Path $root 'tools\build-core.ps1') -Mingw $mingw -Vulkan (Join-Path $vk.FullName 'include') -OpenXR $xr -Out $out
if ($LASTEXITCODE -ne 0) { throw 'retraced.dll did not build' }
if ($SwordHand) {   # (the sword hand chosen: written in the game's settings file)
    $ini = Join-Path $out 'retraced.ini'
    $left = if ($SwordHand -eq 'left') { 1 } else { 0 }
    $lines = @(Get-Content -LiteralPath $ini | Where-Object { $_ -notmatch '^\s*vr_left_handed\s*=' })
    $lines += "vr_left_handed = $left  ; 1: the sword in your left hand, as Link holds it, the shield on your right arm"
    Set-Content -LiteralPath $ini -Value $lines -Encoding ascii
    "sword hand: $SwordHand"
}

Say "Link's arms and skeleton, read from your game's files"
$env:RETRACED_GAME_DIR = $out
& $py -I (Join-Path $root 'tools\vr-corps.py')
if ($LASTEXITCODE -ne 0) { throw 'tools\vr-corps.py failed' }

Say 'Done'
'Put your headset on, start its PC link (Quest Link, SteamVR...), then double-click "Play OcarinaVR.bat".'
