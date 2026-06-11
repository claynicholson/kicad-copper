# PROTOCOL — wire contract between the kicad-copper plugin and the Copper backend

This document is the **single source of truth** for the JSON shapes and HTTP
semantics that the plugin (`eeschema/copper/copper_client.cpp`) and the
backend (`copper` / `copper-platform`) must both honor. Any change here MUST
be paired with code changes on both sides and a bumped `protocol_version` in
the response.

Current `protocol_version`: **1**.

## Coordinate units (ADR-006)

All `x`/`y` in this document are **KiCad internal units (int, nanometers)**.
- 1 mil = 25 400 nm
- 100 mil grid = 2 540 000 nm (default grid snap)
- KiCad page area is roughly ±10⁹ nm. Coordinates outside [-1e9, 1e9] are
  rejected by the plugin (see Response Validation below).

## Endpoints

Base URL (env > settings > default):
1. `$COPPER_API_URL` (env)
2. `EESCHEMA_SETTINGS::m_Copper.api_url`
3. `https://api.coppereda.com`

Auth header on every request:
```
Authorization: Bearer <oauth_access_token>
```

The plugin refreshes the access token (via `COPPER::AUTH::RefreshToken`) if
`IsAuthenticated()` returns false at request time.

| Method | Path                  | Intent     | Response shape                       |
|--------|-----------------------|------------|--------------------------------------|
| POST   | `/api/v1/chat`        | `chat`     | One JSON `CopperResponse`            |
| POST   | `/api/v1/recommend`   | `recommend`| One JSON `CopperResponse`            |
| POST   | `/api/v1/plan`        | `plan`     | One JSON `CopperResponse` (no ops)   |
| POST   | `/api/v1/generate`    | `generate` | SSE stream (text/event-stream)       |

## Request body (all four endpoints, identical shape)

```jsonc
{
  "prompt": "string — user's natural-language prompt",
  "intent": "chat" | "recommend" | "plan" | "generate",
  "context": {
    "components": [
      {
        "reference": "U1",
        "value": "RP2040",
        "lib_id": "MCU_RaspberryPi:RP2040",
        "position": { "x": 12700000, "y": 12700000 },
        "rotation": 0.0
      }
    ],
    "nets": [
      { "name": "VBUS", "connected_pins": ["U1:1", "J1:1"] }
    ],
    "power_rails": ["VBUS", "GND", "3V3"],
    "bounding_box": {
      "min": { "x": 0, "y": 0 },
      "max": { "x": 25400000, "y": 25400000 }
    },
    "free_position": { "x": 50800000, "y": 12700000 },
    "selected_refs": ["U1"]
  }
}
```

- `prompt`: non-empty UTF-8.
- `intent`: must match the endpoint (server may reject mismatch).
- `context`: required, may be empty (`components`/`nets`/etc. empty arrays).

## Response body — non-streaming (`/chat`, `/recommend`, `/plan`)

HTTP 200 + `Content-Type: application/json`:

```jsonc
{
  "protocol_version": 1,
  "success": true,
  "intent": "chat",          // echoes the request
  "message": "string — AI's natural-language reply, may be empty",
  "operations": [             // optional; absent or [] for /chat
    { "type": "PLACE_COMPONENT", "data": { ... } }
  ],
  "plan": {                   // optional; for /plan and /recommend
    "steps": [
      { "index": 0, "description": "Place RP2040 at (12.7mm, 12.7mm)" }
    ],
    "placement_info": "optional human-readable placement summary"
  },
  "erc": {                    // optional; null for chat/recommend
    "errors": 0,
    "warnings": 1,
    "messages": [
      { "severity": "warning", "code": "no_decoupling", "text": "...", "refs": ["U1"] }
    ]
  },
  "error": ""                 // empty on success
}
```

Failure (HTTP 200 with `success: false`, or HTTP 4xx/5xx):

```json
{
  "protocol_version": 1,
  "success": false,
  "intent": "generate",
  "message": "",
  "operations": [],
  "error": "Human-readable error.  Optional code field added later."
}
```

The plugin treats any of these as failure:
- HTTP status ≠ 200.
- `success == false`.
- JSON parse error.
- Response missing `protocol_version` or with `protocol_version > 1`.

## Response — streaming (`/generate`)

