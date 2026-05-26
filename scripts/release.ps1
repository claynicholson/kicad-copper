<#
.SYNOPSIS
    Cut a new kicad-copper release.

.DESCRIPTION
    Bumps the VERSION file, commits, tags v<version>, and pushes both the
    commit and the tag. The Release Windows .exe GitHub Actions workflow
    (see .github/workflows/release.yml) takes over from there: it builds the
    installer and publishes the GitHub Release with the .exe + .zip attached.

.PARAMETER Version
    Semantic version to release (e.g. 0.1.0). No leading "v".

.PARAMETER Remote
    Git remote to push to. Defaults to "origin".

.PARAMETER DryRun
    Print what would happen without changing anything.

.PARAMETER Force
    Allow release even if the working tree is dirty or the tag already exists.

.EXAMPLE
    ./scripts/release.ps1 0.1.0

.EXAMPLE
    ./scripts/release.ps1 0.2.0 -DryRun
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Version,

    [string]$Remote = "origin",

    [switch]$DryRun,

    [switch]$Force
)

$ErrorActionPreference = 'Stop'

function Step($msg) {
    Write-Host ""
    Write-Host "==> $msg" -ForegroundColor Cyan
}

function Run($cmd) {
    Write-Host "    $cmd" -ForegroundColor DarkGray
    if (-not $DryRun) {
        Invoke-Expression $cmd
        if ($LASTEXITCODE -ne 0) {
            throw "Command failed (exit $LASTEXITCODE): $cmd"
        }
    }
}

# Normalise: accept "v0.1.0" too, strip the v.
$Version = $Version.TrimStart('v').Trim()
if ($Version -notmatch '^\d+\.\d+\.\d+(?:-[0-9A-Za-z\.\-]+)?$') {
    throw "Invalid version '$Version'. Expected semver like 0.1.0 or 0.1.0-rc.1."
}
$Tag = "v$Version"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
Push-Location $repoRoot
try {
    Step "Pre-flight checks"
    Run "git rev-parse --is-inside-work-tree"

    # Working tree must be clean unless -Force.
    $status = & git status --porcelain 2>$null
    if ($status -and -not $Force) {
        Write-Host $status -ForegroundColor Yellow
        throw "Working tree is dirty. Commit or stash first, or pass -Force."
    }

    # Tag must not already exist unless -Force.
    $existingTag = & git tag --list $Tag 2>$null
    if ($existingTag -and -not $Force) {
        throw "Tag '$Tag' already exists. Use a new version, or pass -Force to overwrite."
    }

    # Confirm the remote is reachable so we don't bump the file and then fail.
    Run "git fetch $Remote --tags --prune"

    Step "Update VERSION file"
    $versionFile = Join-Path $repoRoot "VERSION"
    $current = (Get-Content $versionFile -Raw).Trim()
    Write-Host "    Current: $current"
    Write-Host "    New:     $Version"
    if (-not $DryRun) {
        Set-Content -Path $versionFile -Value "$Version`n" -NoNewline
    }

    Step "Commit + tag"
    Run "git add VERSION"

    $alreadyClean = $false
    if (-not $DryRun) {
        $diff = & git diff --cached --quiet
        if ($LASTEXITCODE -eq 0) {
            $alreadyClean = $true
            Write-Host "    VERSION unchanged from $current; skipping commit." -ForegroundColor Yellow
        }
    }
    if (-not $alreadyClean) {
        Run "git commit -m `"Release $Tag`""
    }

    if ($Force -and $existingTag) {
        Run "git tag -d $Tag"
    }
    Run "git tag -a $Tag -m `"kicad-copper $Tag`""

    Step "Push"
    Run "git push $Remote HEAD"
    Run "git push $Remote $Tag"

    Step "Done"
    Write-Host "Pushed $Tag to $Remote. The release workflow will build and publish the .exe." -ForegroundColor Green
    $remoteUrl = (& git remote get-url $Remote 2>$null)
    if ($remoteUrl -match 'github\.com[:/](.+?)(?:\.git)?$') {
        $slug = $matches[1]
        Write-Host ""
        Write-Host "Watch the build:   https://github.com/$slug/actions" -ForegroundColor Green
        Write-Host "Release will land: https://github.com/$slug/releases/tag/$Tag" -ForegroundColor Green
    }
}
finally {
    Pop-Location
}
