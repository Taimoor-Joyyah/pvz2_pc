param([string]$SourceDirectory, [string]$DestinationDirectory, [switch]$AllowDifferentBuild)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (!$SourceDirectory) { $SourceDirectory = Join-Path $env:LOCALAPPDATA 'Android\Sdk\emulator\lib64\gles_angle' }
if (!$DestinationDirectory) { $DestinationDirectory = Join-Path $projectRoot 'out\build\release\angle' }
$locked = Get-Content -LiteralPath (Join-Path $projectRoot 'cmake\angle-windows.lock.json') -Raw | ConvertFrom-Json
$records = @()
foreach ($file in $locked.files.PSObject.Properties) {
    $sourceFile = Join-Path $SourceDirectory $file.Name
    if (!(Test-Path -LiteralPath $sourceFile -PathType Leaf)) { throw "Missing ANGLE component: $sourceFile" }
    $hash = (Get-FileHash -LiteralPath $sourceFile -Algorithm SHA256).Hash.ToLowerInvariant()
    if (!$AllowDifferentBuild -and $hash -ne $file.Value) { throw "ANGLE component hash differs from the tested build: $($file.Name). Use -AllowDifferentBuild explicitly to test another build." }
    $records += [PSCustomObject]@{ name = $file.Name; sha256 = $hash; source = $sourceFile }
}
New-Item -ItemType Directory -Path $DestinationDirectory -Force | Out-Null
foreach ($record in $records) { Copy-Item -LiteralPath $record.source -Destination (Join-Path $DestinationDirectory $record.name) -Force }
@{ source = $locked.source; components = $records } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $DestinationDirectory 'angle-build.json') -Encoding utf8
Write-Output "Packaged standalone ANGLE DLLs in $DestinationDirectory"
