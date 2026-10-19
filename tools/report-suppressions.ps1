param(
    [Parameter(Mandatory)][string]$Report,
    [string]$Output = 'out/reports/suppressed-functions.md'
)
$ErrorActionPreference = 'Stop'
$taskAudit = Get-Content -LiteralPath $Report -Raw | ConvertFrom-Json
if (!$taskAudit.PSObject.Properties['suppressed_functions']) {
    throw 'This runtime report predates the suppression inventory.'
}
function Escape-Cell([string]$Value) {
    return $Value.Replace('|', '\|').Replace("`r", ' ').Replace("`n", ' ')
}
$taskLines = [System.Collections.Generic.List[string]]::new()
$taskLines.Add('# Suppressed function inventory')
$taskLines.Add('')
$taskLines.Add('Generated from `' + [System.IO.Path]::GetFileName($Report) + '` for `' + $taskAudit.package + '` version ' + $taskAudit.version + '.')
$taskLines.Add('')
$taskLines.Add('These are explicit disabled bindings, not unresolved imports. Calls count only this session; zero means installed but unused. Local setup, cleanup and preference/storage implementations remain active. No paid item or ad reward is fabricated.')
$taskLines.Add('')
$taskLines.Add('The implementation audit and compatibility boundaries are in [offline-compatibility.md](offline-compatibility.md). Runtime JSON also includes the exact importing libraries and binding status under `native_imports`.')
foreach ($taskGroup in ($taskAudit.suppressed_functions | Group-Object category | Sort-Object Name)) {
    $taskLines.Add('')
    $taskLines.Add('## ' + $taskGroup.Name + ' (' + $taskGroup.Count + ')')
    $taskLines.Add('')
    $taskLines.Add('| Function / Java signature | Behavior | Calls |')
    $taskLines.Add('| --- | --- | --- |')
    foreach ($taskItem in ($taskGroup.Group | Sort-Object name)) {
        $taskLines.Add('| `' + (Escape-Cell $taskItem.name) + '` | ' + (Escape-Cell $taskItem.behavior) + ' | ' + $taskItem.calls + ' |')
    }
}
$taskOutputPath = [System.IO.Path]::GetFullPath($Output)
[System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($taskOutputPath)) | Out-Null
[System.IO.File]::WriteAllLines($taskOutputPath, $taskLines, [System.Text.UTF8Encoding]::new($false))
Write-Output ('Wrote {0} explicit suppression bindings to {1}' -f @($taskAudit.suppressed_functions).Count, $taskOutputPath)
