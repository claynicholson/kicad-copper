"""
panel_glue — documented contract describing how the C++ chat panel binds to
the controller. There is no runtime code here on the Python side (the panel
is C++); this module exists so the harness can:

    1. Verify the symbols the C++ side relies on are documented + named
       consistently with COPPER_CHAT_PANEL's hooks (FORK_SURFACE.md §1).
    2. Provide a Python ControllerObserver impl that mimics the C++ panel
       for end-to-end tests (`FakeChatPanel`).

The C++ binding contract (after M4):

    SCH_EDIT_FRAME::initCopperPanel():
        1. Create COPPER_CHAT_PANEL(this, this).
        2. Register it with the AUI manager as a right-dock pane.
        3. Wire View menu toggle.

    COPPER_CHAT_PANEL::sendRequest(prompt) is invoked on the wx main thread.
    It calls m_client->Generate(prompt, ctx, onEvent, onError); the onEvent
    lambda calls CallAfter(handleSSEEvent). handleSSEEvent routes to:
        - showStages / updateStage for 'stage'
        - addAIMessage for 'message'
        - addPlanCard for 'plan'
        - handleResponse for 'done'
        - handleError for 'error'

    A plan card with an Approve button → onPlanApproved → ExecuteOperations.

    State transitions match controller.py ControllerState. The C++ side does
    not need to share the enum literal — it uses booleans + UI widget state
    — but the *semantics* must match for the harness checks to be predictive.
"""

from __future__ import annotations

from typing import List

from .controller import (
    Controller,
    ControllerObserver,
    ControllerState,
    UiError,
)
from .apply_engine import ApplyResult
from .backend_client import Message, PlanEvent, Stage
from .validators import ValidatedResponse


class FakeChatPanel(ControllerObserver):
    """Lightweight panel mock for end-to-end harness tests.

    Mirrors what the C++ COPPER_CHAT_PANEL does in response to controller
    events: appends bubbles, updates stage indicators, holds pending ops
    awaiting Approve.
    """

    def __init__(self) -> None:
        self.bubbles: List[str] = []
        self.ai_bubbles: List[str] = []
        self.stages: dict[str, str] = {}  # name -> status
        self.last_plan: PlanEvent | None = None
        self.last_response: ValidatedResponse | None = None
        self.last_error: UiError | None = None
        self.last_apply: ApplyResult | None = None
        self.states: List[ControllerState] = []

    # ── ControllerObserver impl ──

    def on_state(self, state: ControllerState) -> None:
        self.states.append(state)

    def on_stage(self, stage: Stage) -> None:
        self.stages[stage.name] = stage.status

    def on_message(self, msg: Message) -> None:
        self.ai_bubbles.append(msg.text)

    def on_plan(self, plan: PlanEvent) -> None:
        self.last_plan = plan

    def on_done(self, response: ValidatedResponse) -> None:
        self.last_response = response
        if response.message:
            self.ai_bubbles.append(response.message)

    def on_error(self, err: UiError) -> None:
        self.last_error = err

    def on_applied(self, result: ApplyResult) -> None:
        self.last_apply = result

    # ── helper for tests ──

    def type_prompt(self, controller: Controller, text: str) -> None:
        """Mimic user pressing Send in the input bar."""
        self.bubbles.append(f"user: {text}")
        controller.generate(text)
