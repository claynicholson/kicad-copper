# Vendor the bundled Copper KiCad plugins at their pinned refs (PowerShell).
#
# Clones each plugin into resources/copper/plugins/<dir>/ at its pinned ref,
# strips the nested .git, and keeps the upstream LICENSE. Run before packaging a
# release (see docs/DISTRIBUTION.md). Mirrors scripts/fetch-bundled-plugins.py.
#
# On network failure a placeholder README is written into each <dir>/ so the
# install layout stays correct, and the script exits non-zero.

$ErrorActionPreference = 'Stop'

$Plugins = @(
    @{ Dir = 'jlcpcb-tools'; Repo = 'https://github.com/Bouni/kicad-jlcpcb-tools';            Ref = '2026.04.03'; License = 'MIT' },
    @{ Dir = 'ibom';         Repo = 'https://github.com/openscopeproject/InteractiveHtmlBom'; Ref = 'v2.9.0';     License = 'MIT' },
    @{ Dir = 'round-tracks'; Repo = 'https://github.com/mitxela/kicad-round-tracks';          Ref = '50374f8';    License = 'GPL-3.0-or-later' }
)

$RepoRoot = Split-Path -Parent $PSScriptRoot
$Staging  = Join-Path $RepoRoot 'resources/copper/plugins'
New-Item -ItemType Directory -Force -Path $Staging | Out-Null

function Write-Placeholder($Dest, $P) {
    New-Item -ItemType Directory -Force -Path $Dest | Out-Null
    @"
# $($P.Dir) (not vendored)

This plugin was NOT fetched (network unavailable when the fetch script ran).

- Repo: $($P.Repo)
- Pinned ref: $($P.Ref)
- License: $($P.License)

Re-run scripts/fetch-bundled-plugins.ps1 (or the .py) with network access before
packaging a release.
"@ | Set-Content -Encoding UTF8 (Join-Path $Dest 'README.placeholder.md')
}

$ok = 0
foreach ($P in $Plugins) {
    $Dest = Join-Path $Staging $P.Dir
    Write-Host "[$($P.Dir)] $($P.Repo)@$($P.Ref)"

    if (Test-Path $Dest) { Remove-Item -Recurse -Force $Dest }

    git clone --quiet $P.Repo $Dest
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "clone failed; writing placeholder for $($P.Dir)"
        Write-Placeholder $Dest $P
        continue
    }

    git -C $Dest checkout --quiet $P.Ref
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "checkout $($P.Ref) failed; writing placeholder for $($P.Dir)"
        Remove-Item -Recurse -Force $Dest
        Write-Placeholder $Dest $P
        continue
    }

    Remove-Item -Recurse -Force (Join-Path $Dest '.git') -ErrorAction SilentlyContinue
    foreach ($junk in '.github', '.gitignore', '.gitattributes') {
        Remove-Item -Recurse -Force (Join-Path $Dest $junk) -ErrorAction SilentlyContinue
    }

    $hasLicense = @('LICENSE','LICENSE.txt','LICENSE.md','COPYING') |
        Where-Object { Test-Path (Join-Path $Dest $_) }
    if ($hasLicense) { Write-Host '  ok (LICENSE present)' }
    else { Write-Host '  ok (WARNING: no LICENSE file found)' }
    $ok++
}

Write-Host "`nVendored $ok/$($Plugins.Count) plugins into $Staging"
if ($ok -ne $Plugins.Count) {
    Write-Error 'One or more plugins were NOT vendored. Re-run with network access before packaging.'
    exit 1
}
