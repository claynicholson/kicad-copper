# KiCad Copper distribution ("batteries included")

This document describes what the KiCad Copper distribution ships beyond stock
KiCad: a default dark Copper color theme, dark app/icon chrome on first run, and
a set of bundled third-party plugins staged into the installer payload.

See [`RELEASING.md`](RELEASING.md) for the full release pipeline; this doc covers
only the Copper-specific distribution bits.

## 1. Default Copper Dark color theme

A third built-in color theme is compiled into the binary alongside KiCad's
"KiCad Default" and "KiCad Classic":

| Property        | Value                                                       |
|-----------------|-------------------------------------------------------------|
| Theme key       | `_builtin_copper_dark` (`COLOR_SETTINGS::COLOR_BUILTIN_COPPER_DARK`) |
| Display title   | **Copper Dark**                                             |
| Palette         | `s_copperDarkTheme` in `common/settings/builtin_color_themes.h` |
| Registration    | `COLOR_SETTINGS::CreateBuiltinColorSettings()` in `common/settings/color_settings.cpp` |

The palette mirrors the full key set of `s_defaultTheme` (every required color
slot is present), recolored to a dark schematic + board scheme: dark backgrounds
(`#1a1a1e` schematic, `#161a1a` board/gerbview), light text (`#dcdce1`), and the
Copper brand accent `#c8783c` (200,120,60) on the selection / highlight / cursor /
ratsnest / drawing-sheet slots. These match the chat panel palette in
`eeschema/widgets/copper_chat_widgets.h` (`COPPER_COLORS`).

### How it becomes the first-run default

Three **default** values were changed. All three are defaults applied only when no
user config exists; a returning user's saved settings are loaded over them on
upgrade and win, so existing installs are **not** forced dark:

1. **Color theme** — `appearance.color_theme` default is `COLOR_BUILTIN_COPPER_DARK`
   (was `COLOR_BUILTIN_DEFAULT`), in `common/settings/app_settings.cpp`. This is a
   single shared `APP_SETTINGS` param, so eeschema / pcbnew / symbol-editor /
   fp-editor all inherit it; there is no per-app default override.
2. **App theme** — `appearance.app_theme` default is `APP_THEME::DARK` (was `AUTO`),
   in `common/settings/common_settings.cpp` (Windows path; non-MSW assigns DARK).
3. **Icon theme** — `appearance.icon_theme` default is `ICON_THEME::DARK` (was
   `AUTO`), same file.

## 2. Bundled plugins

Three GPL/MIT plugins are vendored and staged into the installer:

| Plugin             | Repo                                                   | Pinned ref   | License          |
|--------------------|--------------------------------------------------------|--------------|------------------|
| JLCPCB Tools       | https://github.com/Bouni/kicad-jlcpcb-tools            | `2026.04.03` | MIT              |
| InteractiveHtmlBom | https://github.com/openscopeproject/InteractiveHtmlBom | `v2.9.0`     | MIT              |
| Round Tracks       | https://github.com/mitxela/kicad-round-tracks          | `50374f8`    | GPL-3.0-or-later |

### Vendoring + install flow

1. **Fetch** (run before packaging — `scripts/release-local.ps1` does this for you
   in step 0b):

   ```powershell
   pwsh scripts/fetch-bundled-plugins.ps1     # Windows
   python scripts/fetch-bundled-plugins.py    # cross-platform
   ```

   Each plugin is cloned at its pinned ref into `resources/copper/plugins/<name>/`,
   the nested `.git` is stripped, and the upstream `LICENSE` is preserved. The
   source trees are **not** committed to this repo. On network failure the fetcher
   drops a `README.placeholder.md` into each dir (so the layout is correct) and
   exits non-zero so CI flags an incomplete payload.

2. **Install** — `CMakeLists.txt` has an `install(DIRECTORY resources/copper/plugins/
   DESTINATION ${KICAD_PLUGINS} ...)` rule (component `bundled_plugins`), guarded by
   a `file(GLOB)` check so a missing/empty staging dir never breaks configure. On
   Windows `${KICAD_PLUGINS}` is `share/kicad/plugins`. CPack/NSIS then bundles it.

### ⚠️ Auto-load status — important caveat

The three bundled plugins are **classic SWIG `pcbnew.ActionPlugin` plugins**. KiCad
Copper releases are configured with `-DKICAD_SCRIPTING_WXPYTHON=OFF` (see
`scripts/release-local.ps1`), and this fork's pcbnew does not initialize the legacy
SWIG Python scripting subsystem at launch. The plugin discovery that *does* run on
startup is the **IPC API plugin manager** (`common/api/api_plugin_manager.cpp`),
invoked from `PGM_BASE` (`common/pgm_base.cpp` → `ReloadPlugins()`):

- It scans `PATHS::GetStockPluginsPath()` — on Windows `<install>/bin/scripting/
  plugins`, **not** `share/kicad/plugins` — plus the PCM 3rd-party dir and the user
  dir (`%APPDATA%/kicad/<ver>/plugins`).
- It registers a plugin only if its directory contains a **`plugin.json`** IPC API
  manifest (`identifier`, `name`, `runtime`, ...). Classic `ActionPlugin`
  `__init__.py` plugins have no such manifest.

**Consequence:** with the current release configuration these plugins are *bundled
and present on disk* under `share/kicad/plugins/`, and their layout/licensing/CMake
packaging is correct and forward-compatible, but they will **not** auto-appear in
the pcbnew Tools menu until one of the following is done (out of scope for this
change, flagged for follow-up):

- enable the SWIG/wxPython scripting layer in the release build and initialize it
  in pcbnew so classic action plugins under the stock plugins path are discovered;
  **and/or**
- have the installer also place the plugins under the IPC stock path
  (`bin/scripting/plugins`) for any plugin shipping a `plugin.json`.

A user can still load them manually by copying a plugin dir into
`%APPDATA%/kicad/<ver>/scripting/plugins` in a KiCad build that has scripting
enabled. This caveat is tracked here rather than silently shipping a dead menu.

## 3. Building / packaging the release

One command, from a Windows MSYS2 UCRT64 shell set up per upstream KiCad docs:

```powershell
./scripts/release-local.ps1 <version>      # e.g. 0.1.1
```

It runs the plugin fetch, configures, builds, stages, and produces the NSIS `.exe`
+ portable `.zip`. To package by hand after a build:

```powershell
python scripts/fetch-bundled-plugins.py     # vendor plugins first
cd build/release && cmake .. && cpack
```
