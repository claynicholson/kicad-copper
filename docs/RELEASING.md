# Releasing kicad-copper

This repo ships a Windows `.exe` (NSIS installer) plus a portable `.zip` for
each release. **Releases are built locally and pushed to a GitHub Release** on
`claynicholson/kicad-copper`. The CI workflow (`.github/workflows/release.yml`)
exists as a fallback for contributors without an MSYS2 toolchain but is not the
supported path.

## TL;DR

From a Windows machine with MSYS2 UCRT64 set up the way KiCad upstream
documents:

```powershell
./scripts/release-local.ps1 0.1.1
```

That's it. One command. Configure -> build -> stage -> package -> upload.

The script is idempotent: if you re-run it, steps with existing outputs are
skipped. To force a from-scratch rebuild, pass `-Clean`.

## What it does

[`scripts/release-local.ps1`](../scripts/release-local.ps1) executes the full
pipeline:

1. **Sanity** - verify MSYS2 + Python stdlib are installed.
2. **CMake configure** if `build/release/CMakeCache.txt` doesn't exist
   (~10-12 minute Python detection on first run; subsequent runs skip).
3. **Build** with Ninja + ccache (incremental; minutes on a warm tree,
   30-50 minutes from scratch).
4. **Stage** into `build/release/staging/`:
   - sed-strip absolute paths from KiCad's generated `cmake_install.cmake`
     files (upstream bakes `C:/Program Files/kicad/...` which CPack rejects),
   - `cmake --install --prefix=staging`,
   - copy `api/schemas/*.json` (cmake errors out before reaching them),
   - copy `share/kicad/{resources,scripting,template}` (icons archive +
     Python plugin scaffold + template marker - KiCad's Windows install rules
     skip these),
   - transitively walk every `.exe` and `.dll` in `staging/bin/` with
     `objdump -p`, copying any `/ucrt64/bin/` dependency into `staging/bin/`
     until fixed point,
   - copy Python 3.14 stdlib into `staging/lib/python3.14/`.
5. **Package** with `cpack` (custom standalone NSIS config that points at
   the staged tree, bypassing KiCad's broken install machinery) plus a
   `.NET ZipFile.CreateFromDirectory` portable archive.
6. **Upload** to a GitHub Release. Creates the tag + release if absent;
   replaces existing assets with the same name if present. Auth via the
   git credential helper (Windows Credential Manager) - no PAT prompt.

Expected wall time: 5-10 minutes on a warm tree, 60-90 minutes from a clean
build.

## Common flows

```powershell
# Standard release flow - bumps version, full rebuild, upload
./scripts/release-local.ps1 0.1.1

# Build but don't release (for local verification)
./scripts/release-local.ps1 0.1.1 -SkipUpload

# Dry-run - show every step without changing state
./scripts/release-local.ps1 0.1.1 -DryRun

# Force from-scratch rebuild (wipes build/release/)
./scripts/release-local.ps1 0.2.0 -Clean
```

## Versioning

- `MAJOR.MINOR.PATCH` (semver). `PATCH` for bug fixes, `MINOR` for
  backwards-compatible features, `MAJOR` for breaking changes.
- The script reads from [`VERSION.txt`](../VERSION.txt) if no version arg.
- Pre-release tags are allowed (`0.2.0-rc.1`). The GitHub UI lets you mark
  the resulting Release as "pre-release" after the fact if you want.

## Prerequisites

This is what your local MSYS2 needs (one-time setup):

```bash
# In an MSYS2 UCRT64 shell
pacman -S --needed \
    base-devel \
    mingw-w64-ucrt-x86_64-toolchain \
    mingw-w64-ucrt-x86_64-cmake \
    mingw-w64-ucrt-x86_64-ninja \
    mingw-w64-ucrt-x86_64-pkgconf \
    mingw-w64-ucrt-x86_64-ccache \
    mingw-w64-ucrt-x86_64-nsis \
    mingw-w64-ucrt-x86_64-boost \
    mingw-w64-ucrt-x86_64-wxwidgets3.2-msw \
    mingw-w64-ucrt-x86_64-wxwidgets3.2-msw-libs \
    mingw-w64-ucrt-x86_64-python \
    mingw-w64-ucrt-x86_64-swig \
    mingw-w64-ucrt-x86_64-glew \
    mingw-w64-ucrt-x86_64-glm \
    mingw-w64-ucrt-x86_64-cairo \
    mingw-w64-ucrt-x86_64-pixman \
    mingw-w64-ucrt-x86_64-freetype \
    mingw-w64-ucrt-x86_64-harfbuzz \
    mingw-w64-ucrt-x86_64-fontconfig \
    mingw-w64-ucrt-x86_64-curl \
    mingw-w64-ucrt-x86_64-zlib \
    mingw-w64-ucrt-x86_64-zstd \
    mingw-w64-ucrt-x86_64-openssl \
    mingw-w64-ucrt-x86_64-libgit2 \
    mingw-w64-ucrt-x86_64-ngspice \
    mingw-w64-ucrt-x86_64-opencascade \
    mingw-w64-ucrt-x86_64-protobuf \
    mingw-w64-ucrt-x86_64-abseil-cpp \
    mingw-w64-ucrt-x86_64-nng \
    mingw-w64-ucrt-x86_64-poppler \
    mingw-w64-ucrt-x86_64-unixodbc
```

The script needs MSYS2 at `C:\msys64` and Python stdlib at
`C:\msys64\ucrt64\lib\python3.14`. Both come from the pacman install.

## What's intentionally NOT in the installer

- **`share/kicad/demos/`** - a footprint in the `kit-dev-coldfire-xilinx_5213`
  demo has a 125-character filename which overruns Windows `MAX_PATH` (260 chars)
  inside NSIS's staging tree. Saves ~350 MB. To re-enable in a future version,
  either filter that one demo or enable long-path support.
- **Stock symbol/footprint/3D-model libraries** - these live in separate
  repos (kicad-symbols, kicad-footprints, kicad-packages3D) and aren't part
  of the kicad source tree. The official KiCad installer is ~900 MB largely
  because of bundled 3D models (4.6 GB uncompressed). Users with KiCad 9.0
  already installed can point Preferences -> Configure Paths at
  `C:\Program Files\KiCad\9.0\share\kicad\{symbols,footprints,3dmodels}`.
- **i18n / translations** - configured off (`KICAD_BUILD_I18N=OFF`).

## Troubleshooting

- **`MSYS2 not found`** - the script assumes `C:\msys64`. Edit `$msys2` in
  `release-local.ps1` if yours is elsewhere.
- **`Python stdlib not found`** - install
  `mingw-w64-ucrt-x86_64-python` in MSYS2.
- **`cpack did not produce .exe`** - check
  `build/release/_CPack_Packages/win64/NSIS/NSISOutput.log`. The most
  common cause is a path longer than 260 chars. Add the offending file
  to the deny-list in step 4d.
- **`Could not get GitHub token`** - the script uses the git credential
  helper. Authenticate once via any `git push` to confirm credentials are
  cached, then re-run.
- **Stale build after a source change in a header that's pulled in
  widely** - just re-run; ninja figures out what to rebuild. If something
  feels off, `-Clean` forces a from-scratch rebuild.

## What the release looks like

A successful run lands at
`https://github.com/claynicholson/kicad-copper/releases/tag/v<ver>` with:

- `kicad-copper-<ver>-win64.exe` - NSIS installer (~238 MB)
- `kicad-copper-<ver>-win64-portable.zip` - portable build (~336 MB)
- Auto-generated release notes from the previous tag.

Edit the release in the GitHub UI afterwards if you want to add
highlights or known issues.
