# FORK_SURFACE — what the kicad-copper fork already provides

Result of the §0 audit in BUILD_PROMPT.md. This is the surface that the integration
sits on top of. It is **C++** (KiCad is C++), inside the fork's source tree, not a
Python plugin — see [DECISIONS.md](DECISIONS.md) ADR-001 for why the BUILD_PROMPT's
Python-plugin layout was overridden.

## 1. Chat panel (existing UI)

- **File:** [eeschema/widgets/copper_chat_panel.h](../eeschema/widgets/copper_chat_panel.h),
  [eeschema/widgets/copper_chat_panel.cpp](../eeschema/widgets/copper_chat_panel.cpp).
- **Toolkit:** native wxWidgets — `wxPanel` derived (`COPPER_CHAT_PANEL : public wxPanel`).
  Not HTML/webview. Lives in-process inside `eeschema`.
- **Children:** header (title + login + ⚙), `wxScrolledWindow` message area,
  `wxChoice` mode (Design/Chat/Recommend) + `wxTextCtrl` input + Send button.
- **Custom widgets** in [eeschema/widgets/copper_chat_widgets.h](../eeschema/widgets/copper_chat_widgets.h):
  `COPPER_MESSAGE_BUBBLE` (USER/AI), `COPPER_PLAN_CARD` (approve / edit),
  `COPPER_STAGE_INDICATOR` (PENDING / ACTIVE / COMPLETE / ERROR),
  `COPPER_STAGE_PANEL`, `COPPER_HINT_CHIP`. Color tokens in `COPPER_COLORS`.
- **Custom wx events the panel fires/listens to:**
  `COPPER_EVT_AUTH_SUCCESS`, `COPPER_EVT_AUTH_FAILURE`,
  `COPPER_EVT_PLAN_APPROVED`, `COPPER_EVT_PLAN_EDITED`,
  `COPPER_EVT_HINT_CLICKED`.
- **Hooks for streaming / incremental updates:** the panel calls
  `m_client->Generate(prompt, ctx, onEvent, onError)` and receives
  `COPPER::SSEEvent` callbacks. Inside `handleSSEEvent` it dispatches:
  - `event == "stage"`  → `m_stagePanel->UpdateStage(name, State)` (live progress)
  - `event == "message"` → `addAIMessage(text)` (incremental text bubble)
  - `event == "plan"`   → `addPlanCard(response)` (interactive approval card)
  - `event == "done"`   → final `handleResponse(resp)` (full result)
  - `event == "error"`  → `handleError(msg)`
- **How callbacks return to the UI thread:** the worker thread calls
  `CallAfter([this, evt]{ handleSSEEvent(evt); })`. This is wxWidgets' standard
  cross-thread dispatch — guaranteed to run the lambda on the main thread.
- **Launch / docking:** *currently not instantiated anywhere*. See
  [INTEGRATION_V1_AUDIT.md](INTEGRATION_V1_AUDIT.md) gap G-FRAME — needs to be
  added to `SCH_EDIT_FRAME` as an `AuiPaneInfo` dockable side panel.

## 2. Auth surface

- **File:** [eeschema/copper/copper_auth.h](../eeschema/copper/copper_auth.h),
  [eeschema/copper/copper_auth.cpp](../eeschema/copper/copper_auth.cpp).
- **Class:** `COPPER::AUTH : public wxEvtHandler`. OAuth via the existing KiCad
  infrastructure: `OAUTH_SESSION` (PKCE), `OAUTH_LOOPBACK_SERVER` (browser
  callback), `SECURE_TOKEN_STORE` (OS keychain). These exist in
  [common/oauth/](../common/oauth/) + [include/oauth/](../include/oauth/) and
  are already built into `common`. The token store uses provider id `copper`.
- **API:** `StartLogin(wxEvtHandler*)`, `LoadSavedTokens()`, `RefreshToken()`,
  `Logout()`, `IsAuthenticated()`, `GetAccessToken()`, `GetUserEmail()`,
  `GetApiUrl()`, `SetApiUrl()`.

## 3. Backend transport (existing v1)

- **File:** [eeschema/copper/copper_client.h](../eeschema/copper/copper_client.h),
  [eeschema/copper/copper_client.cpp](../eeschema/copper/copper_client.cpp).
- **Class:** `COPPER::CLIENT`. Wraps `KICAD_CURL_EASY` (KiCad's libcurl wrapper
  from `thirdparty/`). Uses `std::thread` for background work + `std::atomic<bool>`
  for `m_busy` / `m_cancelled`.
