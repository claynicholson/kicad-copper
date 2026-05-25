"""
Controller — orchestrates: prompt → BackendClient → review → ApplyEngine.

This is the Python counterpart of the orchestration buried in
COPPER_CHAT_PANEL on the C++ side. By moving it here we get:
    - a place to model the state machine cleanly (see ARCHITECTURE.md §5)
    - a single observer interface the panel binds to
    - testability without wxWidgets

Production C++: the chat panel does this in `sendRequest`/`handleSSEEvent`
/`onPlanApproved` — and after M4 the panel calls into a parallel C++
IntegrationController that mirrors this state machine.

State machine:
    IDLE -> SENDING -> STREAMING -> PLAN_PRESENTED -> APPLYING -> APPLIED
            └----- ERROR_STATE (offline / 401 / 5xx / schema / mid-apply) ─┘
    User can cancel at any point in STREAMING/PLAN_PRESENTED → IDLE.
"""

from __future__ import annotations

import enum
from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Sequence

from .apply_engine import ApplyEngine, ApplyError, ApplyResult
from .backend_client import (
    BackendClient,
    BackendError,
    BackendHttpError,
    BackendNetworkError,
    BackendRateLimited,
    BackendSchemaError,
    BackendUnauthorized,
    Done,
    ErrorEvent,
    Event,
    Message,
    PlanEvent,
    Stage,
)
from .schematic_api import SchematicApi
from .validators import ValidatedResponse


class ControllerState(enum.Enum):
    IDLE = "idle"
    SENDING = "sending"
    STREAMING = "streaming"
    PLAN_PRESENTED = "plan_presented"
    APPLYING = "applying"
    APPLIED = "applied"
    ERROR_STATE = "error"


@dataclass
class UiError:
    """A controller-emitted error with enough context for the panel to
    render an actionable UI state. See PROTOCOL.md §HTTP errors."""
    kind: str  # 'offline' | 'unauthorized' | 'rate_limited' | 'http_5xx' | 'schema' | 'apply' | 'cancelled' | 'unknown'
    message: str
    retry_after: Optional[float] = None
    detail: str = ""


class ControllerObserver(ABC):
    """The panel implements this to receive controller events. All methods
    default to no-op so test observers can override only what they care
    about."""

    def on_state(self, state: ControllerState) -> None: ...

    def on_stage(self, stage: Stage) -> None: ...

    def on_message(self, msg: Message) -> None: ...

    def on_plan(self, plan: PlanEvent) -> None: ...

    def on_done(self, response: ValidatedResponse) -> None: ...

    def on_error(self, err: UiError) -> None: ...

    def on_applied(self, result: ApplyResult) -> None: ...


class RecordingObserver(ControllerObserver):
    """Test observer that captures every callback in order. Useful for
    asserting flows in tests."""

    def __init__(self) -> None:
        self.states: List[ControllerState] = []
        self.stages: List[Stage] = []
        self.messages: List[Message] = []
        self.plans: List[PlanEvent] = []
        self.dones: List[ValidatedResponse] = []
        self.errors: List[UiError] = []
        self.applied: List[ApplyResult] = []

    def on_state(self, state): self.states.append(state)
    def on_stage(self, stage): self.stages.append(stage)
    def on_message(self, msg): self.messages.append(msg)
    def on_plan(self, plan): self.plans.append(plan)
    def on_done(self, response): self.dones.append(response)
    def on_error(self, err): self.errors.append(err)
    def on_applied(self, result): self.applied.append(result)


