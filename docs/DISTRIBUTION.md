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

2. **Install** — `CMakeLists.txt` installs `resources/copper/plugins/` into the
   directory the plugin manager actually scans at runtime,
   `PATHS::GetStockPluginsPath()` (component `bundled_plugins`, guarded by a
   `file(GLOB)` check so a missing/empty staging dir never breaks configure):

   | Platform | Install destination                | Resolves from                                   |
   |----------|------------------------------------|-------------------------------------------------|
   | Windows  | `<install>/bin/scripting/plugins`  | `GetExecutablePath()` + `scripting/plugins`      |
   | Linux    | `<KICAD_DATA>/plugins`             | `GetStockDataPath()` + `plugins`                 |
   | macOS    | `${KICAD_PLUGINS}` (SharedSupport) | bundle `Contents/SharedSupport/plugins`          |

   This is **not** `share/kicad/plugins`; an earlier draft of this rule targeted
   that path and the plugins would never have been scanned. `cmake --install` (run
   by `release-local.ps1` step 4b) stages them automatically; CPack/NSIS then bundles
   the result.

### ⚠️ Auto-load status — read this before expecting the plugins in the menu

**This fork has no in-process SWIG / wxPython scripting subsystem.** It was removed
wholesale in commit *"REMOVED: SWIG, wxPython, and Python integration"* (518 files,
~97k deletions): all `.i` interface files, `cmake/FindSWIG.cmake`,
`cmake/FindwxPython.cmake`, `cmake/FindPythonLibs.cmake`, `include/macros_swig.h`,
`common/swig/`, the `FRAME_PYTHON` scripting console, and every
`InitPythonScripting` / classic-`ActionPlugin` loader call. **There is no
`KICAD_SCRIPTING_WXPYTHON` CMake option in this tree** — passing `-D…=ON/OFF` is a
silently-ignored no-op (the value seen in a pre-removal `CMakeCache.txt` is stale).
Re-enabling in-process scripting would mean reverting that 97k-line removal and
regenerating the SWIG `pcbnew` bindings — not a flag flip, and not doable without a
full rebuild. So we did **not** flip a flag; instead we point the install at the
real scan path and document the actual model below.

The only plugin system this fork runs on launch is the **out-of-process IPC API
plugin manager** (`common/api/api_plugin_manager.cpp`), kicked off from
`PGM_BASE::InitPgm()` → `m_plugin_manager->ReloadPlugins()` (`common/pgm_base.cpp:483`).
**Two gates apply:** the call is wrapped in `#ifdef KICAD_IPC_API` (CMake option,
default **ON**) *and* `commonSettings->m_Api.enable_server` (`api.enable_server`,
default **false**). So even an IPC-manifested plugin is only scanned/loaded once the
user turns on **Preferences → Plugins → "Enable KiCad API"** (the API server). Its
discovery rules:

- It recursively scans `PATHS::GetStockPluginsPath()` (the table above), the PCM
  3rd-party dir (`KICAD9_3RD_PARTY`/`plugins`-style), and the user dir
  (`%APPDATA%/kicad/<ver>/plugins`) — `PLUGIN_TRAVERSER::OnFile`,
  `api_plugin_manager.cpp:160`.
- It registers a plugin **only if the directory contains a `plugin.json`** that
  validates against `api.v1.schema.json` with required keys `identifier`, `name`,
  `description`, and a `runtime` (the external Python interpreter / venv it launches
  via `PYTHON_MANAGER::Execute`). See `common/api/api_plugin.cpp:63-159`.

**The three bundled plugins are classic `pcbnew.ActionPlugin` plugins and do NOT
ship a `plugin.json`** (jlcpcb-tools carries a PCM `metadata.template.json`, which is
a different thing). Therefore:

> With this release configuration the plugins are **bundled, licensed, and placed at
> the correct scan path**, but they will **not** surface in the pcbnew Tools menu.
> Making them load requires **porting each to the IPC API** (add a `plugin.json` with
> a `runtime`, and rewrite the plugin to talk to KiCad over `kipy`/the IPC API
> instead of importing the in-process `pcbnew` SWIG module). That port is per-plugin
> upstream work and is out of scope here; this is tracked rather than silently
> shipping a dead menu.

If/when a plugin gains a valid `plugin.json`, no CMake change is needed — it will be
discovered automatically because the install already targets the scan path.

### Python runtime bundling (what's wired vs. still manual)

The IPC model needs an **external** Python interpreter at runtime (KiCad launches it
as a subprocess; it is *not* embedded for scripting). What the release currently does:

- **Wired:** the MSYS2 UCRT64 `python3.14` interpreter + stdlib are already staged
  into the portable tree by `release-local.ps1` (step "Python stdlib",
  `lib/python3.14/...`), because `kicad.exe` links `libpython` for unrelated reasons.
  `PYTHON_MANAGER::FindPythonInterpreter()` will also find a system Python on PATH.
- **Wired:** bundled-plugin files are staged to the scan path (above).
- **NOT wired / not needed for IPC:** there is **no** SWIG `_pcbnew.pyd` / `pcbnew.py`
  module in this fork (it was deleted), and **no wxPython** is bundled. IPC plugins
  don't import the in-process `pcbnew` module or wxPython; they depend on the `kipy`
  package, which a plugin's `runtime` venv is expected to provide. We do **not**
  pre-create per-plugin venvs in the installer.

Checklist if you later ship an IPC-ported plugin and want it to work on a clean box:

1. Plugin dir under the scan path contains a valid `plugin.json` (`identifier`,
   `name`, `description`, `runtime`).
2. A Python interpreter is resolvable — bundled `lib/python3.14` or system PATH.
3. The plugin's `runtime` venv has `kipy` (and the plugin's own deps) installed, or
   the plugin declares an `install` step KiCad runs on first use.
4. `api/schemas/api.v1.schema.json` is staged (it already is — step 4c).

### Verifying after a build

1. Install/extract the release and launch **pcbnew**.
2. Confirm the plugin scan path exists and is populated:
   `…\KiCad Copper\bin\scripting\plugins\{jlcpcb-tools,ibom,round-tracks}\`.
3. Turn on **Preferences → Plugins → "Enable KiCad API"** (`api.enable_server`,
   off by default) — without it the manager never scans, even for IPC plugins.
4. Tools menu / the plugins toolbar: **with the current (un-ported) plugins these
   will NOT appear** — that is expected per the caveat above. To prove the pipeline,
   drop a minimal `plugin.json` test plugin into the scan dir, relaunch (with the API
   enabled), and confirm it shows up in Preferences → Plugins.
5. Enable API trace logging (set `KICAD_TRACE=KICAD_API`) and check the log: with the
   API server on, the manager prints "scanning system path
   (…bin/scripting/plugins…) for plugins" on launch, confirming the path alignment
   even when no IPC plugin is present.

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