- **Endpoints already coded:**
  | Method | Path | Mode | Return |
  |--------|------|------|--------|
  | POST   | `/api/v1/chat`      | non-streaming | `CopperResponse` JSON |
  | POST   | `/api/v1/recommend` | non-streaming | `CopperResponse` JSON |
  | POST   | `/api/v1/plan`      | non-streaming | `CopperResponse` JSON |
  | POST   | `/api/v1/generate`  | **SSE stream** | events: `stage`, `message`, `plan`, `done`, `error` |
- **Auth header:** `Authorization: Bearer <access_token>`, refreshed if expired.
- **SSE parser:** inline in `sseWriteCallback` — buffers partial lines, splits
  on `\n`, strips `\r`, parses `event: <type>` and `data: <payload>` fields,
  dispatches per blank-line boundary. Sound; documented in [PROTOCOL.md](PROTOCOL.md).
- **Cancellation:** `m_cancelled.store(true)` aborts the `sseWriteCallback` by
  returning 0 (curl interprets as abort).

## 4. Schematic-context extraction

- **Where:** `COPPER_CHAT_PANEL::ExtractContext()` — reads current `SCHEMATIC`
  via `m_frame->Schematic()`. Walks `screen->Items()` for `SCH_SYMBOL_T`,
  builds `COPPER::ContextComponent` (ref, value, lib_id, position, rotation).
- **Nets:** via `SCHEMATIC::ConnectionGraph()` → `GetNetMap()`. For each net,
  collects `connected_pins` (formatted `REF:PINNUM`) and flags power rails by
  `SCH_CONNECTION::IsPowerConnection()`.
- **Geometry:** bounding box of all items + a "next free position" snapped to
  100 mil (2.54 mm = 2540000 nm) grid, 10 grid units right of existing content.
  Empty-schematic default: (40 grid, 30 grid).
- **Selection:** `m_frame->GetCurrentSelection()` for any selected symbol refs.
- **Units:** KiCad internal units (nm). 1 mil = 25400 nm. 100 mil = 2.54 mm = 2540000 nm.

## 5. Schematic mutation API (the "fork's API" referenced in §0.2)

Two surfaces — both reach the same KiCad data structures, the difference is
caller context.

### 5a. In-process (used by `COPPER_CHAT_PANEL::ExecuteOperations`)

Already implemented in
[copper_chat_panel.cpp lines 797–913](../eeschema/widgets/copper_chat_panel.cpp).
Each call goes through a `SCH_COMMIT` — KiCad's native undo-capable transaction.

| Op type            | Inputs                                                        | Object created     |
|--------------------|---------------------------------------------------------------|--------------------|
| `PLACE_COMPONENT`  | `lib_id`, `reference`, `value`, `x`, `y`                      | `SCH_SYMBOL`       |
| `ADD_WIRE`         | `start_x`, `start_y`, `end_x`, `end_y`                        | `SCH_LINE` (LAYER_WIRE) |
| `ADD_LABEL`        | `name`, `x`, `y`, `label_type` ∈ {local, global, hierarchical} | `SCH_LABEL` / `SCH_GLOBALLABEL` / `SCH_HIERLABEL` |
| `ADD_JUNCTION`     | `x`, `y`                                                      | `SCH_JUNCTION`     |
| `ADD_POWER_SYMBOL` | `net_name`, `x`, `y`                                          | `SCH_SYMBOL` from `power:` lib |

All ops in a single `ExecuteOperations()` call are added to **one** `SCH_COMMIT`
and pushed with one label (`_("Copper AI: Execute plan")`) — so a single Ctrl-Z
undoes the entire applied plan. This is the §6 atomic-rollback property,
already enforced by KiCad's commit system. **Failure mode:** if any individual
`new SCH_LINE(...)` throws, the commit is destroyed without pushing, leaving
the schematic untouched. (No partial state.)

### 5b. Out-of-process via HTTP (the `HTTP_BRIDGE`)

[eeschema/api/http_bridge.h](../eeschema/api/http_bridge.h) +
[eeschema/api/http_bridge.cpp](../eeschema/api/http_bridge.cpp). Lightweight
HTTP server on `127.0.0.1:9742` (default). Exposes the same operations as REST
endpoints — meant for external tools / tests / the copper backend if needed.

