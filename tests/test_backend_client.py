"""Tests for BackendClient + the SSE parser (check 3 in §8)."""

from __future__ import annotations

import json
import unittest

from copper_integration.backend_client import (
    BackendClient,
    BackendHttpError,
    BackendNetworkError,
    BackendRateLimited,
    BackendSchemaError,
    BackendUnauthorized,
    Done,
    ErrorEvent,
    Message,
    PlanEvent,
    Stage,
    parse_sse_stream,
)
from copper_integration.settings import Settings
from copper_integration.stub_backend import StubBackend


def chunked(b: bytes, n: int = 32):
    for i in range(0, len(b), n):
        yield b[i:i + n]


class SseParserTest(unittest.TestCase):
    def test_single_event(self):
        evs = list(parse_sse_stream([b"event: stage\ndata: {\"name\":\"x\"}\n\n"]))
        self.assertEqual(evs, [("stage", '{"name":"x"}')])

    def test_default_event_message(self):
        evs = list(parse_sse_stream([b"data: hello\n\n"]))
        self.assertEqual(evs, [("message", "hello")])

    def test_multi_data_lines(self):
        evs = list(parse_sse_stream([b"event: done\ndata: line1\ndata: line2\n\n"]))
        self.assertEqual(evs, [("done", "line1\nline2")])

    def test_partial_chunks(self):
        raw = b"event: stage\ndata: {\"x\":1}\n\nevent: done\ndata: {}\n\n"
        evs = list(parse_sse_stream(chunked(raw, n=5)))
        self.assertEqual(evs, [("stage", '{"x":1}'), ("done", "{}")])

    def test_carriage_returns(self):
        raw = b"event: stage\r\ndata: x\r\n\r\n"
        evs = list(parse_sse_stream([raw]))
        self.assertEqual(evs, [("stage", "x")])

    def test_comments_ignored(self):
        raw = b": this is a comment\nevent: stage\ndata: x\n\n"
        evs = list(parse_sse_stream([raw]))
        self.assertEqual(evs, [("stage", "x")])

    def test_unknown_field_ignored(self):
        raw = b"id: 1\nretry: 5000\nevent: stage\ndata: x\n\n"
        evs = list(parse_sse_stream([raw]))
        self.assertEqual(evs, [("stage", "x")])

    def test_eof_without_blank_line_still_flushes(self):
        """The C++ parser dispatches whatever's buffered at EOF if there's
        data — match that."""
        raw = b"event: done\ndata: x\n"  # missing terminating blank line
        evs = list(parse_sse_stream([raw]))
        self.assertEqual(evs, [("done", "x")])

    def test_empty_stream(self):
        self.assertEqual(list(parse_sse_stream([])), [])

    def test_event_with_leading_space(self):
        evs = list(parse_sse_stream([b"event: stage\ndata: hello\n\n"]))
        self.assertEqual(evs, [("stage", "hello")])


