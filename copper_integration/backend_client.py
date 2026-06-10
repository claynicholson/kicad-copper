"""
BackendClient — typed transport over the PROTOCOL.md contract.

This is the Python counterpart of COPPER::CLIENT (eeschema/copper/copper_client.cpp).
It accepts a pluggable Transport so production code can wire libcurl-equivalent
behavior (urllib + a thread) while tests inject StubBackend directly.

The C++ side does the same thing structurally — same JSON shapes, same auth
header, same SSE parsing — and stays in sync via docs/PROTOCOL.md.

The streaming interface yields typed Event values (Stage, Message, PlanEvent,
Done, ErrorEvent). Validation of the `done` payload happens via
copper_integration.validators.validate_response. Mid-stream Stage/Message
events are NOT validated structurally on purpose — see PROTOCOL.md §Stream
lifecycle, "partial stages are not validated".
"""

from __future__ import annotations

import json
from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from typing import (
    Any,
    Callable,
    Dict,
    Iterable,
    Iterator,
    List,
    Optional,
    Tuple,
    Union,
)

from .settings import Settings, resolve_api_url, resolve_api_token
from .validators import ValidationError, validate_response, ValidatedResponse


# ── error hierarchy ────────────────────────────────────────────────────────


class BackendError(Exception):
    """Base for any BackendClient failure."""


class BackendNetworkError(BackendError):
    """Connection failed (DNS, TCP, TLS, dropped mid-stream)."""

    def __init__(self, cause: str):
        super().__init__(f"network error: {cause}")
        self.cause = cause


class BackendHttpError(BackendError):
    """Non-200 HTTP status. 401/403/429 get their own subclasses below."""

    def __init__(self, status: int, body: str = ""):
        super().__init__(f"HTTP {status}")
        self.status = status
        self.body = body


class BackendUnauthorized(BackendHttpError):
    """401 or 403 — caller should prompt re-auth."""


class BackendRateLimited(BackendHttpError):
    """429 with optional retry-after seconds. If retry_after isn't passed
    explicitly, the body is sniffed for a `retry_after` field — matches the
    one-shot client path so callers don't need to disambiguate."""

    def __init__(self, body: str = "", retry_after: Optional[float] = None):
        super().__init__(429, body)
        if retry_after is None and body:
            try:
                obj = json.loads(body)
                ra = obj.get("retry_after")
                if isinstance(ra, (int, float)):
                    retry_after = float(ra)
            except (json.JSONDecodeError, AttributeError):
                pass
        self.retry_after = retry_after


class BackendSchemaError(BackendError):
    """Response was syntactically valid JSON but failed PROTOCOL.md checks."""

    def __init__(self, detail: ValidationError):
        super().__init__(f"schema: {detail}")
        self.detail = detail


# ── event types (stream output) ─────────────────────────────────────────────


@dataclass
class Stage:
    name: str
    status: str  # 'pending' | 'active' | 'complete' | 'error'


@dataclass
class Message:
    text: str


@dataclass
class PlanEvent:
    steps: List[Dict[str, Any]]
    placement_info: str


@dataclass
class Done:
    response: ValidatedResponse


@dataclass
class ErrorEvent:
    message: str
    code: str = ""


Event = Union[Stage, Message, PlanEvent, Done, ErrorEvent]


# ── transport seam (the boundary between BackendClient and the wire) ──────


class Transport(ABC):
    """Abstraction over the network call. Production wires this to urllib (or
    libcurl-equivalent); tests inject StubBackend directly."""

    @abstractmethod
    def post_json(
        self,
        url: str,
        body: Dict[str, Any],
        headers: Dict[str, str],
        timeout_seconds: float,
    ) -> Tuple[int, Dict[str, str], bytes]:
        """Return (status, headers, body_bytes). Raises BackendNetworkError on
        connection failure. Does NOT parse JSON or interpret status."""

    @abstractmethod
    def post_sse(
        self,
        url: str,
        body: Dict[str, Any],
        headers: Dict[str, str],
        timeout_seconds: float,
    ) -> Iterable[bytes]:
        """Iterator of raw byte chunks forming the SSE stream. The
        BackendClient runs the SSE parser on top. Raises BackendNetworkError
        on connection failure or BackendHttpError on non-200. Caller may
        stop iteration to cancel."""


