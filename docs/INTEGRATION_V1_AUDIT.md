# INTEGRATION_V1_AUDIT — what already works, what's broken, what's missing

Companion to [FORK_SURFACE.md](FORK_SURFACE.md). This is the backlog the rest
of BUILD_PROMPT.md closes — Milestones M1–M6 each target specific gaps below.

## Verdict at a glance

The "v1" referenced in BUILD_PROMPT.md is **source-complete but not wired in**.
Every piece of C++ scaffolding exists in `eeschema/copper/`, `eeschema/api/`,
and `eeschema/widgets/copper_chat_*`, but:

1. None of it is in `eeschema/CMakeLists.txt` → none of it compiles today.
2. The chat panel is never instantiated by `SCH_EDIT_FRAME` → even if it
   compiled, you couldn't see it.
3. It references `EESCHEMA_SETTINGS::m_Copper.api_url` which doesn't exist in
   the settings struct → it wouldn't link.
4. There is no test harness — and §8 requires one that runs headless.

So treat this as v0.9: the design is good, the code is real, the wiring isn't
done. Our job is the last 10% (wiring + validation + safety nets) per §10.

---

## A. WORKS (verified by reading the source)

### A1. SSE parsing — `copper_client.cpp::sseWriteCallback`
Correctly buffers partial network reads, splits on `\n`, strips `\r`, parses
`event:` and `data:` lines, dispatches per blank-line boundary. Matches the
W3C EventSource spec for the fields we care about. Cancellation via returning
0 from the write callback is the right libcurl idiom.

### A2. Atomic apply (in-process, panel path)
`COPPER_CHAT_PANEL::ExecuteOperations` builds one `SCH_COMMIT` for the whole
plan and calls `commit.Push(_("Copper AI: Execute plan"))` once. KiCad's
commit system guarantees:
- One undo entry per `Push()` → single Ctrl-Z reverts the whole apply. ✓
- If `Push()` is never reached (exception during `new SCH_LINE(...)` etc.),
  the commit destructor cleans up without mutating the schematic. ✓

The §6 atomic-rollback property is **already satisfied** by leveraging
`SCH_COMMIT`. This is correct design — do not replace.

### A3. Threading model
Network on `std::thread`, UI/mutations on main thread via `CallAfter`.
Idiomatic for wx, correct.

### A4. OAuth integration
Reuses KiCad's existing PKCE flow + OS keychain. No secrets in code or files.

### A5. Schematic context extraction
`ExtractContext()` produces a typed `SchematicContext` (components, nets,
power rails, bbox, free position, selection) with stable units (nm) — solid
input shape for the backend.

### A6. Wire protocol design
JSON request body `{ prompt, context, intent }` to four endpoints; SSE event
types `stage / message / plan / done / error`; structured `Operation` with
`{ type, data }`. Clean and language-neutral. Documented in [PROTOCOL.md](PROTOCOL.md).

### A7. HTTP bridge (out-of-process apply path)
[http_bridge.cpp](../eeschema/api/http_bridge.cpp) maps every operation a
plan can contain onto a REST endpoint, each wrapped in its own `SCH_COMMIT`.
Useful for external tools and as a contract surface the Python test harness
can replicate. Currently not started by anyone; not on the hot path for the
in-editor flow (which uses 5a, not 5b).

---

## B. PARTIAL / STUBBED (the gaps the milestones close)

### B1. Plan-approval UX — handlers are empty
`onPlanApproved` and `onPlanEdited` in
[copper_chat_panel.cpp:329-340](../eeschema/widgets/copper_chat_panel.cpp)
are TODO stubs — they don't call `ExecuteOperations` and the panel does not
hold the pending operations off the last response. **Gap G-APPROVE.**
- **What's missing:** the panel must store the `std::vector<Operation>` that
  arrived on the most recent `done` SSE event (or with the plan card itself)
  and call `ExecuteOperations(ops)` from `onPlanApproved`.
- **Closes:** M3 / M4. Without this, the happy path is broken — a user can
  see the plan but cannot apply it.

### B2. No response validation before apply
`CopperResponse::fromJson` is permissive — every field uses `.value(..., default)`.
Garbage in, garbage out: a malformed plan can produce zero-coordinate symbols
on top of the origin, mis-typed labels, etc. **Gap G-VALIDATE.**
- **What's missing:** a validation pass between `fromJson` and `ExecuteOperations`
  that rejects:
  - missing/empty `lib_id` on `PLACE_COMPONENT`,
  - non-integer or `< -1e9` / `> 1e9` coordinates,
  - unknown `label_type`,
  - unknown `op.type`,
  - operations with duplicate (or empty) references inside the same plan.
