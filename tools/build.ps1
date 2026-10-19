param([ValidateSet('release','debug')][string]$Configuration = 'release', [switch]$Test, [switch]$ProfileSymbols)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio C++ desktop build tools.' }
$vsPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vsPath) { throw 'No x64 MSVC toolchain found.' }
$devShell = Join-Path $vsPath 'Common7\Tools\Launch-VsDevShell.ps1'
& $devShell -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
if (!(Get-Command ninja -ErrorAction SilentlyContinue)) {
    $env:PATH = (Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja') + ';' + $env:PATH
}
Push-Location -LiteralPath $projectRoot
try {
    $profileValue = if ($ProfileSymbols) { 'ON' } else { 'OFF' }
    cmake --preset "windows-$Configuration" "-DACP_PROFILE_SYMBOLS=$profileValue"
    if ($LASTEXITCODE) { throw 'CMake configure failed.' }
    cmake --build --preset "windows-$Configuration" --parallel 8
    if ($LASTEXITCODE) { throw 'Build failed.' }
    if ($Test) {
        ctest --test-dir "out/build/$Configuration" --output-on-failure
        if ($LASTEXITCODE) { throw 'Tests failed.' }
    }
} finally { Pop-Location }
