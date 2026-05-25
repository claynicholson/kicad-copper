"""End-to-end Controller tests. Drives prompt → generate → plan → apply →
undo using the FakeSchematicApi + StubBackend, and the FakeChatPanel
observer to verify the panel-side hooks fire in the right order.

These mirror the §5 core flows. They are the closest thing to a real
in-editor test we can do without running KiCad."""

from __future__ import annotations

import unittest

from copper_integration.apply_engine import ApplyEngine
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


def _client_and_stub():
    stub = StubBackend()
    client = BackendClient(stub, Settings(api_url="https://x.y", saved_token="t"))
    return client, stub


class HappyPathTest(unittest.TestCase):
    """§5 happy path: prompt → stages stream → plan → apply → APPLIED."""

    def test_e2e(self):
        client, stub = _client_and_stub()
        stub.program_sse_file("generate_happy.sse")

        api = FakeSchematicApi()
        panel = FakeChatPanel()
        ctrl = Controller(client, api, panel)

        before = api.serialize()

        panel.type_prompt(ctrl, "RP2040 dev board with a 6-axis IMU for a flight controller")

        # Now in PLAN_PRESENTED with the Done captured.
        self.assertEqual(ctrl.state, ControllerState.PLAN_PRESENTED)
        self.assertIsNotNone(panel.last_response)
        self.assertGreater(len(panel.stages), 0)
        self.assertIn("choosing_parts", panel.stages)
        self.assertEqual(panel.stages["placement"], "complete")
        # Plan card payload was rendered.
        self.assertIsNotNone(panel.last_plan)
        self.assertGreater(len(panel.last_plan.steps), 0)
        # No apply has happened yet.
        self.assertEqual(api.serialize(), before)
        # Mid-stream message bubble received.
        self.assertGreaterEqual(len(panel.ai_bubbles), 1)

        # User approves.
        ctrl.approve_plan()
        self.assertEqual(ctrl.state, ControllerState.APPLIED)
        self.assertIsNotNone(panel.last_apply)
        self.assertTrue(panel.last_apply.ok)

        # Real change happened.
        self.assertNotEqual(api.serialize(), before)
        # 4 symbols (U1, U2, +3V3, GND) per fixture.
        self.assertEqual(len(api.list_symbols()), 4)

        # Single undo reverts the whole apply.
        ctrl.undo_last_apply()
        self.assertEqual(api.serialize(), before)


class CancelPlanTest(unittest.TestCase):
    def test_cancel_after_plan_returns_to_idle(self):
        client, stub = _client_and_stub()
        stub.program_sse_file("generate_happy.sse")

        api = FakeSchematicApi()
        panel = FakeChatPanel()
        ctrl = Controller(client, api, panel)

        before = api.serialize()
        panel.type_prompt(ctrl, "x")
        self.assertEqual(ctrl.state, ControllerState.PLAN_PRESENTED)

        ctrl.cancel_plan()
        self.assertEqual(ctrl.state, ControllerState.IDLE)
        self.assertEqual(api.serialize(), before)
        # We should be able to start a new generate from IDLE.
        stub.program_sse_file("generate_happy.sse")
        panel.type_prompt(ctrl, "again")
        self.assertEqual(ctrl.state, ControllerState.PLAN_PRESENTED)


class OfflineTest(unittest.TestCase):
    def test_network_error_yields_offline_state(self):
        client, stub = _client_and_stub()
        stub.program_network_error("DNS failed")
        api = FakeSchematicApi()
        panel = FakeChatPanel()
        ctrl = Controller(client, api, panel)

        before = api.serialize()
        panel.type_prompt(ctrl, "x")
        self.assertEqual(ctrl.state, ControllerState.ERROR_STATE)
        self.assertIsNotNone(panel.last_error)
        self.assertEqual(panel.last_error.kind, "offline")
        # Nothing applied.
        self.assertEqual(api.serialize(), before)

    def test_no_done_yields_offline(self):
        client, stub = _client_and_stub()
        stub.program_sse_file("generate_partial_no_done.sse")
        api = FakeSchematicApi()
        panel = FakeChatPanel()
        ctrl = Controller(client, api, panel)

        before = api.serialize()
        panel.type_prompt(ctrl, "x")
        self.assertEqual(ctrl.state, ControllerState.ERROR_STATE)
        self.assertIsNotNone(panel.last_error)
        self.assertEqual(panel.last_error.kind, "no_done")
        self.assertEqual(api.serialize(), before)


