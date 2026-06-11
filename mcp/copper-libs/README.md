# copper-libs MCP server

Zero-dependency Node MCP server (stdio) exposing the local KiCad symbol and
footprint libraries, plus self-service library install/repair.

```sh
node mcp/copper-libs/server.mjs
```

Registered for Claude Code via `.mcp.json` at the repo root.

## Tools

| tool | what it does |
|---|---|
| `list_libraries` | every lib in the global `sym-lib-table` / `fp-lib-table`, env vars resolved, existence-checked |
| `search_symbols` | substring search across all `.kicad_sym` files → `Lib:Symbol` ids |
| `search_footprints` | substring search across all `.pretty` dirs → `Lib:Footprint` ids |
| `get_symbol_info` | pins, description, default footprint, fp filters for one lib_id |
| `repair_library_tables` | point the newest KiCad config at an existing install (e.g. KiCad 9 in Program Files) — instant, no download; backs up tables |
| `install_libraries` | `git clone --depth 1` the official kicad-symbols (and optionally kicad-footprints, ~1 GB) from GitLab and register them |

## Why this exists

The kicad-copper portable build ships **no** symbol libraries
(`staging/share/kicad/` has no `symbols/` and no `template/sym-lib-table`),
so a fresh config floods "Library not found in library table" and every
`PLACE_COMPONENT` fails. `repair_library_tables` fixes that in one call when
a stock KiCad is installed; `install_libraries` fixes it from nothing.

Config discovery: newest `%APPDATA%\kicad\<version>\` (or XDG/macOS
equivalents). Env vars come from `kicad_common.json` + process env.
