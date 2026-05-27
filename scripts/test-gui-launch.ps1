# Launches the locally-built kicad.exe and verifies the process stays alive
# (which means the GUI window came up — exit-on-launch usually = missing DLL
# or fatal init error).
$ErrorActionPreference = 'Stop'

$exe = 'C:\Users\claya\Projects\kicad-copper\build\release\staging\bin\kicad.exe'
if (-not (Test-Path $exe)) { throw "Not found: $exe" }

Write-Host "Launching $exe ..."
$ps = Start-Process -FilePath $exe -PassThru -WindowStyle Normal
Write-Host "  Started PID $($ps.Id) at $(Get-Date -Format 'HH:mm:ss')"
Start-Sleep -Seconds 10

$running = Get-Process -Id $ps.Id -ErrorAction SilentlyContinue
if ($running) {
    $title = $running.MainWindowTitle
    $ws    = [math]::Round($running.WorkingSet / 1MB, 1)
    $cpu   = [math]::Round($running.CPU, 2)
    Write-Host ""
    Write-Host "OK  PID $($ps.Id) still running after 10s" -ForegroundColor Green
    Write-Host "    MainWindowTitle: $title"
    Write-Host "    WorkingSet: $ws MB"
    Write-Host "    CPU: $cpu s"
    Write-Host ""
    Write-Host "(Leaving process running for you to inspect. Close the window or run Stop-Process -Id $($ps.Id) to terminate.)"
} else {
    Write-Host ""
    Write-Host "FAIL  PID $($ps.Id) EXITED within 10s" -ForegroundColor Red
    Write-Host "      ExitCode: $($ps.ExitCode)"
    Write-Host "      Most likely cause: missing DLL or fatal init error"
}
