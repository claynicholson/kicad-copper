# PROGRESS — milestone status

Live status board.

| ID  | Milestone                                | Status     | Closes gaps                            |
|-----|------------------------------------------|------------|----------------------------------------|
| M0  | Audit fork + scaffold                    | done       | G-HARNESS scaffolding, G-PROTO docs    |
| M1  | SchematicApi seam + FakeSchematicApi     | done       | G-SEAM                                 |
| M2  | BackendClient + StubBackend + PROTOCOL   | done       | G-STUB, G-PROTO                        |
| M3  | ApplyEngine (atomic, validated)          | done       | G-VALIDATE, G-ATOMIC, G-QUALITY, G-APPROVE (Python side) |
| M4  | Controller + panel glue + CMake wiring   | done       | G-CMAKE, G-FRAME, G-SETTINGS, G-REVIEW, G-STAGES, G-APPROVE (C++ side), G-BRIDGE¹ |
| M5  | State + UX hardening                     | done       | G-STATES                               |
| M6  | scripts/assess.py + polish loop          | done       | G-ASSESS                               |

¹ G-BRIDGE: the `HTTP_BRIDGE` class is in CMake now but is not auto-started
  by `SCH_EDIT_FRAME`. The bridge is a side surface for external tools, not
  on the chat flow's critical path (see [FORK_SURFACE.md](FORK_SURFACE.md) §5b
  and [ROADMAP.md](ROADMAP.md)). Starting it is a one-line addition once
  there's UX for the on/off toggle.

## End state

- §8 self-assessment: **100.0 / 100**, all **7 hard gates green**, polished.
  See [SELF_ASSESSMENT.md](SELF_ASSESSMENT.md) + [LOOP_LOG.md](LOOP_LOG.md).
- 126 unit + integration tests passing in `tests/`.
- C++ plugin: CMake wired (5 new sources), `EESCHEMA_SETTINGS::m_Copper`
  added, `COPPER_CHAT_PANEL` instantiated in `SCH_EDIT_FRAME` as a right-dock
  AUI pane, `onPlanApproved` now actually applies the pending plan,
  `ExecuteOperations` validates fail-closed (ADR-004) before constructing
  any items.
- Python harness: `copper_integration/` exports `SchematicApi`,
  `FakeSchematicApi`, `BackendClient`, `StubBackend`, `ApplyEngine`,
  `Controller`, `Settings`, `panel_glue.FakeChatPanel`.
- Protocol contract documented in [PROTOCOL.md](PROTOCOL.md) — the shared
  source of truth with `copper` / `copper-platform`.

## What was fixed vs the v1 audit

From [INTEGRATION_V1_AUDIT.md](INTEGRATION_V1_AUDIT.md):

| Gap         | Status                          |
|-------------|---------------------------------|
| G-APPROVE   | fixed (M4)                      |
| G-VALIDATE  | fixed (M3 Python + M4 C++)      |
| G-REVIEW    | fixed (M4)                      |
| G-SETTINGS  | fixed (M4)                      |
| G-STATES    | fixed (M5)                      |
| G-ATOMIC    | fixed (M3 + ADR-004 in C++)     |
| G-STAGES    | fixed (M4)                      |
| G-QUALITY   | fixed (M3)                      |
| G-CMAKE     | fixed (M4)                      |
| G-FRAME     | fixed (M4)                      |
| G-BRIDGE    | partially fixed (CMake; start-up TODO) |
| G-HARNESS   | fixed (M1–M6)                   |
| G-SEAM      | fixed (M1)                      |
| G-PROTO     | fixed (M2)                      |
| G-STUB      | fixed (M2)                      |
| G-ASSESS    | fixed (M6)                      |

## What's stubbed / deferred

- The C++ build was not run in this session (a full KiCad compile is hours
  of cold build on Windows). The C++ side passes the harness's static
  consistency checks (CMake wiring, settings field, frame instantiation,
  op-type coverage). A manual smoke test in a real KiCad build is
  recommended before shipping — see [TESTING.md](TESTING.md) §"Manual
  smoke test" and [DEMO.md](DEMO.md).

## Loop log

See [LOOP_LOG.md](LOOP_LOG.md). The polish loop in M6 converged on the
first iteration because M1–M5 already closed every gap by construction.
Each milestone added the verification harness alongside the implementation,
so the work was effectively tested-as-built. The §8 closed-loop was the
final acceptance gate, not the bug-finding engine.

This is acceptable per BUILD_PROMPT §8 ("if polished: break -> DONE"). If
the harness ever regresses on a future change, the loop will iterate then;
the log will grow.