HTTP 200 + `Content-Type: text/event-stream`. Server-Sent Events per
[W3C EventSource](https://html.spec.whatwg.org/multipage/server-sent-events.html).

Event format:
```
event: stage
data: {"name":"choosing_parts","status":"active"}

event: message
data: {"text":"I picked an RP2040 and an LSM6DSO IMU."}

...

event: done
data: {"protocol_version":1,"success":true,"intent":"generate","operations":[...]}
```

Each event has:
- `event:` — one of `stage`, `message`, `plan`, `done`, `error`.
- `data:` — a JSON object (single-line, OR multi-line with each line
  prefixed by `data:` — the plugin concatenates with `\n` per spec).
- An empty line terminates the event.

### Event payloads

| `event` | `data` shape                                                          | Plugin behavior                                                  |
|---------|-----------------------------------------------------------------------|------------------------------------------------------------------|
| `stage` | `{"name": "string", "status": "pending"\|"active"\|"complete"\|"error"}` | Updates `COPPER_STAGE_PANEL` indicator for `name`.            |
| `message` | `{"text": "string"}`                                                | Appends an AI bubble to the chat.                                |
| `plan`  | `{"steps":[{"index":N,"description":"…"}],"placement_info":"…"}`      | Renders a `COPPER_PLAN_CARD`. User must Approve to apply.       |
| `done`  | Full `CopperResponse` (same shape as the non-streaming response).     | Final result. If `operations` present + plan was approved, apply.|
| `error` | `{"message": "string", "code": "optional"}`                           | Surfaces an error state in the panel; nothing applied.           |

### Stage names (canonical, not enforced)

The backend chooses names; current convention:
- `choosing_parts`
- `expanding_reference_circuits`
- `net_stitching`
- `placement`
- `wiring`
- `verifying`

The plugin renders whatever names arrive — it does not hardcode the list.

### Stream lifecycle

- Stages may arrive in any order; same `name` may go `pending → active → complete`.
- At most one `plan` event per stream.
- Exactly one `done` event terminates a successful stream.
- Exactly one `error` event terminates a failed stream (and there is no
  subsequent `done`).
- The HTTP body MAY end without a `done` if the connection drops — plugin
  treats this as a `connection_lost` error (different from server-sent `error`).

## Apply-plan: `Operation` shape (canonical)

The `operations` array in a `CopperResponse` is a list of these:

### `PLACE_COMPONENT`
```json
{
  "type": "PLACE_COMPONENT",
  "data": {
    "lib_id": "MCU_RaspberryPi:RP2040",
    "reference": "U1",
    "value": "RP2040",
    "x": 12700000,
    "y": 12700000,
    "rotation": 0.0
  }
}
```

- `lib_id`: required, non-empty, format `lib:symbol`.
- `reference`: required, non-empty, **unique within the plan**.
- `value`: required, may be the same as the symbol name.
- `x`, `y`: required, int, in `[-1e9, 1e9]`.
- `rotation`: optional, one of `0.0 / 90.0 / 180.0 / 270.0`.

### `ADD_WIRE`
```json
{
  "type": "ADD_WIRE",
  "data": { "start_x": 0, "start_y": 0, "end_x": 2540000, "end_y": 0 }
}
```

- All four coords required, int, in `[-1e9, 1e9]`.
- Zero-length wires are rejected.

### `ADD_LABEL`
```json
{
  "type": "ADD_LABEL",
  "data": { "name": "SDA", "x": 0, "y": 0, "label_type": "local" }
}
```

- `name`: required, non-empty, ≤ 64 chars.
- `label_type`: one of `local` / `global` / `hierarchical`. Default `local`
  if absent.

### `ADD_JUNCTION`
```json
{ "type": "ADD_JUNCTION", "data": { "x": 0, "y": 0 } }
```

### `ADD_POWER_SYMBOL`
```json
{
  "type": "ADD_POWER_SYMBOL",
  "data": { "net_name": "VCC", "x": 0, "y": 0 }
}
```

- `net_name`: non-empty. Must exist as a symbol in the `power:` library
  (`power:VCC`, `power:GND`, `power:+3V3`, …) — if absent, the op is rejected
  with an actionable error referencing the missing symbol.

### `ADD_PIN_LABEL`
```json
{
  "type": "ADD_PIN_LABEL",
  "data": { "reference": "U1", "pin": "SWCLK", "net_name": "SWCLK", "style": "global" }
}
```

The preferred connectivity mechanism: no coordinates cross the wire. The
plugin resolves the symbol placed earlier in the same plan (or already on the
current sheet) by `reference`, locates the pin, and drops the net marker
exactly on the pin's real position.

- `reference`: required. Must match a `PLACE_COMPONENT` in the same plan or a
  symbol on the current sheet.
- `pin`: required. Matched against the library symbol's pins in order: exact
  name, exact number, decoration-stripped name (`~{WP}(IO2)` → `WP`), any
  `/`-separated alias segment (`SDA/SDI/SDO` matches `SDI`). Single-pin
  symbols match any token.
- `net_name`: required, non-empty.
- `style`: `global` (default) → global label at the pin; `power` → instance
  of `power:<net_name>` placed pin-on-pin (rotated away from the host pin).

Rationale: the compiler's internal symbol geometry never matches the real
KiCad library symbols, so emitted wire/label coordinates landed in empty
space. Pin-anchored ops make the plugin the geometry authority. All nets are
global by name; no `ADD_WIRE` ops are needed for connectivity.

### `PLACEMENT_HINTS`
```json
{
  "type": "PLACEMENT_HINTS",
  "data": {
    "clusters": [
      { "module_id": "U1", "role": "mcu", "anchor_ref": "U1",
        "refs": ["C1", "U1", "Y1"], "flow_rank": 2 }
    ],
    "attachments": [
      { "ref": "C1", "to_ref": "U1", "to_pin": "IOVDD", "kind": "decap" }
    ]
  }
}
```

Advisory, never schematic-mutating, at most one per plan (emitted first).
The same split as `ADD_PIN_LABEL`: the backend compiles *meaning*, the
client compiles *geometry*. The plugin's refinement pass
(`copper_placement.cpp`) rewrites `PLACE_COMPONENT` coordinates against the
real library bboxes/pins before committing:

- `clusters[*].role`: one of `mcu | power | connector | peripheral |
  passive | other`. `flow_rank` orders left→right signal-flow columns
  (power 0, connector 1, mcu 2, peripherals 3). `anchor_ref` is the
  cluster's biggest part; support parts pack in a grid beside it.
- `attachments[*].kind`: `decap | pullup | pulldown | series`. The `ref`
  part is pulled out of cluster packing and parked one grid step off its
  host's REAL pin (`to_ref`/`to_pin`, matched with the `ADD_PIN_LABEL`
  rules), so decoupling caps hug the power pins they serve.

