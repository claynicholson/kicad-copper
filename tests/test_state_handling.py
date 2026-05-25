"""Tests for check 8: state handling. Every PROTOCOL.md error case must
render an actionable UI state and apply nothing.

Covers:
    - network errors (curl != 0 / connection refused)
    - 401 / 403 unauthorized
    - 429 rate limited (with retry_after)
    - 5xx server errors
    - 4xx generic
    - schema mismatch (future protocol_version, bad json, missing fields)
    - partial stream (no done)
    - mid-stream ErrorEvent
    - mid-apply failure (rollback)
    - cancel during plan_presented (no apply)
"""

from __future__ import annotations

import unittest

from copper_integration.backend_client import BackendClient
from copper_integration.controller import (
    Controller,
    ControllerState,
    UiError,
)
from copper_integration.panel_glue import FakeChatPanel
from copper_integration.schematic_api import FakeSchematicApi
from copper_integration.settings import Settings
from copper_integration.stub_backend import StubBackend


def _wire():
    stub = StubBackend()
    client = BackendClient(stub, Settings(api_url="https://x.y", saved_token="t"))
    api = FakeSchematicApi()
    panel = FakeChatPanel()
    ctrl = Controller(client, api, panel)
    return stub, client, api, panel, ctrl


def _assert_clean_failure(api, panel, ctrl, *, kind: str, before: bytes,
                          retry_after=None):
    assert ctrl.state == ControllerState.ERROR_STATE, ctrl.state
    assert panel.last_error is not None
    assert panel.last_error.kind == kind, panel.last_error.kind
    if retry_after is not None:
        assert panel.last_error.retry_after == retry_after
    assert api.serialize() == before  # nothing applied
    assert panel.last_apply is None  # no apply observer fired


class NetworkErrorTest(unittest.TestCase):
    def test_dns_failure(self):
        stub, _, api, panel, ctrl = _wire()
        stub.program_network_error("DNS lookup failed")
        before = api.serialize()
        panel.type_prompt(ctrl, "x")
        _assert_clean_failure(api, panel, ctrl, kind="offline", before=before)

    def test_connection_refused(self):
        stub, _, api, panel, ctrl = _wire()
        stub.program_network_error("connection refused")
        before = api.serialize()
        panel.type_prompt(ctrl, "x")
        _assert_clean_failure(api, panel, ctrl, kind="offline", before=before)


class HttpStatusTest(unittest.TestCase):
    def test_401(self):
        stub, _, api, panel, ctrl = _wire()
        stub.program_http_error(401, {"error": "expired"})
        before = api.serialize()
        panel.type_prompt(ctrl, "x")
        _assert_clean_failure(api, panel, ctrl, kind="unauthorized", before=before)

    def test_403(self):
        stub, _, api, panel, ctrl = _wire()
        stub.program_http_error(403, {"error": "no access"})
        before = api.serialize()
        panel.type_prompt(ctrl, "x")
        _assert_clean_failure(api, panel, ctrl, kind="unauthorized", before=before)

    def test_429_with_retry_after(self):
        stub, _, api, panel, ctrl = _wire()
        stub.program_http_error(429, {"error": "slow", "retry_after": 10})
        before = api.serialize()
        panel.type_prompt(ctrl, "x")
        _assert_clean_failure(api, panel, ctrl, kind="rate_limited",
                              before=before, retry_after=10.0)

    def test_500(self):
        stub, _, api, panel, ctrl = _wire()
        stub.program_http_error(500, {"error": "boom"})
        before = api.serialize()
        panel.type_prompt(ctrl, "x")
        _assert_clean_failure(api, panel, ctrl, kind="http_5xx", before=before)

    def test_404(self):
        stub, _, api, panel, ctrl = _wire()
        stub.program_http_error(404, {"error": "not found"})
        before = api.serialize()
        panel.type_prompt(ctrl, "x")
        _assert_clean_failure(api, panel, ctrl, kind="http_4xx", before=before)