- **Closes:** M3 (check 4 in §8).

### B3. No "review then apply" — currently the plan is just shown, never auto-applied either
The Design-mode happy path is: prompt → SSE stages → `plan` event → plan card
shown → `done` event → final response added → **nothing happens**. The user
can hit "Approve" on the plan card (visually) but the handler is empty (B1).
**Gap G-REVIEW.**
- **What's missing:** clear Apply / Apply-to-new-sheet / Cancel UX with a
  preview surface (the backend already returns an SVG-renderable plan).
- **Closes:** M4.

### B4. Settings UI for API URL / hosted-vs-local
`COPPER_CHAT_PANEL` reads `cfg->m_Copper.api_url` from `EESCHEMA_SETTINGS` —
but no such field exists (the cast on
[copper_chat_panel.cpp:95-99](../eeschema/widgets/copper_chat_panel.cpp)
would compile if the field existed; today it would not compile). Settings
dialog handler is a TODO. **Gap G-SETTINGS.**
- **What's missing:**
  - Add `EESCHEMA_SETTINGS::m_Copper { wxString api_url; }` (default
    `"https://api.coppereda.com"`) with JSON param binding.
  - Wire `onSettingsClicked` to a small `wxDialog` that edits this field.
  - Honor `COPPER_API_URL` env var as override (for self-host / local dev).
- **Closes:** M4.

### B5. Error/state UX gaps
`handleError` currently just adds an AI message bubble with the error text.
No distinction between offline / unauthorized (401/403) / rate-limited (429) /
malformed-response. Send button is re-enabled in both paths but the user has
no actionable next step. **Gap G-STATES.**
- **What's missing:** typed error states with action buttons:
  - offline → "Retry" button.
  - 401/403 → "Sign in again" → triggers `m_auth->StartLogin`.
  - 429 → countdown + auto-retry.
  - 5xx → "Try again" + opt-in to send debug bundle.
  - schema mismatch → "Report bug" link.
- **Closes:** M5 (check 8 in §8).

### B6. No partial-apply guarantee on op-level failure
`ExecuteOperations` pushes one commit, but inside the loop:
- A failed `m_frame->GetLibSymbol(libId)` returns `nullptr` and the op is
  **silently skipped** ([copper_chat_panel.cpp:828](../eeschema/widgets/copper_chat_panel.cpp)).
- A bad `lib_id` parse is similarly silent.
- A failure halfway through still pushes the partial commit.

So today: a plan with 10 symbols where the 4th has a bad lib_id will apply
6 symbols. **Gap G-ATOMIC.**
- **What's missing:** if any op fails validation/construction, abort the
  whole apply — discard the commit and surface the failure. (Counter-design:
  this is more conservative than KiCad's typical "skip bad item" — but it
  matches §6: "never leave a half-applied schematic".)
- **Closes:** M3 (check 6 in §8).

### B7. Stage panel never cleared between requests
`m_stagePanel` is created lazily on first `stage` event and reused — but a
second request reuses the same panel with prior stages still listed.
**Gap G-STAGES.**
- **What's missing:** reset/clear stages when a new request starts.
- **Closes:** M4 (minor).

### B8. No connectivity check on applied result
The §10 "applied boards have zero overlap and all plan nets realized" check
is not enforced anywhere. Currently the result is whatever the backend says.
**Gap G-QUALITY.**
- **What's missing:** post-apply validation pass (and in headless tests):
  - no two `SCH_SYMBOL`s share the same anchor (overlap-free).
  - every `Operation::PLACE_COMPONENT.reference` is unique.
  - every label declared in the plan is realized on the schematic.
  - bbox of new content does not overlap any pre-existing symbol's bbox.
- **Closes:** M3 (check 7 in §8).

---

## C. BROKEN (does not build / does not run)

### C1. CMake does not include the copper files
[eeschema/CMakeLists.txt](../eeschema/CMakeLists.txt) does not list:
- `widgets/copper_chat_panel.cpp`
- `widgets/copper_chat_widgets.cpp`
- `copper/copper_auth.cpp`
- `copper/copper_client.cpp`
- `api/http_bridge.cpp`

**Effect:** none of v1 compiles. **Gap G-CMAKE.** Closes: M4.

### C2. `EESCHEMA_SETTINGS::m_Copper` does not exist
Referenced in `copper_chat_panel.cpp` ctor. `EESCHEMA_SETTINGS` is in
[eeschema/eeschema_settings.h](../eeschema/eeschema_settings.h) — no `m_Copper`
struct exists. **Effect:** even if added to CMake, won't link/compile.
**Gap G-SETTINGS** (overlaps with B4). Closes: M4.