Clients that don't understand the op skip it and keep the backend's
fallback `PLACE_COMPONENT` coordinates. Backends MAY omit it entirely.

## Response validation (plugin-side, before apply)

Hard rejects (apply nothing, surface error):

1. `protocol_version` missing or > 1.
2. `success: false` or HTTP status ≠ 200.
3. `operations` present but not an array.
4. Any `op` missing `type` or with unknown `type`.
5. Any `PLACE_COMPONENT` with missing/empty `lib_id` or `reference`.
6. Any coordinate outside `[-1e9, 1e9]` or not int.
7. Two `PLACE_COMPONENT` ops with the same `reference`.
8. Any `ADD_WIRE` with `start == end`.
9. Any `ADD_LABEL` with `label_type` not in {local, global, hierarchical}.
10. Any `ADD_POWER_SYMBOL` with empty `net_name`.
11. Any `ADD_PIN_LABEL` with empty `reference`/`pin`/`net_name`, or `style`
    not in {global, power}. At apply time: unresolvable reference or pin →
    whole plan discarded (fail-closed).

(`PLACEMENT_HINTS` is never a hard reject: it is advisory and validated
loosely; malformed hints degrade to the backend's coordinate fallback.)

Soft warnings (apply with a banner):
- `protocol_version == 1` and unknown fields present → log + accept.
- `erc.warnings > 0` → show warning in panel but still allow Apply.

## HTTP errors → plugin states

| HTTP status / condition          | UI state                                       |
|----------------------------------|------------------------------------------------|
| 200 + `success: true`            | Normal flow.                                   |
| 200 + `success: false`           | Error bubble + `error` text.                   |
| 401                              | "Sign in again" button + clears tokens.        |
| 403                              | "You don't have access to this feature."       |
| 429                              | "Rate limited" + retry-after countdown.        |
| 408 / 504 / curl timeout         | "Backend timed out — retry."                   |
| 5xx                              | "Backend error — try again. [Report bug]"      |
| Network unreachable (curl != 0)  | "Offline — check connection. [Retry]"          |
| JSON parse error                 | "Bad response from backend — please report."   |
| `protocol_version` > 1           | "Update KiCad Copper — backend is newer."      |

## Cancellation

The plugin sets `m_cancelled = true` (in `COPPER::CLIENT`). For SSE the curl
write callback returns 0 (abort). For one-shot POSTs the plugin best-effort
times out on the next perform-step. After cancellation, no `done` event is
processed even if it had arrived.

## Versioning

- The `protocol_version` field is **required** in every response and SSE
  `done` payload starting from v1. Backends without it are rejected.
- Backward-compatible additions (new optional fields, new event types) do
  **not** bump the version — the plugin must tolerate unknown fields.
- Backward-incompatible changes (renamed fields, changed semantics,
  removed types) **do** bump the version.
- The plugin accepts `protocol_version == 1` exactly. Future plugins will
  accept their version and one below.

## Reference: matched files

- Plugin transport: [eeschema/copper/copper_client.cpp](../eeschema/copper/copper_client.cpp)
- Plugin types: [eeschema/copper/copper_types.h](../eeschema/copper/copper_types.h)
- Plugin apply: [eeschema/widgets/copper_chat_panel.cpp `ExecuteOperations`](../eeschema/widgets/copper_chat_panel.cpp)
- Plugin SSE parsing: [eeschema/copper/copper_client.cpp `sseWriteCallback`](../eeschema/copper/copper_client.cpp)
- Harness contract:  [copper_integration/backend_client.py](../copper_integration/backend_client.py)
- Harness apply:     [copper_integration/apply_engine.py](../copper_integration/apply_engine.py)
- Harness fakes:     [copper_integration/schematic_api.py](../copper_integration/schematic_api.py)
