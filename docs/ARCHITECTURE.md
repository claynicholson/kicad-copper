# ARCHITECTURE — components, threading, rollback policy

## 1. Two-language picture (per [DECISIONS.md](DECISIONS.md) ADR-001)

```
┌─────────────────────────────────────────────────────────────────────┐
│                        kicad-copper fork                             │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │  C++ PLUGIN  (compiled into eeschema)                       │    │
│  │                                                              │    │
│  │   COPPER_CHAT_PANEL  (wxPanel, in SCH_EDIT_FRAME)            │    │
│  │       │                                                      │    │
│  │       │  user prompt           CallAfter results            │    │
│  │       ▼                            ▲                         │    │
│  │   COPPER::CLIENT  ───────────────────────────── HTTPS ────→  │    │
│  │       │  (std::thread, libcurl SSE)                          │    │
│  │       │                                                      │    │
│  │       ▼  approved plan, main thread                          │    │
│  │   ExecuteOperations  →  SCH_COMMIT  →  SCHEMATIC              │    │
│  │                                                              │    │
│  │   HTTP_BRIDGE (127.0.0.1:9742) — side surface                │    │
│  │       └─ per-op REST endpoints for external tools           │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                                                                      │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │  PYTHON HARNESS  (copper_integration/, tests/, scripts/)    │    │
│  │                                                              │    │
│  │   BackendClient  ─────  validates request shape              │    │
│  │   StubBackend    ─────  serves canned protocol fixtures      │    │
│  │   ApplyEngine    ─────  models C++ ExecuteOperations         │    │
│  │   FakeSchematicApi  ──  in-memory mirror of SCH_COMMIT       │    │
│  │   Controller     ─────  orchestrates a headless e2e flow     │    │
│  │                                                              │    │
│  │   scripts/assess.py  ──  the §8 closed-loop self-test        │    │
│  └─────────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────────┘
                                   │
                                   ▼ HTTPS over the wire defined in PROTOCOL.md
                  ┌──────────────────────────────────┐
                  │  Copper backend (separate repo)  │
                  │  /api/v1/{chat,recommend,plan,   │
                  │           generate (SSE)}        │
                  └──────────────────────────────────┘
```

Both the C++ plugin and the Python harness consume the same [PROTOCOL.md](PROTOCOL.md).
The Python harness is the **contract test** — if it passes against the
backend (or StubBackend), the C++ plugin's transport will work too, because
they're parsing the same JSON shapes.

## 2. C++ component map

| Component                     | File                                                                                                                              | Purpose                                       |
|-------------------------------|-----------------------------------------------------------------------------------------------------------------------------------|-----------------------------------------------|
| `COPPER_CHAT_PANEL`           | [eeschema/widgets/copper_chat_panel.{h,cpp}](../eeschema/widgets/copper_chat_panel.cpp)                                          | Dockable AI chat UI; owns auth + client       |
| `COPPER_*` widgets            | [eeschema/widgets/copper_chat_widgets.{h,cpp}](../eeschema/widgets/copper_chat_widgets.cpp)                                      | Message bubble, plan card, stage indicators   |
| `COPPER::AUTH`                | [eeschema/copper/copper_auth.{h,cpp}](../eeschema/copper/copper_auth.cpp)                                                        | OAuth via existing PKCE + keychain            |
| `COPPER::CLIENT`              | [eeschema/copper/copper_client.{h,cpp}](../eeschema/copper/copper_client.cpp)                                                    | HTTPS + SSE transport, threaded               |
| `COPPER::*` types             | [eeschema/copper/copper_types.h](../eeschema/copper/copper_types.h)                                                              | Request/response/op shapes                    |
| `HTTP_BRIDGE`                 | [eeschema/api/http_bridge.{h,cpp}](../eeschema/api/http_bridge.cpp)                                                              | localhost REST for external tools             |
| `API_HANDLER_SCH`             | [eeschema/api/api_handler_sch.{h,cpp}](../eeschema/api/api_handler_sch.cpp)                                                      | IPC/protobuf bus for existing KiCad API       |
| `SCH_CONTEXT` / `HEADLESS_*`  | [eeschema/api/sch_context.h](../eeschema/api/sch_context.h), [eeschema/api/headless_sch_context.h](../eeschema/api/headless_sch_context.h) | Abstract base for in-frame vs headless |

## 3. Python harness component map

| Component                  | File                                                          | Purpose                                                       |
|----------------------------|---------------------------------------------------------------|---------------------------------------------------------------|
| `SchematicApi` / `FakeSchematicApi` | `copper_integration/schematic_api.py`                  | Mutation surface + in-memory impl                              |
| `BackendClient`            | `copper_integration/backend_client.py`                        | HTTPS POST + SSE parser; validates PROTOCOL shapes             |
| `StubBackend`              | `copper_integration/stub_backend.py`                          | Plays canned fixture streams; configurable error injection     |
| `validators`               | `copper_integration/validators.py`                            | The PROTOCOL.md "Hard rejects" list                            |
| `ApplyEngine`              | `copper_integration/apply_engine.py`                          | Plan → ordered Fake API calls in one commit; rollback on error |
| `Controller`               | `copper_integration/controller.py`                            | Orchestrates the end-to-end flow                               |
| `Settings`                 | `copper_integration/settings.py`                              | Env > settings > default resolution                            |
| `assess.py`                | `scripts/assess.py`                                           | Closed-loop self-assessment harness                            |
| Fixtures                   | `fixtures/*.json` / `*.sse`                                   | Canned streams: happy, error, malformed, partial               |

## 4. Threading & rollback (the §6 baseline)

### C++

