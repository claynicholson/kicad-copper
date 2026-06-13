<#
.SYNOPSIS
    Build kicad-copper locally and publish a GitHub Release.

.DESCRIPTION
    One-command local build pipeline. Does configure -> build -> stage ->
    package (.exe + .zip) -> upload to a GitHub Release. Idempotent: skips
    steps whose outputs already exist. Pass -Clean to force a from-scratch
    build.

    This is the canonical release flow. The GitHub Actions CI workflow at
    .github/workflows/release.yml is kept as a fallback for contributors who
    don't have a local MSYS2 toolchain, but this local path is the supported
    one.

.PARAMETER Version
    Version to release (e.g. 0.1.1). No leading "v". Defaults to VERSION.txt.

.PARAMETER Clean
    Wipe build/release first and reconfigure. ~10-12 minute configure +
    ~30-50 minute build. Without -Clean only changed files are rebuilt.

.PARAMETER SkipUpload
    Build + package locally but don't push to GitHub. Use for testing.

.PARAMETER DryRun
    Show what would happen without actually doing it.

.EXAMPLE
    ./scripts/release-local.ps1                     # use VERSION.txt
    ./scripts/release-local.ps1 0.1.1               # cut 0.1.1
    ./scripts/release-local.ps1 0.1.1 -SkipUpload   # build but don't release
    ./scripts/release-local.ps1 0.2.0 -Clean        # from-scratch rebuild