class BackendClientChatTest(unittest.TestCase):
    def setUp(self):
        self.stub = StubBackend()
        self.client = BackendClient(self.stub, Settings(api_url="https://example.test",
                                                       saved_token="abc"))

    def test_happy_chat(self):
        self.stub.program_json_file("chat_response.json")
        resp = self.client.chat("explain bypass caps")
        self.assertTrue(resp.success)
        self.assertIn("bypass capacitor", resp.message.lower())

        req = self.stub.last_request()
        self.assertEqual(req.url, "https://example.test/api/v1/chat")
        self.assertEqual(req.headers["Authorization"], "Bearer abc")
        self.assertEqual(req.body["intent"], "chat")
        self.assertEqual(req.body["prompt"], "explain bypass caps")

    def test_request_building_no_token(self):
        client = BackendClient(self.stub, Settings(api_url="https://example.test"))
        self.stub.program_response({"protocol_version": 1, "success": True, "intent": "chat"})
        client.chat("hello")
        req = self.stub.last_request()
        self.assertNotIn("Authorization", req.headers)

    def test_user_agent_sent(self):
        self.stub.program_response({"protocol_version": 1, "success": True, "intent": "chat"})
        self.client.chat("x")
        req = self.stub.last_request()
        self.assertTrue(req.headers["User-Agent"].startswith("KiCad-Copper/"))

    def test_context_included(self):
        self.stub.program_response({"protocol_version": 1, "success": True, "intent": "chat"})
        self.client.chat("x", context={"selected_refs": ["U1"]})
        req = self.stub.last_request()
        self.assertEqual(req.body["context"], {"selected_refs": ["U1"]})

    def test_empty_prompt_rejected(self):
        with self.assertRaises(ValueError):
            self.client.chat("")
        with self.assertRaises(ValueError):
            self.client.chat("   ")

    def test_network_error(self):
        self.stub.program_network_error("DNS failed")
        with self.assertRaises(BackendNetworkError):
            self.client.chat("x")

    def test_401_unauthorized(self):
        self.stub.program_http_error(401, {"error": "expired"})
        with self.assertRaises(BackendUnauthorized) as cm:
            self.client.chat("x")
        self.assertEqual(cm.exception.status, 401)

    def test_403_unauthorized(self):
        self.stub.program_http_error(403, {"error": "no access"})
        with self.assertRaises(BackendUnauthorized):
            self.client.chat("x")

    def test_429_rate_limited(self):
        self.stub.program_http_error(429, {"error": "slow down", "retry_after": 7})
        with self.assertRaises(BackendRateLimited) as cm:
            self.client.chat("x")
        self.assertEqual(cm.exception.retry_after, 7.0)

    def test_500_generic(self):
        self.stub.program_http_error(500, {"error": "boom"})
        with self.assertRaises(BackendHttpError) as cm:
            self.client.chat("x")
        self.assertEqual(cm.exception.status, 500)

    def test_malformed_json_response(self):
        self.stub.program_malformed_json()
        with self.assertRaises(BackendSchemaError):
            self.client.chat("x")

    def test_schema_error_on_missing_protocol_version(self):
        self.stub.program_response({"success": True, "intent": "chat"})
        with self.assertRaises(BackendSchemaError) as cm:
            self.client.chat("x")
        # the underlying ValidationError.code is "no_version"
        self.assertEqual(cm.exception.detail.code, "no_version")


class BackendClientGenerateTest(unittest.TestCase):
    def setUp(self):
        self.stub = StubBackend()
        self.client = BackendClient(self.stub, Settings(api_url="https://example.test",
                                                       saved_token="abc"))

    def test_happy_stream_yields_typed_events(self):
        self.stub.program_sse_file("generate_happy.sse")
        events = list(self.client.generate("RP2040 board"))

        # Categorize.
        stages = [e for e in events if isinstance(e, Stage)]
        messages = [e for e in events if isinstance(e, Message)]
        plans = [e for e in events if isinstance(e, PlanEvent)]
        dones = [e for e in events if isinstance(e, Done)]
        errors = [e for e in events if isinstance(e, ErrorEvent)]

        self.assertGreaterEqual(len(stages), 4)
        self.assertGreaterEqual(len(messages), 1)
        self.assertEqual(len(plans), 1)
        self.assertEqual(len(dones), 1)
        self.assertEqual(len(errors), 0)

        # Done payload validated.
        self.assertTrue(dones[0].response.success)
        self.assertGreaterEqual(len(dones[0].response.operations), 5)

    def test_streaming_request_sets_accept_header(self):
        self.stub.program_sse_text("event: done\ndata: {\"protocol_version\":1,\"success\":true,\"intent\":\"generate\"}\n\n")
        list(self.client.generate("x"))
        req = self.stub.last_request()
        self.assertEqual(req.headers["Accept"], "text/event-stream")
        self.assertEqual(req.body["intent"], "generate")

    def test_error_event_terminates(self):
        self.stub.program_sse_file("generate_error.sse")
        events = list(self.client.generate("flux capacitor"))
        self.assertTrue(any(isinstance(e, ErrorEvent) for e in events))
        # No Done should follow the ErrorEvent
        self.assertFalse(any(isinstance(e, Done) for e in events))

    def test_malformed_done_raises_schema_error(self):
        self.stub.program_sse_file("generate_malformed_done.sse")
        with self.assertRaises(BackendSchemaError):
            list(self.client.generate("x"))

    def test_no_done_synthesizes_error_event(self):
        self.stub.program_sse_file("generate_partial_no_done.sse")
        events = list(self.client.generate("x"))
        errors = [e for e in events if isinstance(e, ErrorEvent)]
        self.assertEqual(len(errors), 1)
        self.assertEqual(errors[0].code, "no_done")

    def test_bad_event_json_yields_error(self):
        bad = b"event: stage\ndata: {not json\n\n"
        self.stub.program_sse(bad)
        events = list(self.client.generate("x"))
        # The parser should produce one ErrorEvent.
        self.assertTrue(any(isinstance(e, ErrorEvent) and e.code == "bad_event_json"
                            for e in events))

    def test_401_during_stream_setup(self):
        self.stub.program_http_error(401, {"error": "expired"})
        with self.assertRaises(BackendUnauthorized):
            list(self.client.generate("x"))


