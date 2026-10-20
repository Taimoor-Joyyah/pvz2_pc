param(
    [Parameter(Mandatory)][string]$Baseline,
    [Parameter(Mandatory)][string]$Candidate,
    [Parameter(Mandatory)][string]$Out
)
$ErrorActionPreference = 'Stop'
$before = Get-Content -LiteralPath $Baseline -Raw | ConvertFrom-Json -AsHashtable
$after = Get-Content -LiteralPath $Candidate -Raw | ConvertFrom-Json -AsHashtable
foreach ($field in @('schema', 'compiler', 'architecture', 'cpu', 'logical_processors', 'fixture_sha256', 'warmup_samples', 'measured_samples', 'scope')) {
    if ($before[$field] -ne $after[$field]) { throw "Incompatible benchmark metadata: $field" }
}
$beforeNames = ($before.cases.Keys | Sort-Object) -join ','
$afterNames = ($after.cases.Keys | Sort-Object) -join ','
if ($beforeNames -cne $afterNames) {
    throw 'Benchmark case sets differ.'
}
if ($before.ContainsKey('graphics') -ne $after.ContainsKey('graphics')) { throw 'Graphics measurement modes differ.' }
if ($before.ContainsKey('graphics')) {
    foreach ($field in @('vendor', 'renderer', 'version')) {
        if ($before.graphics[$field] -ne $after.graphics[$field]) { throw "Graphics backend differs: $field" }
    }
    foreach ($dll in @('libGLESv2.dll', 'libEGL.dll')) {
        if (!$before.graphics.dll_sha256[$dll] -or
            $before.graphics.dll_sha256[$dll] -ne $after.graphics.dll_sha256[$dll]) {
            throw "Graphics DLL hashes differ or are missing: $dll"
        }
    }
}
$cases = [ordered]@{}
foreach ($name in ($before.cases.Keys | Sort-Object)) {
    $old = $before.cases[$name]
    $new = $after.cases[$name]
    if ($old.operations_per_sample -ne $new.operations_per_sample) { throw "Operation counts differ: $name" }
    if ($old.median_ns_per_operation -le 0 -or $new.median_ns_per_operation -le 0) { throw "Invalid timing: $name" }
    $cases[$name] = [ordered]@{
        baseline_ns = $old.median_ns_per_operation
        candidate_ns = $new.median_ns_per_operation
        change_percent = 100 * ($new.median_ns_per_operation / $old.median_ns_per_operation - 1)
        speedup = $old.median_ns_per_operation / $new.median_ns_per_operation
    }
}
$report = [ordered]@{
    schema = 1
    baseline = $before.label
    candidate = $after.label
    scope = $before.scope
    note = 'Medians of warmed synthetic samples. Negative change means less time. No gameplay speedup is implied.'
    cases = $cases
}
$outputPath = [IO.Path]::GetFullPath($Out)
[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($outputPath)) | Out-Null
$report | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $outputPath -Encoding utf8
foreach ($name in $cases.Keys) {
    $row = $cases[$name]
    '{0,-30} {1,10:N1} -> {2,10:N1} ns  {3,8:N1}%  {4,6:N2}x' -f $name, $row.baseline_ns, $row.candidate_ns, $row.change_percent, $row.speedup
}
