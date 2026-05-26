# Releasing kicad-copper

This repo ships a Windows `.exe` (NSIS installer) plus a portable `.zip` for
each release. Releases are built by a GitHub Actions workflow and attached to
a GitHub Release on `claynicholson/kicad-copper`.

## TL;DR — cut a new release

From a clean working tree on `master`:

```powershell
./scripts/release.ps1 0.2.0
```

That's it. The script:

1. Validates the version (semver: `X.Y.Z` or `X.Y.Z-prerelease`).
2. Updates the [`VERSION`](../VERSION) file.
3. Commits the bump as `Release v0.2.0`.
4. Tags the commit `v0.2.0`.
5. Pushes the commit and tag to `origin`.

The push of `v*` triggers
[`.github/workflows/release.yml`](../.github/workflows/release.yml), which:

1. Spins up a `windows-2022` runner.
2. Sets up MSYS2 / UCRT64 with KiCad's build dependencies (cached).
3. Configures + builds with CMake / Ninja (ccache'd).
4. Runs `cpack` to produce `kicad-copper-<ver>-win64.exe` (NSIS) and
   `kicad-copper-<ver>-win64.zip` (portable).
5. Creates a GitHub Release at `v<ver>` with both files attached.

Expected runtime: **~1–2 hours on a cold cache, ~30–45 minutes after caches
warm up**. KiCad has a lot of C++ to compile, even on a 4-vCPU runner.

## Dry run

```powershell
./scripts/release.ps1 0.2.0 -DryRun
```

Prints every git/file operation without performing it. Good for sanity-checking
the version string and remote.

## Test the workflow without making a release

Use **Actions → Release Windows .exe → Run workflow** in the GitHub UI (or
`gh workflow run release.yml -f version=0.2.0-test`). This builds and uploads
the artifacts as a regular workflow artifact (downloadable from the run page)
**without** creating a GitHub Release. Useful when iterating on the workflow
itself.

## Version policy

- `MAJOR.MINOR.PATCH` (semver). Bump:
  - `PATCH` for bug fixes only,
  - `MINOR` for new features that are backwards-compatible,
  - `MAJOR` for breaking changes.
- Pre-releases are allowed (`0.2.0-rc.1`). They still create a GitHub Release;
  flip the Release to "pre-release" in the UI if needed.
- The version lives in [`VERSION`](../VERSION) and is read at CMake configure
  time by [`cmake/CopperPackaging.cmake`](../cmake/CopperPackaging.cmake).
- CI also overrides `COPPER_VERSION` from the tag, so the installer's embedded
  version always matches the tag (even if someone forgot to bump `VERSION`).

## Authentication

The workflow uses the default `GITHUB_TOKEN`. No PATs required. Permissions
needed (`contents: write`) are declared inline in the workflow.

If you ever need to publish from a fork or a non-default remote, set
`-Remote upstream` (or similar) on the release script.

## Re-releasing a tag (escape hatch)

If a release artifact is broken and you need to rebuild the same version:

```powershell
./scripts/release.ps1 0.2.0 -Force
```

`-Force` deletes the local tag and pushes a new one. Then in GitHub: delete the
existing Release (its assets go with it) and re-run the workflow, or push the
tag with `--force` to retrigger the workflow.

Avoid this when possible — bump the patch instead (`0.2.1`).

## Troubleshooting the first build

The MSYS2 package list in `release.yml` covers KiCad's standard Windows build,
but the first run may surface a missing dep. Symptoms and fixes:

- **CMake errors `Could NOT find <Lib>`** — add `mingw-w64-ucrt-x86_64-<lib>`
  to the `install:` block in `release.yml`. Search package names with
  `pacman -Ss <lib>` from a local UCRT64 shell.
- **`cpack` produced no `.exe`** — NSIS missing. Verify
  `mingw-w64-ucrt-x86_64-nsis` is installed in the workflow.
- **Out-of-disk** — the runner has ~14 GB free on `D:`. Build dir + ccache fit,
  but ramping `ccache --max-size` past 5 GB can break it. Don't.
- **Build time exceeds 6 h** — `timeout-minutes: 360` in the workflow. Bump if
  caches are still warming.

When a build fails, the workflow uploads the `compilation_log.txt` and CMake
configure log as a workflow artifact — download from the run page for
post-mortem.

## What the release looks like

A successful release lands at
`https://github.com/claynicholson/kicad-copper/releases/tag/v<ver>` with:

- `kicad-copper-<ver>-win64.exe` — NSIS installer (~150–250 MB)
- `kicad-copper-<ver>-win64.zip` — portable build (~250–400 MB)
- Auto-generated release notes (commit log since the previous tag).

Edit the release in the UI afterwards if you want to add highlights or known
issues.