class TimeoutResolutionTest(unittest.TestCase):
    def test_streaming_uses_idle_timeout(self):
        s = Settings(api_url="https://x.y", timeout_seconds=10.0,
                     stream_idle_timeout_seconds=99.0)
        stub = StubBackend()
        stub.program_sse_text("event: done\ndata: {\"protocol_version\":1,\"success\":true,\"intent\":\"generate\"}\n\n")
        client = BackendClient(stub, s)
        list(client.generate("x"))
        self.assertEqual(stub.last_request().timeout, 99.0)

    def test_chat_uses_short_timeout(self):
        s = Settings(api_url="https://x.y", timeout_seconds=10.0,
                     stream_idle_timeout_seconds=99.0)
        stub = StubBackend()
        stub.program_response({"protocol_version": 1, "success": True, "intent": "chat"})
        client = BackendClient(stub, s)
        client.chat("x")
        self.assertEqual(stub.last_request().timeout, 10.0)


if __name__ == "__main__":  # pragma: no cover
    unittest.main()


class NestedDoneUnwrapTest(unittest.TestCase):
    """Backend <= v0.1.0 nested the CopperResponse under data["plan"] in the
    `done` event. The client unwraps it (mirroring copper_chat_panel.cpp)."""

    def setUp(self):
        self.stub = StubBackend()
        self.client = BackendClient(self.stub, Settings(api_url="https://example.test",
                                                        saved_token="abc"))

    def test_nested_done_payload_is_unwrapped(self):
        inner = {
            "protocol_version": 1,
            "success": True,
            "intent": "generate",
            "message": "Compiled board",
            "operations": [
                {"type": "PLACE_COMPONENT",
                 "data": {"lib_id": "Device:R", "reference": "R1", "value": "10k",
                          "x": 2540000, "y": 2540000, "rotation": 0.0}},
            ],
            "plan": {"steps": [{"index": 0, "description": "Place R1"}],
                     "placement_info": ""},
            "erc": None,
            "error": "",
        }
        nested = {"type": "done", "plan": inner}
        self.stub.program_sse_text(
            "event: done\ndata: " + json.dumps(nested) + "\n\n")

        events = list(self.client.generate("a resistor"))
        dones = [e for e in events if isinstance(e, Done)]
        self.assertEqual(len(dones), 1)
        self.assertTrue(dones[0].response.success)
        self.assertEqual(len(dones[0].response.operations), 1)
        self.assertEqual(dones[0].response.operations[0].data["reference"], "R1")