class Controller:
    """Drives the chat flow end-to-end. Stateful (holds the current state +
    pending Done response between PLAN_PRESENTED and APPLY).

    Cross-thread note: the C++ controller bounces all observer calls through
    CallAfter. Tests here are single-threaded; the observer is invoked
    synchronously. The contract is the same."""

    def __init__(
        self,
        backend: BackendClient,
        api: SchematicApi,
        observer: ControllerObserver,
        *,
        apply_engine: Optional[ApplyEngine] = None,
    ) -> None:
        self._backend = backend
        self._api = api
        self._observer = observer
        self._engine = apply_engine or ApplyEngine()

        self._state: ControllerState = ControllerState.IDLE
        self._pending_done: Optional[ValidatedResponse] = None
        self._cancelled = False

    # ── public ──

    @property
    def state(self) -> ControllerState:
        return self._state

    def generate(self, prompt: str, context: Optional[Dict[str, Any]] = None) -> None:
        """Drive a `generate` flow synchronously until done/error/plan.

        For an in-editor C++ panel this would run on a background thread and
        marshal observer calls via CallAfter. Tests call it inline."""

        if self._state not in (ControllerState.IDLE, ControllerState.ERROR_STATE, ControllerState.APPLIED):
            raise RuntimeError(f"cannot generate from state {self._state.name}")

        self._cancelled = False
        self._pending_done = None
        self._set_state(ControllerState.SENDING)

        try:
            events = self._backend.generate(prompt, context or {})
            first_event_seen = False
            for ev in events:
                if self._cancelled:
                    self._emit_error(UiError(kind="cancelled", message="Request cancelled."))
                    return

                if not first_event_seen:
                    self._set_state(ControllerState.STREAMING)
                    first_event_seen = True

                if isinstance(ev, Stage):
                    self._observer.on_stage(ev)
                elif isinstance(ev, Message):
                    self._observer.on_message(ev)
                elif isinstance(ev, PlanEvent):
                    self._observer.on_plan(ev)
                elif isinstance(ev, Done):
                    self._observer.on_done(ev.response)
                    self._pending_done = ev.response
                    # If the backend already delivered a plan event earlier, we
                    # transition to PLAN_PRESENTED. If it didn't but the response
                    # has operations, we still go to PLAN_PRESENTED — the panel
                    # decides whether to auto-apply (it shouldn't, by default).
                    if ev.response.operations:
                        self._set_state(ControllerState.PLAN_PRESENTED)
                    else:
                        # Chat-style response with no ops — final state is APPLIED
                        # (nothing to apply, conversation continues).
                        self._set_state(ControllerState.APPLIED)
                    return
                elif isinstance(ev, ErrorEvent):
                    # Map backend-provided code to a user-state kind.
                    self._emit_error(UiError(
                        kind=ev.code or "unknown",
                        message=ev.message,
                        detail=ev.code,
                    ))
                    return
        except BackendUnauthorized:
            self._emit_error(UiError(
                kind="unauthorized",
                message="Sign in again to continue.",
            ))
            return
        except BackendRateLimited as e:
            self._emit_error(UiError(
                kind="rate_limited",
                message="The backend is rate-limiting requests.",
                retry_after=e.retry_after,
            ))
            return
        except BackendHttpError as e:
            self._emit_error(UiError(
                kind="http_5xx" if e.status >= 500 else "http_4xx",
                message=f"Backend returned HTTP {e.status}.",
                detail=e.body[:500],
            ))
            return
        except BackendSchemaError as e:
            self._emit_error(UiError(
                kind="schema",
                message="The backend sent a response we can't parse.",
                detail=str(e.detail),
            ))
            return
        except BackendNetworkError as e:
            self._emit_error(UiError(
                kind="offline",
                message="Couldn't reach the backend. Check your connection.",
                detail=e.cause,
            ))
            return

        # Stream closed without Done — already converted to ErrorEvent by the
        # client; control should never reach here.
        self._emit_error(UiError(
            kind="offline",
            message="Backend closed the connection unexpectedly.",
            detail="no_done",
        ))

    def chat(self, prompt: str, context: Optional[Dict[str, Any]] = None) -> None:
        """One-shot chat/recommend/plan path — no plan card, just an AI message."""
        if self._state not in (ControllerState.IDLE, ControllerState.ERROR_STATE, ControllerState.APPLIED):
            raise RuntimeError(f"cannot chat from state {self._state.name}")
        self._cancelled = False
        self._set_state(ControllerState.SENDING)
        try:
            resp = self._backend.chat(prompt, context or {})
        except BackendUnauthorized:
            self._emit_error(UiError(kind="unauthorized", message="Sign in again."))
            return
        except BackendNetworkError as e:
            self._emit_error(UiError(kind="offline", message="Couldn't reach backend.", detail=e.cause))
            return
        except BackendSchemaError as e:
            self._emit_error(UiError(kind="schema", message="Bad response.", detail=str(e.detail)))
            return
        except BackendHttpError as e:
            self._emit_error(UiError(kind="http_5xx" if e.status >= 500 else "http_4xx",
                                     message=f"HTTP {e.status}", detail=e.body[:500]))
            return
        self._observer.on_done(resp)
        self._set_state(ControllerState.APPLIED)

    def approve_plan(self) -> None:
        """User clicked Apply on the plan card. Run the apply engine."""
        if self._state != ControllerState.PLAN_PRESENTED:
            raise RuntimeError(f"cannot approve from state {self._state.name}")
        if self._pending_done is None:
            raise RuntimeError("no pending Done response to apply")

        self._set_state(ControllerState.APPLYING)
        try:
            result = self._engine.apply_validated(self._api, self._pending_done)
        except ApplyError as e:
            self._emit_error(UiError(
                kind="apply",
                message=f"Couldn't apply the plan: {e}",
                detail=e.code,
            ))
            return

        self._observer.on_applied(result)
        self._pending_done = None
        self._set_state(ControllerState.APPLIED)

    def cancel_plan(self) -> None:
        if self._state != ControllerState.PLAN_PRESENTED:
            return  # idempotent — no-op when not waiting
        self._pending_done = None
        self._set_state(ControllerState.IDLE)

    def cancel(self) -> None:
        """User clicked Cancel during streaming. Best-effort — the actual
        cancellation pierces the Transport in production via the cancel flag."""
        self._cancelled = True

    def undo_last_apply(self) -> Optional[str]:
        if self._state != ControllerState.APPLIED:
            return None
        cid = self._api.undo()
        if cid is not None:
            self._set_state(ControllerState.IDLE)
        return cid

    # ── internals ──

    def _set_state(self, s: ControllerState) -> None:
        self._state = s
        self._observer.on_state(s)

    def _emit_error(self, err: UiError) -> None:
        self._set_state(ControllerState.ERROR_STATE)
        self._observer.on_error(err)