# ── SSE line parser (matches COPPER::CLIENT::sseWriteCallback) ────────────


def parse_sse_stream(chunks: Iterable[bytes]) -> Iterator[Tuple[str, str]]:
    """Yield (event_type, data_str) tuples. Mirrors the C++ parser:
    buffer partial lines, split on \\n, strip trailing \\r, accumulate
    `data:` lines, dispatch on empty line.

    On a stream that ends without a trailing blank line, the final partial
    event (if any) is yielded — mirroring the C++ behavior of dispatching
    whatever data is in the buffer at EOF."""

    buf = b""
    current_event = ""
    current_data: List[str] = []

    def emit() -> Optional[Tuple[str, str]]:
        nonlocal current_event, current_data
        if not current_data:
            current_event = ""
            return None
        ev = current_event or "message"
        data = "\n".join(current_data)
        current_event = ""
        current_data = []
        return (ev, data)

    for chunk in chunks:
        if not chunk:
            continue
        buf += chunk
        while b"\n" in buf:
            line_b, buf = buf.split(b"\n", 1)
            if line_b.endswith(b"\r"):
                line_b = line_b[:-1]
            line = line_b.decode("utf-8", errors="replace")

            if line == "":
                ev = emit()
                if ev is not None:
                    yield ev
                continue
            if line.startswith(":"):
                continue  # SSE comment
            if line.startswith("event:"):
                current_event = line[6:].lstrip()
                continue
            if line.startswith("data:"):
                d = line[5:]
                if d.startswith(" "):
                    d = d[1:]
                current_data.append(d)
                continue
            # Unknown field per SSE — silently ignore (matches C++).

    # End of stream — flush any half-built event.
    ev = emit()
    if ev is not None:
        yield ev


# ── BackendClient ──────────────────────────────────────────────────────────


@dataclass
class _RequestCtx:
    url: str
    body: Dict[str, Any]
    headers: Dict[str, str]
    timeout: float


