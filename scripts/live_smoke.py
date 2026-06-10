"""Live end-to-end smoke test against the hosted Copper backend.

Mirrors the plugin's exact path: POST /api/v1/generate (SSE) -> collect the
`done` event -> unwrap if nested (backend <= v0.1.0) -> validate_response ->
ApplyEngine -> FakeSchematicApi -> single undo.

Usage:
    PYTHONPATH=<copper-2> python scripts/live_smoke.py [base_url] [prompt]
"""

from __future__ import annotations

import json
import sys
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from copper_integration import ApplyEngine, FakeSchematicApi, validate_response  # noqa: E402

BASE_URL = sys.argv[1] if len(sys.argv) > 1 else "https://api.coppereda.com"
PROMPT = sys.argv[2] if len(sys.argv) > 2 else "a 555 timer led blinker"


def sse_events(resp):
    event, data_lines = "message", []
    for raw in resp:
        line = raw.decode("utf-8").rstrip("\r\n")
        if not line:
            if data_lines:
                yield event, "\n".join(data_lines)
            event, data_lines = "message", []
        elif line.startswith("event:"):
            event = line[6:].strip()
        elif line.startswith("data:"):
            data_lines.append(line[5:].lstrip(" "))


def main() -> int:
    print(f"POST {BASE_URL}/api/v1/generate  prompt={PROMPT!r}")
    req = urllib.request.Request(
        f"{BASE_URL}/api/v1/generate",
        data=json.dumps({"prompt": PROMPT, "intent": "generate", "context": {}}).encode(),
        headers={
            "Content-Type": "application/json",
            "Accept": "text/event-stream",
            # Same UA as the plugin (the edge blocks Python-urllib's default).
            "User-Agent": "KiCad-Copper/1.0",
        },
        method="POST",
    )

    done = None
    with urllib.request.urlopen(req, timeout=120) as resp:
        for event, data in sse_events(resp):
            payload = json.loads(data)
            if event == "stage":
                print(f"  stage {payload.get('name')}: {payload.get('status')}")
            elif event == "message":
                print(f"  message: {payload.get('text', '')[:80]}")
            elif event == "error":
                print(f"FAIL: server error event: {payload.get('message')}")
                return 1
            elif event == "done":
                done = payload

    if done is None:
        print("FAIL: stream ended without a done event")
        return 1

    # Plugin's defensive unwrap (copper_chat_panel.cpp handleSSEEvent).
    if "operations" not in done and isinstance(done.get("plan"), dict) and "operations" in done["plan"]:
        print("  (unwrapping nested done payload — backend <= v0.1.0)")
        done = done["plan"]

    validated = validate_response(done)
    assert validated.success, f"validation failed: {validated}"
    assert validated.operations, "no operations in plan"
    print(f"  validated: {len(validated.operations)} ops, {len(validated.plan_steps)} plan steps")

    api = FakeSchematicApi()
    before = api.serialize()
    result = ApplyEngine().apply_response(api, done)
    assert result.ok, f"apply failed: {result}"
    assert len(api.list_symbols()) > 0, "no symbols placed"
    print(f"  applied: {len(api.list_symbols())} symbols on schematic")

    api.undo()
    assert api.serialize() == before, "single undo did not fully revert"
    print("  undo: clean single-transaction revert")

    print("PASS: live end-to-end generate -> validate -> apply -> undo")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
