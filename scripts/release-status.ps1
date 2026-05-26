<#
.SYNOPSIS
    Show the status of the most recent release build (or a specific run).

.DESCRIPTION
    Polls the public GitHub API (no auth required for a public repo) and prints
    the current state of the Release Windows .exe workflow's latest run. Useful
    when you don't have the gh CLI installed.

.PARAMETER RunId
    Specific workflow run ID to inspect. Defaults to the most recent run.

.PARAMETER Watch
    Re-poll every 30 seconds until the run completes (or you Ctrl-C).

.EXAMPLE
    ./scripts/release-status.ps1
    ./scripts/release-status.ps1 -Watch
    ./scripts/release-status.ps1 -RunId 26424883317
#>
[CmdletBinding()]
param(
    [long]$RunId,
    [switch]$Watch
)

$ErrorActionPreference = 'Stop'

$Repo = 'claynicholson/kicad-copper'
$WorkflowFile = 'release.yml'

function Get-LatestRun {
    $url = "https://api.github.com/repos/$Repo/actions/workflows/$WorkflowFile/runs?per_page=1"
    $resp = Invoke-RestMethod -Uri $url -Headers @{ 'Accept' = 'application/vnd.github+json' }
    return $resp.workflow_runs[0]
}

function Get-Run([long]$Id) {
    $url = "https://api.github.com/repos/$Repo/actions/runs/$Id"
    return Invoke-RestMethod -Uri $url -Headers @{ 'Accept' = 'application/vnd.github+json' }
}

function Get-Jobs([long]$Id) {
    $url = "https://api.github.com/repos/$Repo/actions/runs/$Id/jobs"
    $resp = Invoke-RestMethod -Uri $url -Headers @{ 'Accept' = 'application/vnd.github+json' }
    return $resp.jobs
}

function Show-Run($run) {
    Clear-Host
    Write-Host "kicad-copper release build" -ForegroundColor Cyan
    Write-Host "Run #$($run.run_number) on $($run.head_branch)"
    Write-Host "URL: $($run.html_url)"
    Write-Host "Status: $($run.status)  Conclusion: $($run.conclusion)"
    Write-Host "Started: $($run.run_started_at)"
    Write-Host ""

    $jobs = Get-Jobs $run.id
    foreach ($j in $jobs) {
        Write-Host "Job: $($j.name) [$($j.status) / $($j.conclusion)]" -ForegroundColor Yellow
        foreach ($s in $j.steps) {
            $color = switch ($s.conclusion) {
                'success'    { 'Green' }
                'failure'    { 'Red' }
                'cancelled'  { 'DarkGray' }
                'skipped'    { 'DarkGray' }
                default      { 'White' }
            }
            $glyph = switch ($s.status) {
                'completed'   { '[*]' }
                'in_progress' { '...' }
                default       { '   ' }
            }
            Write-Host ("  {0} {1,-12} {2}" -f $glyph, $s.status, $s.name) -ForegroundColor $color
        }
        Write-Host ""
    }

    if ($run.conclusion -eq 'success') {
        $tag = $run.head_branch
        Write-Host "Release page: https://github.com/$Repo/releases/tag/$tag" -ForegroundColor Green
    } elseif ($run.conclusion -eq 'failure') {
        Write-Host "Build FAILED. Open the URL above and inspect the failing step." -ForegroundColor Red
    }
}

$run = if ($RunId) { Get-Run $RunId } else { Get-LatestRun }

if ($Watch) {
    while ($true) {
        Show-Run $run
        if ($run.status -eq 'completed') { break }
        Start-Sleep -Seconds 30
        $run = Get-Run $run.id
    }
} else {
    Show-Run $run
}