class SchemaMismatchTest(unittest.TestCase):
    def test_malformed_done_payload(self):
        stub, _, api, panel, ctrl = _wire()
        stub.program_sse_file("generate_malformed_done.sse")
        before = api.serialize()
        panel.type_prompt(ctrl, "x")
        # malformed `done` (empty lib_id) → BackendSchemaError → schema state
        _assert_clean_failure(api, panel, ctrl, kind="schema", before=before)

    def test_future_protocol_version_streaming(self):
        stub, _, api, panel, ctrl = _wire()
        # protocol_version 99 in done
        bad = (
            b"event: done\n"
            b'data: {"protocol_version":99,"success":true,"intent":"generate"}\n\n'
        )
        stub.program_sse(bad)
        before = api.serialize()
        panel.type_prompt(ctrl, "x")
        _assert_clean_failure(api, panel, ctrl, kind="schema", before=before)


class PartialStreamTest(unittest.TestCase):
    def test_no_done_event(self):
        stub, _, api, panel, ctrl = _wire()
        stub.program_sse_file("generate_partial_no_done.sse")
        before = api.serialize()
        panel.type_prompt(ctrl, "x")
        # Synthesized ErrorEvent with code "no_done" -> panel.last_error.kind
        self.assertEqual(ctrl.state, ControllerState.ERROR_STATE)
        self.assertEqual(panel.last_error.kind, "no_done")
        self.assertEqual(api.serialize(), before)

    def test_mid_stream_error_event(self):
        stub, _, api, panel, ctrl = _wire()
        stub.program_sse_file("generate_error.sse")
        before = api.serialize()
        panel.type_prompt(ctrl, "x")
        self.assertEqual(ctrl.state, ControllerState.ERROR_STATE)
        self.assertEqual(panel.last_error.kind, "part_not_found")
        self.assertEqual(api.serialize(), before)


class CancelDuringPlanTest(unittest.TestCase):
    def test_cancel_after_plan_does_not_apply(self):
        stub, _, api, panel, ctrl = _wire()
        stub.program_sse_file("generate_happy.sse")
        before = api.serialize()
        panel.type_prompt(ctrl, "x")
        self.assertEqual(ctrl.state, ControllerState.PLAN_PRESENTED)
        ctrl.cancel_plan()
        self.assertEqual(ctrl.state, ControllerState.IDLE)
        self.assertEqual(api.serialize(), before)


class MidApplyFailureTest(unittest.TestCase):
    """Even if the plan validates, an injected failure during apply must
    leave the schematic byte-equal and surface an actionable state."""

    def test_injected_failure_during_apply(self):
        stub, _, api, panel, ctrl = _wire()
        stub.program_sse_file("generate_happy.sse")
        api.inject_failure(after_n_in_commit=3)

        before = api.serialize()
        panel.type_prompt(ctrl, "x")
        self.assertEqual(ctrl.state, ControllerState.PLAN_PRESENTED)

        ctrl.approve_plan()
        self.assertEqual(ctrl.state, ControllerState.ERROR_STATE)
        self.assertEqual(panel.last_error.kind, "apply")
        self.assertEqual(api.serialize(), before)
        self.assertEqual(panel.last_apply, None)


class IdempotentRetryTest(unittest.TestCase):
    """User retries after a clean failure. State machine accepts the second
    attempt."""

    def test_retry_after_offline_succeeds(self):
        stub, _, api, panel, ctrl = _wire()
        stub.program_network_error("first try")
        before = api.serialize()
        panel.type_prompt(ctrl, "x")
        self.assertEqual(ctrl.state, ControllerState.ERROR_STATE)
        self.assertEqual(api.serialize(), before)

        stub.program_sse_file("generate_happy.sse")
        panel.type_prompt(ctrl, "x")
        self.assertEqual(ctrl.state, ControllerState.PLAN_PRESENTED)

        ctrl.approve_plan()
        self.assertEqual(ctrl.state, ControllerState.APPLIED)


class TimeoutTest(unittest.TestCase):
    """A network timeout surfaces as 'offline' kind. We model this with a
    BackendNetworkError since StubBackend doesn't time out for real."""

    def test_timeout_models_as_offline(self):
        stub, _, api, panel, ctrl = _wire()
        stub.program_network_error("operation timed out")
        before = api.serialize()
        panel.type_prompt(ctrl, "x")
        _assert_clean_failure(api, panel, ctrl, kind="offline", before=before)


if __name__ == "__main__":  # pragma: no cover
    unittest.main()
