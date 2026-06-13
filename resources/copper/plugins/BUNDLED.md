# Bundled Copper plugins

This directory is the **staging area** for third-party KiCad action plugins that
ship with the KiCad Copper distribution. The contents of each `<name>/` subdir are
vendored by the fetch script (`scripts/fetch-bundled-plugins.ps1` or
`scripts/fetch-bundled-plugins.py`) at the pinned ref below and then installed by
CMake into `${KICAD_PLUGINS}` (`share/kicad/plugins/`) so CPack/NSIS bundles them.

Run the fetch script **before** packaging a release:

```powershell
pwsh scripts/fetch-bundled-plugins.ps1
# or, cross-platform:
python scripts/fetch-bundled-plugins.py
```

The plugin source trees are **not** committed to this repo (they carry their own
git history and licenses). The fetcher clones each at its pinned ref, strips the
nested `.git`, and preserves the upstream `LICENSE` file. If the network is
unavailable the fetcher leaves a `README` placeholder in each `<name>/` dir so the
install layout stays correct, and the release build must re-run the fetcher with
network access.

| Plugin              | Directory       | Repo                                              | Pinned ref   | License        |
|---------------------|-----------------|---------------------------------------------------|--------------|----------------|
| JLCPCB Tools        | `jlcpcb-tools`  | https://github.com/Bouni/kicad-jlcpcb-tools       | `2026.04.03` | MIT            |
| InteractiveHtmlBom  | `ibom`          | https://github.com/openscopeproject/InteractiveHtmlBom | `v2.9.0`     | MIT            |
| Round Tracks        | `round-tracks`  | https://github.com/mitxela/kicad-round-tracks     | `50374f8`    | GPL-3.0-or-later |

## Auto-load note (read before assuming these appear in pcbnew)

KiCad Copper's pcbnew discovers **IPC API plugins** (directories containing a
`plugin.json` manifest) under `PATHS::GetStockPluginsPath()`. On Windows that path
resolves to `<install>/bin/scripting/plugins`, **not** `share/kicad/plugins`. The
three plugins above are *classic SWIG `ActionPlugin` plugins* (they subclass
`pcbnew.ActionPlugin` in `__init__.py`), which require the embedded SWIG Python
scripting layer to be initialized at launch.

See `docs/DISTRIBUTION.md` for the current auto-load status and what is needed to
make these plugins appear in the pcbnew Tools menu.
