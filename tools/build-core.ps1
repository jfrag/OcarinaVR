# Builds our core (src\*.cpp) into retraced.dll, in a few seconds, with the same compiler as Longshot (llvm-mingw).
#   -Mingw   : the llvm-mingw folder
#   -Vulkan  : a folder with Khronos's Vulkan headers (the "include" folder of Longshot's cpm tool)
#   -OpenXR  : the unpacked OpenXR loader package of Khronos (include\, native\x64\release\bin\openxr_loader.dll)
#   -Out     : the game folder the DLL goes to
#   -Glslang : optional, glslang.exe: the shaders (src\shaders\*.comp) are compiled again into src\gen\*.h.
#              Without it the headers shipped in src\gen are used as they are.
param(
    [Parameter(Mandatory = $true)][string]$Mingw,
    [Parameter(Mandatory = $true)][string]$Vulkan,
    [Parameter(Mandatory = $true)][string]$OpenXR,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Glslang = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$src = Join-Path $root 'src'
$cxx = Join-Path $Mingw 'bin\x86_64-w64-mingw32-clang++.exe'
if (-not (Test-Path $cxx)) { $cxx = Join-Path $Mingw 'bin\clang++.exe' }
if (-not (Test-Path $cxx)) { throw "the compiler was not found in $Mingw" }
$obj = Join-Path $root '_build\core-obj'
New-Item -ItemType Directory -Force $obj, $Out | Out-Null

$stamp = (Get-Date -Format 'yyyyMMdd-HHmmss')
$flags = @('-std=c++20', '-O2', '-g0', '-DNDEBUG', '-DVK_NO_PROTOTYPES', "-DRT_CORE_BUILD_ID=public-$stamp",
    "-I`"$src`"", "-I`"$Vulkan`"", "-I`"$(Join-Path $OpenXR 'include')`"", '-Wall', '-Wno-unused-function', '-march=x86-64-v3')

$t0 = Get-Date
if ($Glslang) {
    $gen = Join-Path $src 'gen'
    foreach ($sh in Get-ChildItem (Join-Path $src 'shaders') -Filter *.comp) {
        $name = $sh.BaseName + '_comp'
        $ErrorActionPreference = 'Continue'
        $msg = & $Glslang -V --target-env vulkan1.3 --vn $name $sh.FullName -o (Join-Path $gen ($name + '.h')) 2>&1 | Out-String
        $ErrorActionPreference = 'Stop'
        if ($LASTEXITCODE -ne 0) { $msg; "FAILED: shader $($sh.Name)"; exit 1 }
    }
}
$objs = @()
$fail = $false
$ErrorActionPreference = 'Continue'   # (the compiler's warnings come on stderr)
foreach ($f in Get-ChildItem $src -Filter *.cpp) {
    $o = Join-Path $obj ($f.BaseName + '.o')
    $objs += $o
    $msg = & $cxx @flags -c $f.FullName -o $o 2>&1 | Out-String
    if ($msg.Trim()) { $msg }
    if ($LASTEXITCODE -ne 0) { $fail = $true }
}
if ($fail) { 'FAILED: the core did not compile'; exit 1 }
$dll = Join-Path $Out 'retraced.dll'
$msg = & $cxx -shared -static -o $dll @objs -luser32 2>&1 | Out-String
if ($msg.Trim()) { $msg }
if ($LASTEXITCODE -ne 0) { 'FAILED: the core did not link'; exit 1 }
$ErrorActionPreference = 'Stop'
Copy-Item (Join-Path $OpenXR 'native\x64\release\bin\openxr_loader.dll') $Out -Force
$ini = Join-Path $Out 'retraced.ini'
if (-not (Test-Path $ini)) { Copy-Item (Join-Path $root 'config\retraced.ini') $ini }
"retraced.dll: {0} ({1:N0} KB) in {2:N1} s" -f $dll, ((Get-Item $dll).Length / 1KB), ((Get-Date) - $t0).TotalSeconds