class UnauthorizedTest(unittest.TestCase):
    def test_401_triggers_unauthorized_state(self):
        client, stub = _client_and_stub()
        stub.program_http_error(401, {"error": "expired"})
        api = FakeSchematicApi()
        panel = FakeChatPanel()
        ctrl = Controller(client, api, panel)
        before = api.serialize()
        panel.type_prompt(ctrl, "x")
        self.assertEqual(ctrl.state, ControllerState.ERROR_STATE)
        self.assertEqual(panel.last_error.kind, "unauthorized")
        self.assertEqual(api.serialize(), before)


class RateLimitedTest(unittest.TestCase):
    def test_429_surfaces_retry_after(self):
        client, stub = _client_and_stub()
        stub.program_http_error(429, {"error": "slow down", "retry_after": 5})
        api = FakeSchematicApi()
        panel = FakeChatPanel()
        ctrl = Controller(client, api, panel)
        panel.type_prompt(ctrl, "x")
        self.assertEqual(panel.last_error.kind, "rate_limited")
        self.assertEqual(panel.last_error.retry_after, 5.0)


class MalformedResponseTest(unittest.TestCase):
    def test_malformed_done_yields_schema_state(self):
        client, stub = _client_and_stub()
        stub.program_sse_file("generate_malformed_done.sse")
        api = FakeSchematicApi()
        panel = FakeChatPanel()
        ctrl = Controller(client, api, panel)
        before = api.serialize()
        panel.type_prompt(ctrl, "x")
        # The BackendClient raises BackendSchemaError mid-stream;
        # the controller catches it via the chunk loop and emits an error.
        # Actually — generate() on the client raises BackendSchemaError on
        # an invalid `done`; let's verify what kind reaches us.
        self.assertIn(panel.last_error.kind, ("schema", "unknown"))
        self.assertEqual(api.serialize(), before)


class ChatModeTest(unittest.TestCase):
    def test_chat_returns_message_without_plan(self):
        client, stub = _client_and_stub()
        stub.program_json_file("chat_response.json")
        api = FakeSchematicApi()
        panel = FakeChatPanel()
        ctrl = Controller(client, api, panel)

        ctrl.chat("explain bypass caps")
        self.assertEqual(ctrl.state, ControllerState.APPLIED)
        self.assertIsNotNone(panel.last_response)
        self.assertIn("bypass capacitor", panel.last_response.message.lower())


class StateMachineTest(unittest.TestCase):
    def test_states_sequence_for_happy_path(self):
        client, stub = _client_and_stub()
        stub.program_sse_file("generate_happy.sse")
        api = FakeSchematicApi()
        panel = FakeChatPanel()
        ctrl = Controller(client, api, panel)

        panel.type_prompt(ctrl, "x")
        self.assertEqual(
            panel.states,
            [
                ControllerState.SENDING,
                ControllerState.STREAMING,
                ControllerState.PLAN_PRESENTED,
            ],
        )

        ctrl.approve_plan()
        # Continue from PLAN_PRESENTED.
        self.assertEqual(
            panel.states[-2:],
            [ControllerState.APPLYING, ControllerState.APPLIED],
        )


class GenerateFromErrorStateTest(unittest.TestCase):
    """User retries after a failure."""

    def test_can_retry_after_offline_error(self):
        client, stub = _client_and_stub()
        stub.program_network_error("first time")
        api = FakeSchematicApi()
        panel = FakeChatPanel()
        ctrl = Controller(client, api, panel)
        panel.type_prompt(ctrl, "x")
        self.assertEqual(ctrl.state, ControllerState.ERROR_STATE)

        stub.program_sse_file("generate_happy.sse")
        panel.type_prompt(ctrl, "x")
        self.assertEqual(ctrl.state, ControllerState.PLAN_PRESENTED)


if __name__ == "__main__":  # pragma: no cover
    unittest.main()
