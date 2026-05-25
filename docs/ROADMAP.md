# ROADMAP — deferred items (out-of-scope per BUILD_PROMPT §2)

Tracked here so the seams aren't lost. Nothing here is required for the §10
Definition of Done. Add anything dropped from scope during M1–M6 here.

## R1 — Generation logic in the plugin
**Defer indefinitely.** All IR creation / part choice / placement / routing
/ ERC lives in the `copper` backend. The plugin should never grow this.

## R2 — PCB layout / routing
Out of scope — pcbnew is a separate editor; chat will eventually drive PCB
too, but that's a copy of this stack pointed at pcbnew. Track for the next
major release.

## R3 — Account management / billing UI
Lives in `copper-platform`. The plugin only renders a "Sign in" button.

## R4 — Drawn-wire aesthetics
The backend emits labels-first plans by design (per the engine's design
philosophy). Once it emits explicit wire routes the apply layer already
supports them (`ADD_WIRE`). Stretch goal post-M6.

## R5 — Inline plan editing
The `onPlanEdited` button in `COPPER_PLAN_CARD` is a UX stub. Edit-then-apply
is a larger UX story (re-prompt with mutations, structured diff against the
plan). Defer.

## R6 — Multi-sheet apply
Today every op targets `m_frame->Schematic().CurrentSheet()`. Multi-sheet
support requires the apply-plan to specify a sheet path per op + a sheet-
creation op. Backend doesn't emit this yet. Defer.

## R7 — "Explain this schematic" / "Verify" follow-ups
Endpoints would be `/api/v1/explain` and `/api/v1/verify`. Transport is
trivially the existing `BackendClient`; UI is a one-button chip on the plan
card or hint chip. Defer to post-M6.

## R8 — Drawn-wire route aesthetics (router-quality)
If the backend ever emits routed wires (vs. labels-first), a follow-up pass
in the apply layer could clean up overshoot / 45° corners. Defer.

## R9 — Telemetry / opt-in usage stats
Out of scope for the plugin (privacy boundary). Backend may collect what it
serves.

## R10 — In-editor diff view
A side-by-side "before / after this plan" panel showing the visual delta.
Would use the SVG preview from the backend's response. Defer.

## R11 — Native HTTP/2 + connection reuse for SSE
The current libcurl wrapper is HTTP/1.1 only. Backend supports both. Not a
correctness issue, just latency. Defer.

## R12 — Schematic export → backend for follow-ups
For "explain", the backend may want the current schematic shape, not just
the lightweight context. Defer; design a new endpoint when needed.

## R13 — Conventional gestures on plan cards
Right-click op → "Pin to library", "Replace this part", etc. UX-heavy; defer.
