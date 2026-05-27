# Light-weight status check for the local build (avoids spawning bash forks).
# The script picks the most recently modified .output file in the tasks dir so
# resumed builds (new task IDs) are tracked automatically. Prefer the build log
# inside build/release which is the canonical "ninja stdout" target.
$buildLog = 'C:\Users\claya\Projects\kicad-copper\build\release\compilation_log.txt'
if (Test-Path $buildLog) {
    $log = $buildLog
} else {
    $tasksDir = 'C:\Users\claya\AppData\Local\Temp\claude\C--Users-claya-Projects-kicad-copper\1596abeb-ba77-4fc6-8357-fd3972b7a1ef\tasks'
    $newest = Get-ChildItem $tasksDir -Filter '*.output' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    $log = if ($newest) { $newest.FullName } else { $null }
}
if (-not (Test-Path $log)) { Write-Host "Log not yet created: $log"; exit 0 }

$lines = Get-Content $log
Write-Host "=== last 5 lines ==="
$lines | Select-Object -Last 5

$failed = @($lines | Select-String -Pattern '^FAILED' -SimpleMatch)
Write-Host ""
Write-Host "=== summary ==="
Write-Host "total lines:    $($lines.Count)"
Write-Host "FAILED entries: $($failed.Count)"
$lastProgress = ($lines | Select-String -Pattern '^\[\d+/\d+\]' | Select-Object -Last 1)
if ($lastProgress) { Write-Host "last progress:  $($lastProgress.Line)" }

if ($failed.Count -gt 0) {
    Write-Host ""
    Write-Host "=== first few FAILED ==="
    $failed | Select-Object -First 3 | ForEach-Object { Write-Host $_.Line }
}
