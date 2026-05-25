# DECISIONS — architectural decisions (ADR-style, append-only)

## ADR-001 — Plugin in C++; harness in Python

**Context.** BUILD_PROMPT.md §4 specifies a `copper_integration/` Python module
layout (`*.py` files). The actual existing v1 in this fork is **native C++**
modifying KiCad's source tree (`eeschema/widgets/copper_chat_panel.cpp`,
`eeschema/copper/`, `eeschema/api/http_bridge.cpp`). KiCad has no Python
plugin system for the schematic editor — the eeschema chat panel cannot be
implemented in Python.

**Decision.** Split the work:
- The **plugin** stays in C++, in the existing v1 file locations. We extend,
  wire, validate, and harden it in place per BUILD_PROMPT §0.4 ("extend, don't
  replace").
- The **closed-loop self-assessment harness** (BUILD_PROMPT §8) lives in
  Python at `copper_integration/` exactly as BUILD_PROMPT §4 prescribes. Its
  job is to validate the wire protocol and the apply-layer semantics that the
  C++ implements. It runs **headless** (no GUI KiCad, no compiled fork required).

**Why this is correct under the prompt's principles.**
- §3 "Thin client, smart backend." Honored — neither side adds generation
  logic.
- §3 "GPL-clean." Both sides are GPLv3-compatible. Python is PSF-licensed,
  GPL-compatible.
- §3 "Validate everything from the backend." The Python harness validates
  the same JSON shapes the C++ client consumes, against the same
  `PROTOCOL.md` contract. Cross-language validation is *stronger* than
  same-language tests.
- §8 "All checks must run headless." Building KiCad in CI is hours of cold
  build; testing protocol contracts in Python is seconds. Honored.

**Trade-off accepted.** The Python `ApplyEngine`/`FakeSchematicApi` is a
*shadow* of the C++ `SCH_COMMIT`-based apply path — it models the semantics
(atomic, ordered, validated, idempotent) without being the actual production
code. We accept the risk that a future C++ change diverges from the model and
mitigate it by: (a) keeping `docs/PROTOCOL.md` + the apply-op schema in this
file as the single source of truth, with both sides referencing it; (b) a
post-build manual smoke test recorded in `docs/TESTING.md`.

**Status.** Accepted, 2026-05-24.

---

## ADR-002 — Apply-plan over file-replace

**Context.** BUILD_PROMPT.md §2: "Prefer an explicit apply-plan (ordered,
idempotent ops: place symbol X at (x,y); add label N on pin P) over blind
file replacement, so it's inspectable and undoable. Always go through the
fork API." The existing v1 already does this — `COPPER::Operation { type, data }`
is the apply-plan unit.

**Decision.** Keep the existing apply-plan shape (`PLACE_COMPONENT`,
`ADD_WIRE`, `ADD_LABEL`, `ADD_JUNCTION`, `ADD_POWER_SYMBOL`). The backend
returns a list of these; the apply layer materializes them via `SCH_COMMIT`.
Do not introduce a "replace the .kicad_sch file" path even as a fallback.

**Why.**
- Inspectable: a JSON list is readable and diffable by a human.
- Undoable: one `SCH_COMMIT::Push()` per plan = one Ctrl-Z to revert.
- Safe-by-construction: we never touch the file behind a running KiCad.
- Extensible: new op types add a case in the switch; old plans keep working.

**Status.** Accepted.

---

## ADR-003 — One `SCH_COMMIT` per applied plan (single-undo)

**Context.** The HTTP bridge pushes one commit per HTTP call (per-op
granularity). The panel's `ExecuteOperations` pushes one commit for the whole
plan (per-plan granularity). BUILD_PROMPT.md §5 + §6 require "one clean Undo
reverts the whole apply."

**Decision.** **Per-plan granularity** is the canonical model for chat-driven
applies. The bridge keeps per-op granularity for external tools (where each
HTTP call is independent and the user expects independent undo). Document
this distinction in PROTOCOL.md.

**Why.** Users think in plans, not ops. "I asked for an RP2040 board, I want
one Ctrl-Z to undo *the board*, not 40 Ctrl-Zs to undo every wire."

**Status.** Accepted.

---

## ADR-004 — Fail-closed validation on op construction

**Context.** Gap B6 / G-ATOMIC: `ExecuteOperations` currently skips unknown
ops and bad lib_ids silently, then pushes whatever it built. A 10-op plan
with one bad op can produce a 9-op partial apply.

**Decision.** **Validate the entire plan before constructing any object.** If
any op fails validation, surface the error in the panel and apply nothing.
This is more conservative than KiCad's typical "skip the bad item" policy,
but matches §6 ("never leave a half-applied schematic").

**Why.** A partial schematic is worse than none — the user's mental model is
"the AI did what I asked" but reality is a mutilated subset. Better to fail
loudly and let the user resubmit.

**Status.** Accepted. Implemented in M3.

---

## ADR-005 — Settings precedence: env var > settings > default

**Context.** BUILD_PROMPT.md §3: "Config over hardcode. Backend URL + token
from settings/env; the plugin must work pointed at hosted or local backend
with no code change."

**Decision.** Resolution order for `COPPER_API_URL`:
1. `COPPER_API_URL` environment variable (highest, for self-host / dev).
2. `EESCHEMA_SETTINGS::m_Copper.api_url` (user-set via dialog).
3. Default: `https://api.copper.dev` (hosted).

Token order:
1. OAuth token from `SECURE_TOKEN_STORE` (default).
2. `COPPER_API_TOKEN` env var (local dev only; logs a warning that this
   bypasses the keychain).

**Why.** Env-var override is required for hermetic test environments and
self-host. The defaults Just Work for end users. Logging the env-var token
override is a basic safety net.

**Status.** Accepted. Implemented in M4.

---

## ADR-006 — Coordinate units are KiCad nm everywhere on the wire

**Context.** The backend could emit mm, mil, or nm. The existing v1 uses nm
throughout (`SCH_SYMBOL::SetPosition( VECTOR2I( posX, posY ) )` where posX is
nm).

**Decision.** **All coordinates on the wire are KiCad internal units (nm),
int32**. The harness asserts this. The backend's protocol doc reflects this.
1 mil = 25400 nm. 100 mil = 2540000 nm. Grid snap = 100 mil grid by default.

**Why.** No conversion layer = no bugs. The C++ side is already in nm; the
Python harness uses int and treats the magnitude correctly.

**Status.** Accepted.

---

## ADR-007 — SSE for streaming, JSON for one-shot

**Context.** Two transport modes already in v1.

**Decision.** Keep both:
- **SSE** for `/api/v1/generate` (the long-running design path with stages).
- **JSON POST/response** for `/api/v1/chat`, `/recommend`, `/plan` (short).

Do not introduce websockets — SSE is simpler, libcurl supports it natively,
and we only need server-to-client streaming.

**Why.** Symmetry with the backend's chosen transport (per the BUILD_PROMPT
note: "match what the backend offers"). SSE is already implemented.

**Status.** Accepted.

---

## ADR-008 — Python 3.8+ for the harness; stdlib-only where possible

**Context.** Harness language choice (per ADR-001).

**Decision.** Python ≥ 3.8 (`from __future__ import annotations` everywhere
for forward-ref ergonomics). **Stdlib-only for the runtime harness.**
Optional dev tools (`ruff`, `pytest`) installed via pip but not required for
`python scripts/assess.py` to run (we ship our own minimal test runner so the
default check works on a vanilla Python).

**Why.** No-deps means CI works on any machine with Python. `ruff` is a soft
gate (check 2 in §8 — not a hard gate).

**Status.** Accepted.
