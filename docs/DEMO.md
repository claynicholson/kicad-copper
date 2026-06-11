# DEMO — manual smoke test in real KiCad (post-build)

This file documents the manual test recommended before shipping a release.
The §8 self-assessment is headless by design, so an in-editor smoke is a
final safety net — not a harness gate.

## Prerequisites

- KiCad compiled with this fork (M4 CMake changes — see
  [eeschema/CMakeLists.txt](../eeschema/CMakeLists.txt) and the §0 audit
  notes in [FORK_SURFACE.md](FORK_SURFACE.md) §8).
- A reachable Copper backend:
  - hosted: `https://api.coppereda.com` (default)
  - local self-host: e.g. `http://localhost:8000`
- Auth is optional — the hosted backend accepts anonymous requests. For
  authenticated use: an OAuth account, or `COPPER_API_TOKEN` env var for
  dev (logs a warning per ADR-005).

## Steps

1. Launch `eeschema`.
2. Open a blank `.kicad_sch` (`File → New Schematic`).
3. Toggle the Copper AI pane on (right side; AUI pane name `CopperChat`).
   It auto-opens on startup if you set `copper.show_panel_on_startup` to
   `true` in the eeschema config JSON, or via the Copper settings dialog
   once implemented.
4. (Optional) Settings → Copper → set API URL to your backend, or
   `export COPPER_API_URL=http://localhost:8000` before launching.
   For transport troubleshooting set `COPPER_DEBUG=1` — requests, HTTP
   statuses, curl errors and SSE events land in `%TEMP%\copper_debug.log`.
5. (Optional) Click **Login** in the Copper panel header → OAuth browser
   flow → tokens cached in OS keychain. Anonymous requests work against
   the hosted backend.
6. In the input bar, mode = **Design**, type:
   > `RP2040 dev board with a 6-axis IMU for a flight controller`
7. Press Enter / Send.
8. Observe stages stream in: `choosing_parts → expanding_reference_circuits
   → placement → wiring → verifying`. Mid-stream AI messages may appear.
9. A **plan card** with numbered steps appears. Click **Approve**.
10. The schematic now contains: 2 ICs, 2 power symbols, 2 labels (SDA/SCL),
    2 wires, 2 junctions (per the canonical happy fixture).
11. Press **Ctrl-Z** once — the **entire** generated board disappears in
    one undo. (Verifies the §10 single-undo guarantee.)
12. Press **Ctrl-Y** — it comes back.

## Recording results

Append a row to the end of this file each time you do the smoke test:

| Date       | KiCad commit | OS       | Backend         | Result | Notes |
|------------|--------------|----------|-----------------|--------|-------|
| (template) | (sha)        | (Win/Mac/Linux) | hosted / local  | pass / fail | …  |

## Failure-path smoke checks (also good to do)

- **Offline:** disconnect network → send a prompt → should see an
  actionable "couldn't reach backend" state with a Retry button (M5).
- **Schema mismatch:** point the plugin at a backend that returns
  `protocol_version: 99` → should see "Update KiCad Copper" state, nothing
  applied.
- **Mid-apply error:** point at a backend that emits a plan referencing a
  library symbol not on your machine (`bogus:NotReal`) → should see
  "Symbol not found in libraries: bogus:NotReal. Nothing applied." with
  no schematic mutation (ADR-004 fail-closed).
