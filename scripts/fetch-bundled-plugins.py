#!/usr/bin/env python3
"""Vendor the bundled Copper KiCad plugins at their pinned refs.

Clones each plugin listed in PLUGINS into resources/copper/plugins/<dir>/ at the
pinned ref, removes the nested .git, and keeps the upstream LICENSE. Run this
before packaging a release (see docs/DISTRIBUTION.md).

If the network is unavailable (clone fails), a README placeholder is written into
each <dir>/ so the install layout stays correct, and the script exits non-zero so
CI can flag that the release payload is incomplete.

Cross-platform; mirrors scripts/fetch-bundled-plugins.ps1.
"""
from __future__ import annotations

import os
import shutil
import stat
import subprocess
import sys
from pathlib import Path


def _force_rmtree(path: Path) -> None:
    """rmtree that clears read-only bits (Windows .git pack files are read-only)."""
    def on_error(func, p, _exc):
        os.chmod(p, stat.S_IWRITE)
        func(p)

    if path.exists():
        shutil.rmtree(path, onerror=on_error)

# (directory, repo url, pinned ref, license id)
PLUGINS = [
    ("jlcpcb-tools", "https://github.com/Bouni/kicad-jlcpcb-tools", "2026.04.03", "MIT"),
    ("ibom", "https://github.com/openscopeproject/InteractiveHtmlBom", "v2.9.0", "MIT"),
    ("round-tracks", "https://github.com/mitxela/kicad-round-tracks", "50374f8", "GPL-3.0-or-later"),
]

REPO_ROOT = Path(__file__).resolve().parent.parent
STAGING = REPO_ROOT / "resources" / "copper" / "plugins"


def run(cmd: list[str], cwd: Path | None = None) -> bool:
    print("  $", " ".join(cmd))
    return subprocess.run(cmd, cwd=cwd).returncode == 0


def placeholder(dest: Path, repo: str, ref: str, lic: str) -> None:
    dest.mkdir(parents=True, exist_ok=True)
    (dest / "README.placeholder.md").write_text(
        f"# {dest.name} (not vendored)\n\n"
        f"This plugin was NOT fetched (network unavailable when the fetch script ran).\n\n"
        f"- Repo: {repo}\n- Pinned ref: {ref}\n- License: {lic}\n\n"
        f"Re-run `python scripts/fetch-bundled-plugins.py` (or the .ps1) with network\n"
        f"access before packaging a release.\n",
        encoding="utf-8",
    )


def fetch(dir_name: str, repo: str, ref: str, lic: str) -> bool:
    dest = STAGING / dir_name
    print(f"[{dir_name}] {repo}@{ref}")

    _force_rmtree(dest)

    # Shallow clone then checkout the pinned ref (works for both tags and commits).
    if not run(["git", "clone", "--quiet", repo, str(dest)]):
        print(f"  ! clone failed; writing placeholder for {dir_name}")
        placeholder(dest, repo, ref, lic)
        return False

    if not run(["git", "-C", str(dest), "checkout", "--quiet", ref]):
        print(f"  ! checkout {ref} failed; writing placeholder for {dir_name}")
        _force_rmtree(dest)
        placeholder(dest, repo, ref, lic)
        return False

    # Strip nested VCS metadata so it does not pollute the outer repo / payload.
    _force_rmtree(dest / ".git")
    for junk in (".github", ".gitignore", ".gitattributes"):
        p = dest / junk
        if p.is_dir():
            _force_rmtree(p)
        elif p.exists():
            p.unlink()

    has_license = any((dest / n).exists() for n in ("LICENSE", "LICENSE.txt", "LICENSE.md", "COPYING"))
    print(f"  ok ({'LICENSE present' if has_license else 'WARNING: no LICENSE file found'})")
    return True


def main() -> int:
    STAGING.mkdir(parents=True, exist_ok=True)
    results = {name: fetch(name, repo, ref, lic) for name, repo, ref, lic in PLUGINS}
    ok = sum(results.values())
    print(f"\nVendored {ok}/{len(PLUGINS)} plugins into {STAGING}")
    if ok != len(PLUGINS):
        print("One or more plugins were NOT vendored. Re-run with network access "
              "before packaging.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
