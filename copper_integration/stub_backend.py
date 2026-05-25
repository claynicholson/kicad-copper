"""
StubBackend — an in-memory Transport that replays canned fixtures.

Used by the harness. The same JSON shapes the real Copper backend serves;
the C++ COPPER::CLIENT cannot distinguish StubBackend from the real one
because both speak PROTOCOL.md.

Features:
    - load_fixture(path) parses SSE-or-JSON fixture files from fixtures/.
    - Per-call error injection (http_status, network_error_on_call, etc).
    - Per-call assertion hooks (asserts the C++ client sends the right body).
    - Records every request for later inspection.
"""

from __future__ import annotations

import io
import json
import os
import pathlib
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, Iterable, Iterator, List, Optional, Tuple

from .backend_client import (
    BackendError,
    BackendNetworkError,
    Transport,
)


FIXTURES_DIR = pathlib.Path(__file__).resolve().parent.parent / "fixtures"


@dataclass
class RecordedRequest:
    url: str
    body: Dict[str, Any]
    headers: Dict[str, str]
    timeout: float


@dataclass
class _Programmed:
    """One programmed response. The StubBackend FIFO-consumes these."""
    kind: str  # 'json' | 'sse' | 'network_error' | 'http_status'
    status: int = 200
    json_body: Optional[Dict[str, Any]] = None
    sse_bytes: Optional[bytes] = None
    err_msg: str = ""
    extra_headers: Dict[str, str] = field(default_factory=dict)


class StubBackend(Transport):
    """In-memory Transport for tests.

    Usage:
        stub = StubBackend()
        stub.program_response({"protocol_version": 1, "success": True, ...})
        client = BackendClient(stub)
        client.chat("hi")

    Or for streaming:
        stub.program_sse_file("generate_happy.sse")
        events = list(client.generate("..."))
    """

    def __init__(self) -> None:
        self._queue: List[_Programmed] = []
        self.recorded: List[RecordedRequest] = []

    # ── programming ──

    def program_response(self, body: Dict[str, Any], status: int = 200) -> None:
        self._queue.append(_Programmed(kind="json", json_body=body, status=status))

    def program_sse(self, raw: bytes, status: int = 200) -> None:
        self._queue.append(_Programmed(kind="sse", sse_bytes=raw, status=status))

    def program_sse_text(self, text: str, status: int = 200) -> None:
        self.program_sse(text.encode("utf-8"), status=status)

    def program_sse_file(self, name: str) -> None:
        path = FIXTURES_DIR / name
        if not path.exists():
            raise FileNotFoundError(f"fixture not found: {path}")
        self.program_sse(path.read_bytes())

    def program_json_file(self, name: str, status: int = 200) -> None:
        path = FIXTURES_DIR / name
        if not path.exists():
            raise FileNotFoundError(f"fixture not found: {path}")
        payload = json.loads(path.read_text(encoding="utf-8"))
        self.program_response(payload, status=status)

    def program_http_error(self, status: int, body: Dict[str, Any] = None) -> None:
        self._queue.append(_Programmed(
            kind="http_status", status=status,
            json_body=body or {"error": f"HTTP {status}"}
        ))

    def program_network_error(self, message: str = "connection refused") -> None:
        self._queue.append(_Programmed(kind="network_error", err_msg=message))

    # Convenience for malformed
    def program_malformed_json(self) -> None:
        self._queue.append(_Programmed(
            kind="json", status=200,
            json_body=None,  # poisoned; see _next() for handling
            err_msg="__MALFORMED__",
        ))

    # ── Transport protocol ──

    def post_json(
        self,
        url: str,
        body: Dict[str, Any],
        headers: Dict[str, str],
        timeout_seconds: float,
    ) -> Tuple[int, Dict[str, str], bytes]:
        self.recorded.append(RecordedRequest(url, body, dict(headers), timeout_seconds))
        prog = self._next()

        if prog.kind == "network_error":
            raise BackendNetworkError(prog.err_msg or "stub network error")
        if prog.kind == "http_status":
            return prog.status, prog.extra_headers, json.dumps(prog.json_body).encode("utf-8")
        if prog.kind == "json":
            if prog.err_msg == "__MALFORMED__":
                return prog.status, prog.extra_headers, b"{not json"
            return prog.status, prog.extra_headers, json.dumps(prog.json_body).encode("utf-8")
        raise RuntimeError(f"stub programmed for {prog.kind!r} but post_json was called")

    def post_sse(
        self,
        url: str,
        body: Dict[str, Any],
        headers: Dict[str, str],
        timeout_seconds: float,
    ) -> Iterable[bytes]:
        # IMPORTANT: errors must be raised *before* the function returns the
        # iterable, so callers can catch them around the post_sse() call —
        # mirroring real libcurl, where the HTTP status arrives before any
        # body. If this method were a generator (had a `yield` directly), the
        # error would only surface during iteration. Hence the helper.
        self.recorded.append(RecordedRequest(url, body, dict(headers), timeout_seconds))
        prog = self._next()

        if prog.kind == "network_error":
            raise BackendNetworkError(prog.err_msg or "stub network error")
        if prog.kind == "http_status":
            from .backend_client import BackendHttpError, BackendUnauthorized, BackendRateLimited
            body_s = json.dumps(prog.json_body)
            if prog.status in (401, 403):
                raise BackendUnauthorized(prog.status, body_s)
            if prog.status == 429:
                raise BackendRateLimited(body_s)
            raise BackendHttpError(prog.status, body_s)
        if prog.kind != "sse":
            raise RuntimeError(f"stub programmed for {prog.kind!r} but post_sse was called")

        data = prog.sse_bytes or b""

        def _iter():
            chunk_size = max(1, len(data) // 4)
            for i in range(0, len(data), chunk_size):
                yield data[i : i + chunk_size]

        return _iter()

    # ── internals ──

    def _next(self) -> _Programmed:
        if not self._queue:
            raise RuntimeError(
                "StubBackend has no programmed responses left. "
                "Call program_response / program_sse / program_http_error / "
                "program_network_error before the call."
            )
        return self._queue.pop(0)

    # ── helpers ──

    def last_request(self) -> RecordedRequest:
        if not self.recorded:
            raise RuntimeError("no requests recorded yet")
        return self.recorded[-1]

    def reset(self) -> None:
        self._queue.clear()
        self.recorded.clear()