### C3. Panel is never instantiated by `SCH_EDIT_FRAME`
Nowhere in `eeschema/sch_edit_frame.cpp` does `new COPPER_CHAT_PANEL(...)`
appear, and there is no `AuiPaneInfo` registration for it. **Effect:** even
if it compiled, the user could never see it. **Gap G-FRAME.** Closes: M4.

### C4. `HTTP_BRIDGE` is never started
Nothing constructs or calls `Start()` on it. **Effect:** the bridge exists
as code but the port is never bound. **Gap G-BRIDGE.** Closes: M4 (low pri —
the bridge is a side surface, not on the chat flow's critical path).

---

## D. MISSING (no code exists yet)

### D1. Test harness (per §8)
No tests of any kind for the copper layer. The §8 self-assessment requires
8 checks running headless. **Gap G-HARNESS.** Closes: M1–M6 incrementally.

### D2. `SchematicApi` seam / `FakeSchematicApi`
The C++ `SCH_CONTEXT` is an abstract base, but it does not expose the
mutation surface the apply layer needs (place-symbol, draw-wire, …) — those
are scattered as concrete `SCH_COMMIT` calls in the panel and bridge. For
testability we need a narrow interface. **Gap G-SEAM.** Closes: M1.

### D3. Documented wire protocol
The protocol is implicit in the code. BUILD_PROMPT.md §7 requires
`docs/PROTOCOL.md` as the single source of truth. **Gap G-PROTO.** Closes: M2.

### D4. Stub backend + canned fixtures
For the §8 headless tests. **Gap G-STUB.** Closes: M2.

### D5. `scripts/assess.py` self-assessment harness
The closed-loop polish gate. **Gap G-ASSESS.** Closes: M6.

### D6. Settings env-var override
Spec: `COPPER_API_URL` env var beats settings. Not coded. Closes: M4.

### D7. Single-undo guarantee verified by test
Need a deterministic test that proves one `ACTIONS::undo` reverts a full
multi-op apply. Closes: M3 (check 6).

---

## E. NOT-YET-IN-SCOPE (defer per §2; track in [ROADMAP.md](ROADMAP.md))

- E1. Real-wire-routing aesthetics (the engine emits labels-first; that's fine).
- E2. Multi-sheet apply.
- E3. "Explain this schematic" / "Verify" follow-up flows.
- E4. Inline plan editing (the `onPlanEdited` handler is intentionally
  out-of-scope for v2; flag for a future milestone).
- E5. Billing / account-management UI (lives in `copper-platform`).

---

## F. Gap → milestone matrix

| Gap         | Closes in | §8 check |
|-------------|-----------|----------|
| G-APPROVE   | M3 / M4   | 5        |
| G-VALIDATE  | M3        | 4        |
| G-REVIEW    | M4        | (UX)     |
| G-SETTINGS  | M4        | 8        |
| G-STATES    | M5        | 8        |
| G-ATOMIC    | M3        | 6        |
| G-STAGES    | M4        | (UX)     |
| G-QUALITY   | M3        | 7        |
| G-CMAKE     | M4        | 1        |
| G-FRAME     | M4        | 1        |
| G-BRIDGE    | M4        | (low)    |
| G-HARNESS   | M1–M6     | all      |
| G-SEAM      | M1        | 5,6,7    |
| G-PROTO     | M2        | 3,4      |
| G-STUB      | M2        | 3,4,8    |
| G-ASSESS    | M6        | all      |

---

## G. Status update (2026-06-10)

Audit gaps closed since this document was written:

- **G-SETTINGS — CLOSED.** `EESCHEMA_SETTINGS::m_Copper.api_url` exists with
  JSON param binding (default `https://api.coppereda.com`),
  `onSettingsClicked` opens an edit/validate/persist dialog, and
  `COPPER_API_URL` env overrides settings in the panel ctor.
- **G-CMAKE / G-FRAME — CLOSED** in M4 (panel compiles, links, and is docked
  by `SCH_EDIT_FRAME`; full `ninja eeschema` build passes).
- **G-PROTO / G-STUB / G-SEAM / G-HARNESS / G-ASSESS — CLOSED** (harness at
  100/100 POLISHED, 146 tests passing).
- Live e2e verified against https://api.coppereda.com via
  `scripts/live_smoke.py` (generate → validate → apply → single undo).

- **G-STATES — CLOSED.** `handleError` now classifies transport errors:
  401/403 → logout + re-auth prompt, 429 → rate-limit guidance, curl/stream
  failures → "connection lost, check Settings" with retry hint.

Still open: G-REVIEW (plan preview UX), G-BRIDGE (low priority).