| Method | Path | Behavior |
|--------|------|----------|
| GET    | `/api/schematic/info`               | sheet count, paper, title block |
| GET    | `/api/schematic/components`         | list of symbols |
| GET    | `/api/schematic/nets`               | list of nets + connected pins |
| GET    | `/api/schematic/power-rails`        | power net names |
| GET    | `/api/schematic/wires`              | wire segments |
| GET    | `/api/schematic/labels`             | local/global/hierarchical labels |
| GET    | `/api/schematic/bounding-box`       | bbox in nm |
| GET    | `/api/schematic/next-free-position` | grid-snapped free position |
| POST   | `/api/schematic/place-symbol`       | one `SCH_COMMIT` per call |
| POST   | `/api/schematic/draw-wire`          | one `SCH_COMMIT` per call |
| POST   | `/api/schematic/add-label`          | one `SCH_COMMIT` per call |
| POST   | `/api/schematic/add-junction`       | one `SCH_COMMIT` per call |
| POST   | `/api/schematic/refresh`            | repaint canvas + reconnect |
| POST   | `/api/schematic/undo`               | runs `ACTIONS::undo` |
| POST   | `/api/schematic/redo`               | runs `ACTIONS::redo` |

Note: each POST endpoint pushes its **own** `SCH_COMMIT`, so an external caller
that places 10 symbols via 10 POSTs gets 10 undo entries (one per op). For the
in-editor chat flow we use 5a (one commit per plan) — that is the correct UX.

Also note: `HTTP_BRIDGE::handleClient` runs on the wx main thread (it is a
`wxEvtHandler` socket callback), so all mutations done via the bridge are
already on the right thread. Performance penalty is acceptable — the bridge
isn't on the hot path for the in-editor flow.

## 6. Headless schematic context

[eeschema/api/headless_sch_context.h](../eeschema/api/headless_sch_context.h)
provides `HEADLESS_SCH_CONTEXT : public SCH_CONTEXT` — an `SCH_EDIT_FRAME`-less
context that holds a `SCHEMATIC*` + `PROJECT*` + its own `TOOL_MANAGER`. This
is the existing fork's seam for running schematic operations outside the GUI
(`CanAcceptApiCommands() == true`). The IPC API handler (`API_HANDLER_SCH`)
already accepts this context, gated by `KICAD_IPC_API`.

Implication: the §4 `SchematicApi` interface (M1) already exists at the C++
abstract-base level as `SCH_CONTEXT`. We extend, not replace.

## 7. Threading model (constraints)

- **All wx UI mutations** — adding bubbles, plan cards, stage updates — must
  happen on the **main thread**. The existing v1 enforces this via
  `CallAfter([...]{ ... })` from worker-thread callbacks.
- **All schematic mutations** (anything that creates a `SCH_COMMIT`) must also
  happen on the main thread. KiCad's `SCH_COMMIT::Push()` notifies undo
  listeners and triggers canvas refresh — both wx-thread-affine.
- **Network / curl I/O** runs on `m_workerThread` (a `std::thread` owned by
  `COPPER::CLIENT`). Pattern: kick off thread, do blocking curl, post results
  back via `CallAfter` from the chat panel's lambda.
- **HTTP_BRIDGE** is a wx socket handler — main-thread by construction.
- Threading constraint summary: **network = background, everything else = main**.

## 8. Build wiring — current state

| Component                              | In CMake?            | Instantiated?       |
|----------------------------------------|----------------------|---------------------|
| `api/api_handler_sch.cpp`              | Yes (KICAD_IPC_API)  | Yes (IPC bus)       |
| `api/headless_sch_context.cpp`         | Yes (KICAD_IPC_API)  | Yes (IPC bus)       |
| `api/sch_context.cpp`                  | Yes (KICAD_IPC_API)  | Yes (IPC bus)       |
| **`api/http_bridge.cpp`**              | **No** (gap)         | **No** (gap)        |
| **`copper/copper_auth.cpp`**           | **No** (gap)         | (Panel creates it)  |
| **`copper/copper_client.cpp`**         | **No** (gap)         | (Panel creates it)  |
| **`widgets/copper_chat_widgets.cpp`**  | **No** (gap)         | (Panel children)    |
| **`widgets/copper_chat_panel.cpp`**    | **No** (gap)         | **No** (gap)        |

Five files need to be added to [eeschema/CMakeLists.txt](../eeschema/CMakeLists.txt).
The panel itself needs an instantiation site in `SCH_EDIT_FRAME` and a
settings field (`EESCHEMA_SETTINGS::m_Copper.api_url`).
Tracked as gaps in [INTEGRATION_V1_AUDIT.md](INTEGRATION_V1_AUDIT.md).

## 9. KiCad version + language

- **Language:** C++17 (matches the rest of KiCad).
- **wx:** wxWidgets 3.2+ (KiCad's minimum).
- **JSON:** `nlohmann/json` (vendored in `thirdparty/`).
- **HTTP:** `KICAD_CURL_EASY` (libcurl wrapper, vendored).
- **OAuth:** native `OAUTH_SESSION` etc. in `common/oauth/`.
- **Plugin model:** there is no plugin model. The fork **modifies** KiCad's
  source directly. The integration is therefore not loaded — it is compiled in.