- **Network thread:** `m_workerThread` (one std::thread per
  `COPPER::CLIENT::doPost` or `doStreamPost`). Wakes only on its own.
  Talks to libcurl. Never touches wx UI or `SCHEMATIC` objects directly.
- **Main thread:** every result handler is bounced through `CallAfter`. The
  panel only touches `m_messageSizer`, plan cards, and `SCH_COMMIT` from the
  main thread.
- **Rollback:** one `SCH_COMMIT` per applied plan. If validation fails (per
  ADR-004 fail-closed), the commit is never pushed → schematic unchanged. If
  a per-op construction throws, the commit destructor still cleans up → no
  partial state.
- **Cancellation:** sets `m_cancelled` atomic; SSE callback returns 0; one-shot
  perform exits at its next checkpoint. UI re-enables Send.

### Python (harness)

- All synchronous by default. Threading only used to model the C++ split
  when a test specifically exercises it (rare; we instead use an
  injected-clock fake).
- Rollback in `FakeSchematicApi`: every call within a `commit_scope()` is
  buffered into a pending batch; `push()` atomically appends to the
  committed log, while `abort()` discards the batch. `undo()` pops the last
  commit. This mirrors KiCad's `SCH_COMMIT::Push()` / destructor semantics.

## 5. State machine — the in-editor chat flow

```
              ┌─────────────────────────────┐
              │  IDLE (input enabled)        │
              └──────────────┬───────────────┘
                  user types  │
                  + sends     ▼
              ┌─────────────────────────────┐
              │  SENDING (input disabled)    │
              └──────────────┬───────────────┘
       stage events│  done    │  error / offline / 401 / 5xx
       ──────────▶ │          │  ────────────────────────────▶ ERROR_STATE
                   ▼          │
              ┌──────────────────────────────┐
              │ STREAMING (stages animate)   │
              └──────────────┬───────────────┘
                  plan event │
                             ▼
              ┌──────────────────────────────┐
              │  PLAN_PRESENTED              │
              │  (Apply / Apply to new sheet │
              │   / Cancel buttons live)     │
              └──────┬───────────────┬───────┘
              Apply │               │ Cancel
                    ▼               ▼
       ┌────────────────────┐  ┌───────────────────────┐
       │ VALIDATING (sync)  │  │  IDLE (no apply done) │
       └──────────┬─────────┘  └───────────────────────┘
       valid │           │ invalid
             ▼           ▼
       ┌──────────────┐  ┌─────────────────────────────┐
       │  APPLYING    │  │  ERROR_STATE                │
       │  (one        │  │  ("Backend sent bad plan,   │
       │   SCH_COMMIT)│  │    nothing applied. [Retry]")│
       └──────┬───────┘  └─────────────────────────────┘
              │ Push() succeeds
              ▼
       ┌──────────────────────────────┐
       │  APPLIED  (Undo available)   │
       └──────────────────────────────┘
```

## 6. Boundary contracts (the seams that keep both languages honest)

### 6a. `SchematicApi` seam (M1)

Methods both the C++ implementation (`ExecuteOperations`) and the Python
`FakeSchematicApi` honor:

```
begin_commit(label: str)            -> CommitToken
abort_commit(token)
push_commit(token)                  -> CommitId
undo()                              -> CommitId | None

# Within a commit:
place_component(token, op_data)     -> SymbolId
add_wire(token, op_data)            -> WireId
add_label(token, op_data)           -> LabelId
add_junction(token, op_data)        -> JunctionId
add_power_symbol(token, op_data)    -> SymbolId

# Read-only:
list_symbols()                      -> [Symbol]
list_wires()                        -> [Wire]
list_labels()                       -> [Label]
serialize()                         -> bytes      # for byte-equality tests
```

Invariants:
- All op data **validated** before the SchematicApi call.
- `abort_commit` after any number of within-commit calls leaves
  `serialize()` byte-identical to pre-`begin_commit`.
- `push_commit` is atomic — observers see all-or-nothing.
- `undo()` after a `push_commit` reverses *the entire* commit.

### 6b. `BackendClient` seam (M2)

```
generate(prompt, context) -> Iterator[Event]
chat(prompt, context)     -> CopperResponse
recommend(prompt, context)-> CopperResponse
plan(prompt, context)     -> CopperResponse
```

`Event` is a typed dataclass per the PROTOCOL.md SSE table.

Behaviors guaranteed by the harness:
- Auth header attached, base URL resolved per ADR-005.
- HTTP error → `BackendHttpError(status, body)`.
- Network error → `BackendNetworkError(cause)`.
- Schema error → `BackendSchemaError(detail)` (and not raised mid-stream
  unless `done` payload was malformed — partial stages are not validated).

## 7. Security boundary (also in [SECURITY.md](SECURITY.md))

- Tokens never on disk in cleartext. OS keychain via `SECURE_TOKEN_STORE` in
  C++; for Python tests we use an in-memory store only.
- No tokens, URLs, or fixtures in this repo embed real credentials.
  `COPPER_API_TOKEN` env var is the documented dev-only override; the
  C++ plugin logs a warning if it's used.
- All response data is validated before it crosses the apply boundary.
- No write to `.kicad_sch` files outside of `SCH_COMMIT::Push()`.

## 8. Definition of "the plugin is loadable" (check 1 in §8)

For the C++ side: the listed files are in CMake, the headers are
syntactically valid, the panel is instantiated by `SCH_EDIT_FRAME`. We can't
prove "compiles" in this session (would require building KiCad), but we can
prove "consistent" via grep-level checks the harness performs (referenced
classes exist, forward decls match definitions, CMake adds the files).

For the Python side: `python -c "import copper_integration"` returns 0; all
modules can be imported; `Controller`, `BackendClient`, `ApplyEngine`, and
`FakeSchematicApi` are reachable. This is check 1.
