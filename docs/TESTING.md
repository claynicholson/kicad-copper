# TESTING — how to run the closed-loop self-assessment and add cases

## Quick start

```bash
# From the repo root.
python scripts/assess.py
```

That runs every §8 check, prints a scorecard, writes
[SELF_ASSESSMENT.md](SELF_ASSESSMENT.md) + `assessment.json` at the repo
root, appends a row to [LOOP_LOG.md](LOOP_LOG.md), and exits non-zero unless
*polished* per §10.

No dependencies are required to run the harness: stdlib Python (≥ 3.8).

## Optional: dev tools

```bash
python -m pip install ruff
ruff check copper_integration/ tests/ scripts/
```

`ruff` is used only by Check 2 (lint/format) — a soft gate. The harness
detects ruff's absence and reports it as "skipped, no penalty".

## Running individual checks

```bash
python scripts/assess.py --check 3       # only the BackendClient unit check
python scripts/assess.py --check 5,6,7   # the apply / rollback / quality checks
python scripts/assess.py --verbose       # print stage-by-stage
python scripts/assess.py --no-write      # don't update SELF_ASSESSMENT.md
```

## Layout

```
copper_integration/         the harness modules
  __init__.py
  schematic_api.py          SchematicApi + FakeSchematicApi
  validators.py             PROTOCOL.md hard-rejects
  backend_client.py         transport + SSE parser + auth + states
  stub_backend.py           in-memory backend that replays fixtures
  apply_engine.py           plan -> ordered Fake API calls in one commit
  controller.py             end-to-end orchestrator
  settings.py               env > settings > default resolution
  panel_glue.py             contract for the C++ panel side (stub here)
tests/                      pytest-style tests (no pytest required)
  test_fake_schematic_api.py
  test_validators.py
  test_backend_client.py
  test_apply_engine.py
  test_atomic_rollback.py
  test_applied_board_quality.py
  test_state_handling.py
  test_flows.py             end-to-end happy/offline/malformed/mid-apply
fixtures/                   canned PROTOCOL.md payloads
  generate_happy.sse
  generate_partial.sse
  generate_error.sse
  generate_malformed.sse
  chat_response.json
  plan_response.json
  unauthorized_401.json
scripts/
  assess.py                 the §8 harness
```

## Writing a new test

Tests live in `tests/`. They are stdlib `unittest`-discoverable (one class per
file with `Test*` methods). The harness collects them by walking `tests/`.

Example:

```python
# tests/test_my_thing.py
import unittest
from copper_integration.schematic_api import FakeSchematicApi

class MyThingTest(unittest.TestCase):
    def test_one_thing(self):
        api = FakeSchematicApi()
        self.assertEqual(api.list_symbols(), [])
```

Run:
```bash
python scripts/assess.py --check 5    # whichever check this test belongs to
# or
python -m unittest tests.test_my_thing
```

## Adding a new fixture

Drop a JSON or `.sse` file under `fixtures/`. SSE files are read literally
(byte-for-byte) by `StubBackend`; JSON files are parsed and re-emitted.

```
fixtures/generate_my_case.sse
```

Then reference it from a test:
```python
backend.load_fixture("generate_my_case.sse")
```

## Live backend smoke test (no KiCad needed)

`scripts/live_smoke.py` exercises the hosted backend end-to-end exactly the
way the plugin does: `POST /api/v1/generate` (SSE) → collect `done` →
unwrap-if-nested → `validate_response` → `ApplyEngine` → `FakeSchematicApi`
→ single undo.

```bash
PYTHONPATH=<copper-2> python scripts/live_smoke.py                        # hosted (api.coppereda.com)
PYTHONPATH=<copper-2> python scripts/live_smoke.py http://localhost:8080  # local backend
```

It sends the plugin's `KiCad-Copper/1.0` User-Agent (the hosted edge blocks
generic script UAs). Exit code 0 = full round trip passed.

## Manual smoke test in real KiCad

The harness is intentionally headless. A real-KiCad manual smoke test is
**recommended before shipping a release** but is NOT a gate per §8.

Steps (recorded in [DEMO.md](DEMO.md) once M6 is done):

1. Build KiCad with `KICAD_IPC_API=ON` and the patched
   `eeschema/CMakeLists.txt` (CMake gap G-CMAKE closed in M4).
2. Run `eeschema` and open a blank `.kicad_sch`.
3. Toggle View → "Copper AI" to dock the chat panel.
4. (Optional, self-host) `export COPPER_API_URL=http://localhost:8000`
   before launching, or set it in the Copper settings dialog.
5. (Optional) Click "Login" (or `export COPPER_API_TOKEN=…` for a dev
   token). Anonymous requests work against the hosted backend.
6. In the panel, type:
   `RP2040 dev board with a 6-axis IMU for a flight controller`
7. Watch stages stream in. A plan card appears. Click **Apply**.
8. Confirm the schematic now contains the board.
9. Press **Ctrl-Z** once — the whole apply should disappear.
10. Press **Ctrl-Y** — it should come back.

Record results (date, KiCad version, host OS, observed lag) in `docs/DEMO.md`.

## CI integration (recommended)

A minimal CI job:

```yaml
- name: Copper harness
  run: python scripts/assess.py
- name: Upload scorecard
  uses: actions/upload-artifact@v3
  with:
    name: scorecard
    path: docs/SELF_ASSESSMENT.md
```

Exit code 0 only if `polished` per §10. Otherwise the harness exit code is
the failing-check count (1–8), which `set -e` will catch.