class BackendClient:
    """Stateless wrapper over Transport. Reads Settings to build URLs/headers.

    Methods raise BackendError subclasses on failure. Callers in the C++
    plugin map those onto user-visible states (see PROTOCOL.md §HTTP errors)."""

    USER_AGENT = "KiCad-Copper/0.1"

    def __init__(self, transport: Transport, settings: Optional[Settings] = None):
        self._transport = transport
        self._settings = settings or Settings()

    # ── non-streaming endpoints ──

    def chat(self, prompt: str, context: Optional[Dict[str, Any]] = None) -> ValidatedResponse:
        return self._post_one_shot("chat", prompt, context)

    def recommend(self, prompt: str, context: Optional[Dict[str, Any]] = None) -> ValidatedResponse:
        return self._post_one_shot("recommend", prompt, context)

    def plan(self, prompt: str, context: Optional[Dict[str, Any]] = None) -> ValidatedResponse:
        return self._post_one_shot("plan", prompt, context)

    # ── streaming ──

    def generate(
        self, prompt: str, context: Optional[Dict[str, Any]] = None
    ) -> Iterator[Event]:
        """Yield typed Events. The final event is Done or ErrorEvent. May
        raise BackendError subclasses before the first event if the connect
        or HTTP status fails. Mid-stream failures yield an ErrorEvent and
        terminate the iterator (no further yields)."""

        if not prompt or not prompt.strip():
            raise ValueError("prompt must be non-empty")

        ctx = self._build_request("generate", prompt, context)
        ctx.headers["Accept"] = "text/event-stream"

        try:
            chunks = self._transport.post_sse(
                ctx.url, ctx.body, ctx.headers, ctx.timeout,
            )
        except BackendError:
            raise
        except Exception as e:
            raise BackendNetworkError(str(e)) from e

        terminated = False
        for ev_type, data_str in parse_sse_stream(chunks):
            try:
                data = json.loads(data_str) if data_str else {}
            except json.JSONDecodeError:
                yield ErrorEvent(
                    message=f"bad JSON in '{ev_type}' event",
                    code="bad_event_json",
                )
                terminated = True
                break

            if ev_type == "stage":
                name = data.get("name", "")
                status = data.get("status", "pending")
                yield Stage(name=name, status=status)
            elif ev_type == "message":
                yield Message(text=data.get("text", ""))
            elif ev_type == "plan":
                yield PlanEvent(
                    steps=data.get("steps", []),
                    placement_info=data.get("placement_info", ""),
                )
            elif ev_type == "done":
                # Defensive unwrap (mirrors copper_chat_panel.cpp): PROTOCOL.md
                # says done data is a flattened CopperResponse, but backend
                # <= v0.1.0 nested it under a "plan" key.
                if (
                    "operations" not in data
                    and isinstance(data.get("plan"), dict)
                    and "operations" in data["plan"]
                ):
                    data = data["plan"]
                try:
                    validated = validate_response(data)
                except ValidationError as ve:
                    raise BackendSchemaError(ve) from ve
                yield Done(response=validated)
                terminated = True
                break
            elif ev_type == "error":
                yield ErrorEvent(
                    message=data.get("message", "unknown error"),
                    code=data.get("code", ""),
                )
                terminated = True
                break
            # else: unknown event, silently drop (PROTOCOL.md compat clause)

        if not terminated:
            # Stream closed without a `done` or `error`. Surface as a clean
            # network-level signal so the controller can show a "Backend
            # connection lost" state.
            yield ErrorEvent(message="stream ended without done", code="no_done")

    # ── internals ──

    def _post_one_shot(
        self,
        intent: str,
        prompt: str,
        context: Optional[Dict[str, Any]],
    ) -> ValidatedResponse:
        if not prompt or not prompt.strip():
            raise ValueError("prompt must be non-empty")
        ctx = self._build_request(intent, prompt, context)

        try:
            status, _hdrs, body = self._transport.post_json(
                ctx.url, ctx.body, ctx.headers, ctx.timeout,
            )
        except BackendError:
            raise
        except Exception as e:
            raise BackendNetworkError(str(e)) from e

        self._check_status(status, body)

        try:
            payload = json.loads(body.decode("utf-8", errors="replace"))
        except json.JSONDecodeError as e:
            raise BackendSchemaError(
                ValidationError(f"response body is not valid JSON: {e}", code="bad_json")
            ) from e

        try:
            return validate_response(payload)
        except ValidationError as ve:
            raise BackendSchemaError(ve) from ve

    def _build_request(
        self,
        intent: str,
        prompt: str,
        context: Optional[Dict[str, Any]],
    ) -> _RequestCtx:
        base = resolve_api_url(self._settings).rstrip("/")
        path = {
            "chat": "/api/v1/chat",
            "recommend": "/api/v1/recommend",
            "plan": "/api/v1/plan",
            "generate": "/api/v1/generate",
        }[intent]
        url = base + path

        token = resolve_api_token(self._settings)
        headers: Dict[str, str] = {
            "Content-Type": "application/json",
            "User-Agent": self.USER_AGENT,
        }
        if token:
            headers["Authorization"] = f"Bearer {token}"

        body = {
            "prompt": prompt,
            "intent": intent,
            "context": context or {},
        }

        timeout = (
            self._settings.stream_idle_timeout_seconds
            if intent == "generate"
            else self._settings.timeout_seconds
        )
        return _RequestCtx(url=url, body=body, headers=headers, timeout=timeout)

    @staticmethod
    def _check_status(status: int, body: bytes) -> None:
        if status == 200:
            return
        body_s = body.decode("utf-8", errors="replace") if body else ""
        if status == 401 or status == 403:
            raise BackendUnauthorized(status, body_s)
        if status == 429:
            # retry-after is best-effort — look in body
            retry_after = None
            try:
                obj = json.loads(body_s) if body_s else {}
                ra = obj.get("retry_after")
                if isinstance(ra, (int, float)):
                    retry_after = float(ra)
            except json.JSONDecodeError:
                pass
            raise BackendRateLimited(body_s, retry_after=retry_after)
        raise BackendHttpError(status, body_s)