#>
[CmdletBinding()]
param(
    [string]$Version,
    [switch]$Clean,
    [switch]$SkipUpload,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

# Paths
$repoRoot     = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$buildDir     = Join-Path $repoRoot 'build\release'
$stagingDir   = Join-Path $buildDir 'staging'
$msys2        = 'C:\msys64'
$msysShell    = "$msys2\usr\bin\bash.exe"
$pythonStdlib = "$msys2\ucrt64\lib\python3.14"
$ghRepo       = 'claynicholson/kicad-copper'
$nsisConfig   = Join-Path $buildDir 'CopperNSISConfig.cmake'

# Resolve version
if (-not $Version) {
    $Version = (Get-Content (Join-Path $repoRoot 'VERSION.txt') -Raw).Trim()
}
$Version = $Version.TrimStart('v').Trim()
if ($Version -notmatch '^\d+\.\d+\.\d+(?:-[0-9A-Za-z\.\-]+)?$') {
    throw "Invalid version '$Version'. Expected semver like 0.1.0."
}
$tag = "v$Version"
$exeName = "kicad-copper-$Version-win64.exe"
$zipName = "kicad-copper-$Version-win64-portable.zip"
$exePath = Join-Path $buildDir $exeName
$zipPath = Join-Path $buildDir $zipName

function Step($msg) {
    Write-Host ""
    Write-Host "==> $msg" -ForegroundColor Cyan
}
function Sub($msg) { Write-Host "    $msg" -ForegroundColor DarkGray }
function Run($block, $tag) {
    Sub $tag
    if (-not $DryRun) { & $block }
}

# Invoke a bash command in the MSYS2 UCRT64 environment, passing the env
# vars the login shell strips. Throws on non-zero exit.
#
# The command is written to a temp script and run via `bash -l <script>`
# rather than `bash -lc <string>`: Windows PowerShell 5.1 mangles embedded
# double quotes when passing native-command arguments, which corrupts any
# command containing "$(...)" or quoted paths.
function MsysBash([string]$cmd) {
    $env:MSYSTEM       = 'UCRT64'
    $env:CHERE_INVOKING= '1'

    $tmp = Join-Path $env:TEMP ("msys-cmd-{0}.sh" -f [guid]::NewGuid())
    # bash wants LF line endings and no BOM.
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($tmp, (($cmd -replace "`r`n", "`n") + "`n"), $utf8NoBom)
    $tmpUnix = & "$msys2\usr\bin\cygpath.exe" -u $tmp

    try {
        & $msysShell -l $tmpUnix
        if ($LASTEXITCODE -ne 0) {
            throw "MSYS2 command failed (exit $LASTEXITCODE): $cmd"
        }
    }
    finally {
        Remove-Item $tmp -ErrorAction SilentlyContinue
    }
}

Step "kicad-copper local release - version $Version"
Sub  "repo:        $repoRoot"
Sub  "build dir:   $buildDir"
Sub  "msys2:       $msys2"
Sub  "exe out:     $exeName"
Sub  "zip out:     $zipName"
Sub  "github:      $ghRepo (tag $tag)"
if ($DryRun) { Sub "DRY-RUN mode: no state changes" }

# 0. Sanity
Step "Sanity checks"
if (-not (Test-Path $msysShell))    { throw "MSYS2 not found at $msys2" }
if (-not (Test-Path $pythonStdlib)) { throw "Python stdlib not found at $pythonStdlib (pacman -S mingw-w64-ucrt-x86_64-python)" }
Sub "OK  MSYS2 + Python stdlib present"

# 0b. Vendor bundled plugins into resources/copper/plugins/ so cmake --install
#     stages them into share/kicad/plugins/. No-op-safe to re-run.
Step "Fetch bundled plugins (resources/copper/plugins)"
Run { & $PSScriptRoot\fetch-bundled-plugins.ps1 } "fetch-bundled-plugins.ps1"

# 1. Clean (optional)
if ($Clean) {
    Step "Clean build dir"
    Run { Remove-Item -Recurse -Force $buildDir -ErrorAction SilentlyContinue } "rm -rf $buildDir"
}
if (-not (Test-Path $buildDir)) {
    Run { New-Item -ItemType Directory -Force -Path $buildDir | Out-Null } "mkdir build/release"
}

# 2. CMake configure (skip if already configured)
$cacheFile = Join-Path $buildDir 'CMakeCache.txt'
if (-not (Test-Path $cacheFile)) {
    Step "CMake configure (~10-12 minutes, mostly Python detection)"
    # NOTE: this fork removed the in-process SWIG/wxPython scripting subsystem
    # (commit "REMOVED: SWIG, wxPython, and Python integration", 518 files). There
    # is no KICAD_SCRIPTING_WXPYTHON option in this tree, so it is intentionally not
    # passed (it would be a silently-ignored no-op). Plugins use the out-of-process
    # IPC API model: a `plugin.json` manifest + an external Python interpreter
    # discovered at runtime. See docs/DISTRIBUTION.md.
    $cfgCmd = "export COPPER_VERSION=$Version && cd `"`$(cygpath -u '$buildDir')`" && cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DKICAD_USE_3DCONNEXION=OFF -DKICAD_BUILD_QA_TESTS=OFF -DKICAD_BUILD_I18N=OFF -DKICAD_USE_OCC=ON -DOCC_INCLUDE_DIR=/ucrt64/include/opencascade -DOCC_LIBRARY_DIR=/ucrt64/lib -DKICAD_USE_CMAKE_FINDPROTOBUF=OFF -DKICAD_SPICE=ON -DKICAD_BUILD_PNS_DEBUG_TOOL=OFF ../.."
    Run { MsysBash $cfgCmd } "cmake -G Ninja ..."
} else {
    Sub "CMakeCache.txt exists - skipping configure (use -Clean to force)"
}

# 3. Build (incremental ninja)
Step "Build (incremental: ninja decides what to recompile)"
$buildCmd = "export USERPROFILE='$env:USERPROFILE' && export CCACHE_DIR='$env:USERPROFILE\.ccache' && cd `"`$(cygpath -u '$buildDir')`" && cmake --build . 2>&1 | tee compilation_log.txt && test `${PIPESTATUS[0]} -eq 0"
Run { MsysBash $buildCmd } "cmake --build ."

# 4. Stage (install + DLLs + Python + resources)
Step "Stage install tree (bin/ lib/ share/)"

# 4a. Sed-strip absolute paths from cmake_install.cmake files. KiCad's
#     install() rules bake "C:/Program Files/kicad/..." which CPack rejects
#     and cmake --install --prefix=... ignores.
$sedCmd = "cd `"`$(cygpath -u '$buildDir')`" && find . -name cmake_install.cmake -exec sed -i 's|C:/Program Files/kicad/||g; s|C:/Program Files/kicad||g' {} +"
Run { MsysBash $sedCmd } "sed-strip absolute prefix from cmake_install.cmake files"

# 4b. cmake --install to staging
Run {
    if (Test-Path $stagingDir) { Remove-Item -Recurse -Force $stagingDir }
    $installCmd = "export USERPROFILE='$env:USERPROFILE' && cd `"`$(cygpath -u '$buildDir')`" && cmake --install . --prefix `"`$(cygpath -u '$stagingDir')`" 2>&1 | tail -5"
    MsysBash $installCmd
} "cmake --install --prefix=staging"

# 4c. Copy api/schemas/*.json (cmake install errors before getting to them)
Run {
    $dstSchemas = Join-Path $stagingDir 'share\kicad\schemas'
    New-Item -ItemType Directory -Force -Path $dstSchemas | Out-Null
    Copy-Item "$repoRoot\api\schemas\*.json" $dstSchemas -Force
} "copy api/schemas/*.json -> staging/share/kicad/schemas/"

# 4d. Copy share/kicad/{resources,scripting,template}. KiCad's Windows
#     install rules don't handle these. resources/images.tar.gz is the icon
#     archive that fixes the "?" placeholders in the toolbar.
#     NOTE: demos/ is intentionally NOT copied. A footprint with a 125-char
#     filename overruns Windows MAX_PATH inside NSIS's staging tree.
Run {
    foreach ($d in @('resources','scripting','template')) {
        $src = Join-Path $buildDir "share\kicad\$d"
        $dst = Join-Path $stagingDir "share\kicad\$d"
        if (Test-Path $src) {
            if (Test-Path $dst) { Remove-Item -Recurse -Force $dst }
            Copy-Item -Recurse $src $dst
        }
    }
} "copy build/release/share/kicad/{resources,scripting,template} -> staging/"

# 4e. Transitive DLL copier. Walk objdump -p over every PE in staging/bin/,
#     copy any /ucrt64/bin/ dependency into staging/bin/. Iterate until
#     fixed point.
Step "Resolve and bundle runtime DLLs (objdump-based transitive walker)"
$dllCmd = "cd `"`$(cygpath -u '$stagingDir')`" && round=0 && total=0 && while : ; do round=`$((round + 1)); new=0; imports=`$(for f in bin/*.exe bin/*.dll; do objdump -p `"`$f`" 2>/dev/null | awk '/DLL Name:/ {print `$3}'; done | sort -u); while read -r dll; do [ -z `"`$dll`" ] && continue; [ -e `"bin/`$dll`" ] && continue; if [ -e `"/ucrt64/bin/`$dll`" ]; then cp `"/ucrt64/bin/`$dll`" `"bin/`$dll`"; new=`$((new + 1)); fi; done <<< `"`$imports`"; echo `"  round `$round: copied `$new`"; total=`$((total + new)); [ `"`$new`" -eq 0 ] && break; [ `"`$round`" -gt 12 ] && break; done && echo `"total new DLLs: `$total`""
Run { MsysBash $dllCmd } "objdump-walker /ucrt64/bin -> staging/bin"

# 4f. Python stdlib. kicad.exe embeds Python and needs the stdlib at a
#     predictable relative path.
Step "Bundle Python stdlib"
Run {
    $dst = Join-Path $stagingDir 'lib\python3.14'
    if (-not (Test-Path $dst)) {
        New-Item -ItemType Directory -Force -Path (Join-Path $stagingDir 'lib') | Out-Null
        Copy-Item -Recurse $pythonStdlib $dst
    } else {
        Sub "already present, skipping"
    }
} "copy /ucrt64/lib/python3.14 -> staging/lib/python3.14"

# 5. Package
Step "Package - NSIS installer (.exe)"

# 5a. Write the standalone CPack config that packages the staged tree.
$versionParts = $Version -split '[.-]'
$verMajor = $versionParts[0]
$verMinor = $versionParts[1]
$verPatch = $versionParts[2]
$repoRootSlash = $repoRoot.Replace('\','/')
$stagingDirSlash = $stagingDir.Replace('\','/')

$nsisConfigContent = @"
# Auto-generated by scripts/release-local.ps1. Do not edit by hand.
set(CPACK_GENERATOR                       "NSIS")
set(CPACK_PACKAGE_NAME                    "KiCad Copper")
set(CPACK_PACKAGE_VENDOR                  "kicad-copper")
set(CPACK_PACKAGE_VERSION                 "$Version")
set(CPACK_PACKAGE_VERSION_MAJOR           "$verMajor")
set(CPACK_PACKAGE_VERSION_MINOR           "$verMinor")
set(CPACK_PACKAGE_VERSION_PATCH           "$verPatch")
set(CPACK_PACKAGE_DESCRIPTION
    "KiCad Copper is a fork of KiCad with an embedded Copper chat panel integrated into the schematic editor. Describe a circuit in natural language and the Copper backend generates a clean schematic, applied directly to your live editor.")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "KiCad fork with an embedded Copper chat panel and schematic-generation backend integration.")
set(CPACK_PACKAGE_HOMEPAGE_URL            "https://github.com/$ghRepo")
set(CPACK_PACKAGE_INSTALL_DIRECTORY       "KiCad Copper")
set(CPACK_PACKAGE_FILE_NAME               "kicad-copper-$Version-win64")
set(CPACK_TOPLEVEL_TAG                    "win64")

set(CPACK_RESOURCE_FILE_LICENSE           "$repoRootSlash/LICENSE")
set(CPACK_RESOURCE_FILE_README            "$repoRootSlash/README.md")

set(CPACK_NSIS_PACKAGE_NAME               "KiCad Copper $Version")
set(CPACK_NSIS_DISPLAY_NAME               "KiCad Copper $Version")
set(CPACK_NSIS_INSTALL_ROOT               "`$PROGRAMFILES64")
set(CPACK_NSIS_MODIFY_PATH                ON)
set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
set(CPACK_NSIS_URL_INFO_ABOUT             "https://github.com/$ghRepo")
set(CPACK_NSIS_HELP_LINK                  "https://github.com/$ghRepo")
set(CPACK_NSIS_CONTACT                    "https://github.com/$ghRepo/issues")
set(CPACK_NSIS_EXECUTABLES_DIRECTORY      "bin")
set(CPACK_PACKAGE_EXECUTABLES             "kicad" "KiCad Copper")
set(CPACK_CREATE_DESKTOP_LINKS            "kicad")

# Skip CMake's broken install() machinery; package the staged tree directly.
set(CPACK_INSTALLED_DIRECTORIES
    "$stagingDirSlash;/")
set(CPACK_MONOLITHIC_INSTALL              ON)
"@
Run { Set-Content -Path $nsisConfig -Value $nsisConfigContent -Encoding UTF8 } "write CopperNSISConfig.cmake"

Run {
    if (Test-Path $exePath) { Remove-Item $exePath }
    $cpackCmd = "cd `"`$(cygpath -u '$buildDir')`" && rm -rf _CPack_Packages && cpack --config CopperNSISConfig.cmake 2>&1 | tail -10"
    MsysBash $cpackCmd
} "cpack --config CopperNSISConfig.cmake"

if (-not (Test-Path $exePath)) {
    throw "cpack did not produce $exePath. Check build/release/_CPack_Packages/.../NSISOutput.log"
}
$exeSize = [math]::Round((Get-Item $exePath).Length / 1MB, 1)
Sub "OK  $exeName ($exeSize MB)"

# 5b. Portable zip via .NET ZipFile (Compress-Archive trips on large/locked files)
Step "Package - portable zip"
Run {
    if (Test-Path $zipPath) { Remove-Item $zipPath }
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::CreateFromDirectory(
        $stagingDir, $zipPath,
        [System.IO.Compression.CompressionLevel]::Optimal, $false)
} "ZipFile.CreateFromDirectory(staging, $zipName)"

if (-not (Test-Path $zipPath)) { throw "Portable zip not produced." }
$zipSize = [math]::Round((Get-Item $zipPath).Length / 1MB, 1)
Sub "OK  $zipName ($zipSize MB)"

# 6. Upload to GitHub Release
if ($SkipUpload) {
    Step "Skipping upload (-SkipUpload set)"
    Write-Host "Artifacts ready at: $buildDir" -ForegroundColor Green
    return
}

Step "Push to GitHub Release $tag"

# 6a. Get token from git credential helper (Windows Credential Manager)
$credInput = "protocol=https`nhost=github.com`n`n"
$credOutput = $credInput | & git credential fill 2>&1
$tokenLine = $credOutput | Where-Object { $_ -match '^password=' } | Select-Object -First 1
if (-not $tokenLine) { throw "Could not get GitHub token from git credential helper" }
$token = $tokenLine.Substring(9)

$headers = @{
    'Authorization' = "Bearer $token"
    'Accept'        = 'application/vnd.github+json'
}

# 6b. Ensure tag exists on remote
$existingTag = & git -C $repoRoot ls-remote origin "refs/tags/$tag" 2>&1
if (-not $existingTag) {
    Sub "tag $tag doesn't exist on origin - creating + pushing"
    if (-not $DryRun) {
        & git -C $repoRoot tag -a $tag -m "kicad-copper $tag"
        & git -C $repoRoot push origin $tag
    }
} else {
    Sub "tag $tag already on origin"
}

# 6c. Ensure release exists
$release = $null
try {
    $release = Invoke-RestMethod -Uri "https://api.github.com/repos/$ghRepo/releases/tags/$tag" -Headers $headers -ErrorAction Stop
    Sub "release $tag exists (id=$($release.id))"
} catch {
    Sub "creating release $tag"
    if (-not $DryRun) {
        $body = @{
            tag_name = $tag
            name     = $tag
            body     = "kicad-copper $tag. Built locally on Windows / MSYS2 UCRT64. Includes the Copper chat panel and a bundled Python 3.14 runtime."
            draft    = $false
            prerelease = $false
        } | ConvertTo-Json
        $release = Invoke-RestMethod -Uri "https://api.github.com/repos/$ghRepo/releases" -Method POST -Headers $headers -Body $body -ContentType 'application/json'
        Sub "created release id=$($release.id)"
    }
}

# 6d. Delete any pre-existing assets with the same name (replace, not append)
foreach ($name in @($exeName, $zipName)) {
    $existing = $release.assets | Where-Object { $_.name -eq $name }
    if ($existing) {
        Sub "deleting old asset $name (id=$($existing.id))"
        if (-not $DryRun) {
            Invoke-WebRequest -Uri "https://api.github.com/repos/$ghRepo/releases/assets/$($existing.id)" `
                -Method DELETE -Headers $headers | Out-Null
        }
    }
}

# 6e. Upload both assets
foreach ($pair in @(@($exeName, $exePath), @($zipName, $zipPath))) {
    $name, $path = $pair
    $sizeMb = [math]::Round((Get-Item $path).Length / 1MB, 1)
    Sub "uploading $name ($sizeMb MB)"
    if (-not $DryRun) {
        $uploadUrl = "https://uploads.github.com/repos/$ghRepo/releases/$($release.id)/assets?name=$name"
        $resp = Invoke-RestMethod -Uri $uploadUrl -Method POST -Headers $headers `
                -ContentType 'application/octet-stream' -InFile $path
        Sub "    -> $($resp.browser_download_url) state=$($resp.state)"
    }
}

# Done
Step "Done"
Write-Host "Release: https://github.com/$ghRepo/releases/tag/$tag" -ForegroundColor Green
Write-Host "  exe:   https://github.com/$ghRepo/releases/download/$tag/$exeName" -ForegroundColor Green
Write-Host "  zip:   https://github.com/$ghRepo/releases/download/$tag/$zipName" -ForegroundColor Green
