param(
    [string]$Executable = 'out/build/release/performance_benchmarks.exe',
    [Parameter(Mandatory)][string]$Out,
    [Parameter(Mandatory)][string]$Label,
    [ValidateRange(1, 20)][int]$Runs = 3,
    [switch]$Graphics
)
$ErrorActionPreference = 'Stop'
$executablePath = (Resolve-Path -LiteralPath $Executable).Path
$outputPath = [IO.Path]::GetFullPath($Out)
[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($outputPath)) | Out-Null
$reports = @()
for ($run = 1; $run -le $Runs; ++$run) {
    $runPath = "$outputPath.run-$run.json"
    $arguments = @($runPath, $Label)
    if ($Graphics) { $arguments += '--graphics' }
    & $executablePath @arguments | Out-File -LiteralPath "$runPath.log" -Encoding utf8
    if ($LASTEXITCODE) { throw "Benchmark run $run failed; inspect $runPath.log" }
    $reports += Get-Content -LiteralPath $runPath -Raw | ConvertFrom-Json -AsHashtable
}
$report = $reports[0]
$report.executable_sha256 = (Get-FileHash -LiteralPath $executablePath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($report.ContainsKey('graphics')) {
    $report.graphics.dll_sha256 = [ordered]@{}
    foreach ($dll in @('libGLESv2.dll', 'libEGL.dll')) {
        $dllPath = Join-Path $report.graphics.directory $dll
        $report.graphics.dll_sha256[$dll] = (Get-FileHash -LiteralPath $dllPath -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
$report.run_count = $Runs
$report.measured_samples *= $Runs
$report.warmup_samples *= $Runs
foreach ($name in @($report.cases.Keys)) {
    $samples = @($reports | ForEach-Object { $_.cases[$name].samples_ns_per_operation })
    $ordered = @($samples | Sort-Object)
    $case = $report.cases[$name]
    $case.samples_ns_per_operation = $samples
    $middle = [int][Math]::Floor($ordered.Count / 2)
    $median = $ordered[$middle]
    if ($ordered.Count % 2 -eq 0) { $median = ($ordered[$middle - 1] + $median) / 2 }
    $case.median_ns_per_operation = $median
    $case.min_ns_per_operation = $ordered[0]
    $case.max_ns_per_operation = $ordered[-1]
}
$report | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $outputPath -Encoding utf8
Write-Output "Recorded $Runs warmed runs in $outputPath"
