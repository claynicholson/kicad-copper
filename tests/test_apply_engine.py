"""Tests for ApplyEngine — checks 5 (apply correctness), 6 (atomic rollback),
7 (applied-board quality), and 4 (response validation enforcement).

The "canned apply-plan" referenced by §8 check 5 lives in
fixtures/generate_happy.sse — we extract its `done` payload and feed it to
the engine. The expected post-apply state is hand-checked against PROTOCOL.md
op shapes."""

from __future__ import annotations

import json
import pathlib
import unittest
from typing import List, Tuple

from copper_integration.apply_engine import ApplyEngine, ApplyError, ApplyResult
from copper_integration.backend_client import BackendClient
from copper_integration.schematic_api import (
    FakeSchematicApi,
    Pin,
    SchematicError,
)
from copper_integration.settings import Settings
from copper_integration.stub_backend import StubBackend
from copper_integration.validators import (
    PROTOCOL_VERSION,
    ValidatedOperation,
    ValidatedResponse,
    validate_response,
)


# ── helpers ────────────────────────────────────────────────────────────────

_UNSET = object()  # sentinel: "no page key on the response" vs. an explicit one


def _happy_response():
    """Fetch the canonical happy-path response by replaying generate_happy.sse
    through the real BackendClient (sourced from PROTOCOL.md). This anchors
    the test to the same canned plan the harness's check 5 uses."""
    stub = StubBackend()
    stub.program_sse_file("generate_happy.sse")
    client = BackendClient(stub, Settings(api_url="https://x.y", saved_token="t"))
    for ev in client.generate("rp2040 board"):
        # Done is always last.
        if ev.__class__.__name__ == "Done":
            return ev.response
    raise AssertionError("no Done in happy fixture")


def _mk_response(ops, *, success=True, intent="generate", message="", page=_UNSET):
    """Build a raw response dict (then validate). When `page` is provided it is
    placed verbatim on the response so we can exercise the optional §page hint
    (including malformed shapes)."""
    raw = {
        "protocol_version": PROTOCOL_VERSION,
        "success": success,
        "intent": intent,
        "message": message,
        "operations": ops,
        "error": "" if success else "boom",
    }
    if page is not _UNSET:
        raw["page"] = page
    return validate_response(raw)


def _place(ref, x, y, lib="L:S", val="v", footprint=None):
    data = {
        "lib_id": lib, "reference": ref, "value": val,
        "x": x, "y": y, "rotation": 0.0,
    }
    if footprint is not None:
        data["footprint"] = footprint
    return {"type": "PLACE_COMPONENT", "data": data}


def _wire(sx, sy, ex, ey):
    return {"type": "ADD_WIRE", "data": {
        "start_x": sx, "start_y": sy, "end_x": ex, "end_y": ey
    }}


def _label(name, x, y, kind="local"):
    return {"type": "ADD_LABEL", "data": {
        "name": name, "x": x, "y": y, "label_type": kind
    }}


def _junction(x, y):
    return {"type": "ADD_JUNCTION", "data": {"x": x, "y": y}}


def _power(net, x, y):
    return {"type": "ADD_POWER_SYMBOL", "data": {
        "net_name": net, "x": x, "y": y
    }}


def _pin_label(ref, pin, net, style="global"):
    return {"type": "ADD_PIN_LABEL", "data": {
        "reference": ref, "pin": pin, "net_name": net, "style": style,
    }}


def _no_connect(ref, pin):
    return {"type": "ADD_NO_CONNECT", "data": {"reference": ref, "pin": pin}}


# ── Check 5: apply correctness ────────────────────────────────────────────


class ApplyCorrectnessTest(unittest.TestCase):
    def test_happy_path_produces_expected_state(self):
        api = FakeSchematicApi()
        engine = ApplyEngine()
        resp = _happy_response()

        before = api.snapshot_hash()
        result = engine.apply_validated(api, resp)
        self.assertTrue(result.ok)
        self.assertIsNotNone(result.commit_id)
        self.assertNotEqual(api.snapshot_hash(), before)

        # Two real symbols + two power symbols.
        symbols = api.list_symbols()
        self.assertEqual(len(symbols), 4)

        refs = {s.reference for s in symbols if not s.is_power}
        self.assertEqual(refs, {"U1", "U2"})

        # Power symbol nets
        power_nets = {s.value for s in symbols if s.is_power}
        self.assertEqual(power_nets, {"+3V3", "GND"})

        # SDA / SCL labels
        label_names = {la.name for la in api.list_labels()}
        self.assertEqual(label_names, {"SDA", "SCL"})

        # Two wires + two junctions
        self.assertEqual(len(api.list_wires()), 2)
        self.assertEqual(len(api.list_junctions()), 2)

        # All assigned ids are unique
        ids = (
            [s.id for s in symbols]
            + [w.id for w in api.list_wires()]
            + [la.id for la in api.list_labels()]
            + [j.id for j in api.list_junctions()]
        )
        self.assertEqual(len(set(ids)), len(ids))

    def test_op_counts_in_result(self):
        api = FakeSchematicApi()
        engine = ApplyEngine()
        resp = _happy_response()
        result = engine.apply_validated(api, resp)
        # 2 PLACE + 2 POWER + 2 LABEL + 2 WIRE + 2 JUNCTION = 10
        self.assertEqual(result.placed_symbols, 2)
        self.assertEqual(result.power_symbols, 2)
        self.assertEqual(result.labels, 2)
        self.assertEqual(result.wires, 2)
        self.assertEqual(result.junctions, 2)

    def test_empty_plan_is_no_op(self):
        api = FakeSchematicApi()
        before = api.snapshot_hash()
        engine = ApplyEngine()
        resp = _mk_response([])
        result = engine.apply_validated(api, resp)
        self.assertTrue(result.ok)
        self.assertIsNone(result.commit_id)
        self.assertEqual(api.snapshot_hash(), before)

    def test_success_false_rejected(self):
        api = FakeSchematicApi()
        engine = ApplyEngine()
        resp = _mk_response([_place("U1", 0, 0)], success=False)
        with self.assertRaises(ApplyError) as cm:
            engine.apply_validated(api, resp)
        self.assertEqual(cm.exception.code, "backend_failure")

    # ── footprint plumbing (validators → engine → fake symbol) ──

    def test_footprint_applied_to_placed_symbol(self):
        api = FakeSchematicApi()
        engine = ApplyEngine()
        fp = "Resistor_SMD:R_0603_1608Metric"
        resp = _mk_response([_place("R1", 0, 0, footprint=fp)])
        result = engine.apply_validated(api, resp)
        self.assertTrue(result.ok)
        (sym,) = api.list_symbols()
        self.assertEqual(sym.footprint, fp)

    def test_missing_footprint_defaults_to_empty(self):
        api = FakeSchematicApi()
        engine = ApplyEngine()
        resp = _mk_response([_place("R1", 0, 0)])  # no footprint key
        engine.apply_validated(api, resp)
        (sym,) = api.list_symbols()
        self.assertEqual(sym.footprint, "")

    def test_empty_footprint_kept_empty(self):
        api = FakeSchematicApi()
        engine = ApplyEngine()
        resp = _mk_response([_place("R1", 0, 0, footprint="")])
        engine.apply_validated(api, resp)
        (sym,) = api.list_symbols()
        self.assertEqual(sym.footprint, "")

    # ── §page: sheet resize on apply ──

    def test_page_present_calls_set_page_and_serializes(self):
        api = FakeSchematicApi()
        engine = ApplyEngine()
        resp = _mk_response(
            [_place("R1", 0, 0)],
            page={"size": "A3", "width_mm": 420.0, "height_mm": 297.0},
        )
        result = engine.apply_validated(api, resp)
        self.assertTrue(result.ok)
        self.assertEqual(api.page, "A3")
        # The page is part of the committed, serialized state.
        self.assertIn(b'"page":"A3"', api.serialize())

    def test_page_absent_is_no_op(self):
        api = FakeSchematicApi()
        engine = ApplyEngine()
        resp = _mk_response([_place("R1", 0, 0)])  # no page key
        result = engine.apply_validated(api, resp)
        self.assertTrue(result.ok)
        self.assertIsNone(api.page)
        self.assertIn(b'"page":null', api.serialize())

    def test_page_malformed_is_tolerated_no_resize(self):
        # A malformed page (no usable standard size) must never crash the apply
        # nor resize the sheet — fail-closed.
        for bad in (
            {"width_mm": 100},   # no size
            {"size": 123},       # size wrong type → coerced to "" → no resize
            {"size": "Letter"},  # non-standard size name → rejected by set_page
            "A3",                # not even an object → page is None
            [],                  # wrong container → page is None
        ):
            api = FakeSchematicApi()
            engine = ApplyEngine()
            resp = _mk_response([_place("R1", 0, 0)], page=bad)
            result = engine.apply_validated(api, resp)  # must not raise
            self.assertTrue(result.ok)
            self.assertIsNone(api.page, f"bad page {bad!r} should not resize")

    def test_page_valid_size_with_junk_dimensions_still_resizes(self):
        # The size name is authoritative — junk width/height is tolerated and
        # the resize still happens to the named standard page.
        api = FakeSchematicApi()
        engine = ApplyEngine()
        resp = _mk_response(
            [_place("R1", 0, 0)],
            page={"size": "A3", "width_mm": "wide"},  # junk numeric, valid size
        )
        result = engine.apply_validated(api, resp)
        self.assertTrue(result.ok)
        self.assertEqual(api.page, "A3")

    def test_page_resize_reversed_by_single_undo(self):
        api = FakeSchematicApi()
        engine = ApplyEngine()
        before = api.serialize()
        resp = _mk_response(
            [_place("R1", 0, 0)],
            page={"size": "A2", "width_mm": 594.0, "height_mm": 420.0},
        )
        engine.apply_validated(api, resp)
        self.assertEqual(api.page, "A2")
        api.undo()
        self.assertIsNone(api.page)
        self.assertEqual(api.serialize(), before,
                         "one undo() did not reverse the page resize")

    def test_invalid_footprint_hard_rejects_plan(self):
        api = FakeSchematicApi()
        engine = ApplyEngine()
        before = api.serialize()
        raw = {
            "protocol_version": PROTOCOL_VERSION,
            "success": True,
            "intent": "generate",
            "message": "",
            "operations": [_place("R1", 0, 0, footprint="no_colon_here")],
            "error": "",
        }
        with self.assertRaises(ApplyError) as cm:
            engine.apply_response(api, raw)
        self.assertEqual(cm.exception.code, "response_invalid")
        self.assertEqual(api.serialize(), before)

    def test_non_string_footprint_hard_rejects_plan(self):
        api = FakeSchematicApi()
        engine = ApplyEngine()
        raw = {
            "protocol_version": PROTOCOL_VERSION,
            "success": True,
            "intent": "generate",
            "message": "",
            "operations": [_place("R1", 0, 0, footprint=123)],
            "error": "",
        }
        with self.assertRaises(ApplyError) as cm:
            engine.apply_response(api, raw)
        self.assertEqual(cm.exception.code, "response_invalid")

    # ── multi-pad pin-label / no-connect resolution (BUG FIX) ──
    #
    # On a USB-C connector the pin NAME "D+" sits on TWO pads (A6 and B6).
    # A name token must label/NC EVERY matching pad — leaving a sibling pad
    # dangling is what ERC-failed in the field. A pin NUMBER token resolves
    # to exactly one pad.

    def _usb_c_api(self):
        api = FakeSchematicApi()
        # D+ repeats across A6/B6, GND across A1/B1 — classic USB-C duplication.
        api.register_pins("J1", [
            Pin(number="A1", name="GND", x=0, y=0),
            Pin(number="A6", name="D+", x=100, y=0),
            Pin(number="A7", name="D-", x=200, y=0),
            Pin(number="B1", name="GND", x=0, y=300),
            Pin(number="B6", name="D+", x=100, y=300),
            Pin(number="B7", name="D-", x=200, y=300),
        ])
        return api

    def test_pin_label_by_name_hits_every_matching_pad(self):
        api = self._usb_c_api()
        engine = ApplyEngine()
        resp = _mk_response([
            _place("J1", -1000, -1000, lib="Connector:USB_C_Receptacle"),
            _pin_label("J1", "D+", "USB_DP"),
        ])
        result = engine.apply_validated(api, resp)
        self.assertTrue(result.ok)
        # D+ is on A6 AND B6 → two labels, both named USB_DP.
        labels = [la for la in api.list_labels() if la.name == "USB_DP"]
        self.assertEqual(len(labels), 2)
        self.assertEqual({(la.x, la.y) for la in labels}, {(100, 0), (100, 300)})

    def test_no_connect_by_name_hits_every_matching_pad(self):
        api = self._usb_c_api()
        engine = ApplyEngine()
        resp = _mk_response([
            _place("J1", -1000, -1000, lib="Connector:USB_C_Receptacle"),
            _no_connect("J1", "D+"),
        ])
        result = engine.apply_validated(api, resp)
        self.assertTrue(result.ok)
        # Two pads named D+ → two no-connects.
        ncs = api.list_no_connects()
        self.assertEqual(len(ncs), 2)
        self.assertEqual({(nc.x, nc.y) for nc in ncs}, {(100, 0), (100, 300)})

    def test_pin_label_by_number_hits_exactly_one_pad(self):
        # The backend emits per-NUMBER normally; a number must anchor one pad.
        api = self._usb_c_api()
        engine = ApplyEngine()
        resp = _mk_response([
            _place("J1", -1000, -1000, lib="Connector:USB_C_Receptacle"),
            _pin_label("J1", "B6", "USB_DP"),
        ])
        result = engine.apply_validated(api, resp)
        self.assertTrue(result.ok)
        labels = [la for la in api.list_labels() if la.name == "USB_DP"]
        self.assertEqual(len(labels), 1)
        self.assertEqual((labels[0].x, labels[0].y), (100, 300))

    def test_no_connect_by_number_hits_exactly_one_pad(self):
        api = self._usb_c_api()
        engine = ApplyEngine()
        resp = _mk_response([
            _place("J1", -1000, -1000, lib="Connector:USB_C_Receptacle"),
            _no_connect("J1", "A6"),
        ])
        result = engine.apply_validated(api, resp)
        self.assertTrue(result.ok)
        ncs = api.list_no_connects()
        self.assertEqual(len(ncs), 1)
        self.assertEqual((ncs[0].x, ncs[0].y), (100, 0))

    def test_unresolved_pin_fails_closed(self):
        api = self._usb_c_api()
        engine = ApplyEngine()
        before = api.serialize()
        resp = _mk_response([
            _place("J1", -1000, -1000, lib="Connector:USB_C_Receptacle"),
            _pin_label("J1", "NOPE", "USB_DP"),
        ])
        with self.assertRaises(ApplyError) as cm:
            engine.apply_validated(api, resp)
        self.assertEqual(cm.exception.code, "op_failed")
        # Atomic: nothing applied.
        self.assertEqual(api.serialize(), before)

    def test_happy_fixture_footprints_land_on_symbols(self):
        # generate_happy.sse carries footprints for U1/U2 and none for the
        # power symbols — exactly the new backend output shape.
        api = FakeSchematicApi()
        engine = ApplyEngine()
        engine.apply_validated(api, _happy_response())
        by_ref = {s.reference: s for s in api.list_symbols()}
        self.assertEqual(
            by_ref["U1"].footprint,
            "Package_QFN:QFN-56-1EP_7x7mm_P0.4mm_EP3.2x3.2mm",
        )
        self.assertEqual(
            by_ref["U2"].footprint,
            "Package_LGA:LGA-14_2.5x3mm_P0.5mm_LayoutBorder3x4y",
        )
        for s in api.list_symbols():
            if s.is_power:
                self.assertEqual(s.footprint, "")


# ── Check 6: atomic rollback ──────────────────────────────────────────────


class AtomicRollbackTest(unittest.TestCase):
    def test_failure_mid_plan_leaves_byte_equal_state(self):
        api = FakeSchematicApi()
        engine = ApplyEngine()

        # Pre-populate something so the snapshot is non-trivial.
        tok = api.begin_commit("setup")
        api.place_component(tok, {
            "lib_id": "L:S", "reference": "U99", "value": "v",
            "x": 100_000_000, "y": 100_000_000, "rotation": 0.0,
        })
        api.push_commit(tok)
        before = api.serialize()
        before_hash = api.snapshot_hash()

        # Inject a failure on the 3rd op.
        api.inject_failure(after_n_in_commit=2)

        bad_plan = _mk_response([
            _place("A", 0, 0),
            _place("B", 1_000_000, 0),
            _place("C", 2_000_000, 0),  # boom here
            _place("D", 3_000_000, 0),
        ])

        with self.assertRaises(ApplyError) as cm:
            engine.apply_validated(api, bad_plan)
        self.assertEqual(cm.exception.code, "op_failed")

        # Byte-equal recovery.
        self.assertEqual(api.serialize(), before)
        self.assertEqual(api.snapshot_hash(), before_hash)

        # Nothing was committed.
        self.assertEqual(api.committed_commit_count(), 1)  # only the setup commit
        self.assertEqual(api.open_commit_count(), 0)

    def test_failure_on_first_op_leaves_byte_equal(self):
        api = FakeSchematicApi()
        engine = ApplyEngine()
        before = api.serialize()

        api.inject_failure(op_kind="place_component")
        plan = _mk_response([_place("A", 0, 0)])
        with self.assertRaises(ApplyError):
            engine.apply_validated(api, plan)
        self.assertEqual(api.serialize(), before)

    def test_one_undo_reverses_whole_apply(self):
        """The §10 'one Undo reverts a whole apply' requirement."""
        api = FakeSchematicApi()
        engine = ApplyEngine()
        before = api.serialize()

        resp = _happy_response()
        engine.apply_validated(api, resp)
        self.assertNotEqual(api.serialize(), before)

        # One undo.
        api.undo()
        self.assertEqual(api.serialize(), before)

    def test_validation_failure_doesnt_open_commit(self):
        api = FakeSchematicApi()
        engine = ApplyEngine()
        before = api.serialize()

        raw = {
            "protocol_version": PROTOCOL_VERSION,
            "success": True,
            "intent": "generate",
            "operations": [{"type": "PLACE_COMPONENT", "data": {
                # missing lib_id triggers validator
                "reference": "U1", "value": "v", "x": 0, "y": 0,
            }}],
        }
        with self.assertRaises(ApplyError) as cm:
            engine.apply_response(api, raw)
        self.assertEqual(cm.exception.code, "response_invalid")
        self.assertEqual(api.serialize(), before)
        self.assertEqual(api.open_commit_count(), 0)


# ── Check 7: applied-board quality ────────────────────────────────────────


class AppliedBoardQualityTest(unittest.TestCase):
    def test_overlap_within_plan_rejected(self):
        api = FakeSchematicApi()
        engine = ApplyEngine()
        before = api.serialize()
        plan = _mk_response([
            _place("U1", 0, 0),
            _place("U2", 0, 0),  # same anchor as U1
        ])
        with self.assertRaises(ApplyError) as cm:
            engine.apply_validated(api, plan)
        self.assertEqual(cm.exception.code, "symbol_overlap")
        self.assertEqual(api.serialize(), before)

    def test_overlap_with_existing_rejected(self):
        api = FakeSchematicApi()
        engine = ApplyEngine()
        # Pre-place a symbol at (0,0)
        tok = api.begin_commit("setup")
        api.place_component(tok, {
            "lib_id": "L:S", "reference": "U99", "value": "v",
            "x": 0, "y": 0, "rotation": 0.0,
        })
        api.push_commit(tok)
        before = api.serialize()

        plan = _mk_response([_place("U1", 0, 0)])  # overlaps existing
        with self.assertRaises(ApplyError) as cm:
            engine.apply_validated(api, plan)
        self.assertEqual(cm.exception.code, "overlap_with_existing")
        self.assertEqual(api.serialize(), before)

    def test_ref_conflict_with_existing_rejected(self):
        api = FakeSchematicApi()
        engine = ApplyEngine()
        # Pre-place U1
        tok = api.begin_commit("setup")
        api.place_component(tok, {
            "lib_id": "L:S", "reference": "U1", "value": "v",
            "x": 0, "y": 0, "rotation": 0.0,
        })
        api.push_commit(tok)
        before = api.serialize()

        plan = _mk_response([_place("U1", 1_000_000, 0)])  # ref collision
        with self.assertRaises(ApplyError) as cm:
            engine.apply_validated(api, plan)
        self.assertEqual(cm.exception.code, "ref_already_on_sheet")
        self.assertEqual(api.serialize(), before)

    def test_happy_plan_has_zero_overlap(self):
        api = FakeSchematicApi()
        engine = ApplyEngine()
        resp = _happy_response()
        engine.apply_validated(api, resp)

        # Build (ref, x, y) list and check no duplicates among non-power refs.
        from copper_integration.validators import check_no_symbol_overlap
        triples = [(s.reference, s.x, s.y) for s in api.list_symbols()]
        self.assertEqual(check_no_symbol_overlap(triples), [])

    def test_all_plan_refs_realized(self):
        api = FakeSchematicApi()
        engine = ApplyEngine()
        resp = _happy_response()
        engine.apply_validated(api, resp)

        plan_refs = {
            op.data["reference"]
            for op in resp.operations
            if op.type == "PLACE_COMPONENT"
        }
        actual_refs = {s.reference for s in api.list_symbols() if not s.is_power}
        self.assertEqual(plan_refs, actual_refs)

    def test_all_plan_labels_realized(self):
        api = FakeSchematicApi()
        engine = ApplyEngine()
        resp = _happy_response()
        engine.apply_validated(api, resp)

        plan_label_names = {
            op.data["name"]
            for op in resp.operations
            if op.type == "ADD_LABEL"
        }
        actual_label_names = {la.name for la in api.list_labels()}
        self.assertEqual(plan_label_names, actual_label_names)


# ── Check 4: response validation enforcement ──────────────────────────────


class ResponseValidationEnforcementTest(unittest.TestCase):
    """Verify that ApplyEngine rejects bad responses before touching the
    schematic. This is the integration of Check 4 with the apply path."""

    def test_missing_protocol_version(self):
        api = FakeSchematicApi()
        engine = ApplyEngine()
        before = api.serialize()
        bad = {"success": True, "intent": "generate", "operations": []}
        with self.assertRaises(ApplyError) as cm:
            engine.apply_response(api, bad)
        self.assertEqual(cm.exception.code, "response_invalid")
        self.assertEqual(api.serialize(), before)

    def test_unknown_op_type(self):
        api = FakeSchematicApi()
        engine = ApplyEngine()
        before = api.serialize()
        bad = {
            "protocol_version": PROTOCOL_VERSION,
            "success": True,
            "intent": "generate",
            "operations": [{"type": "DOES_NOT_EXIST", "data": {}}],
        }
        with self.assertRaises(ApplyError) as cm:
            engine.apply_response(api, bad)
        self.assertEqual(cm.exception.code, "response_invalid")
        self.assertEqual(api.serialize(), before)

    def test_coord_out_of_range(self):
        api = FakeSchematicApi()
        engine = ApplyEngine()
        before = api.serialize()
        bad = {
            "protocol_version": PROTOCOL_VERSION,
            "success": True,
            "intent": "generate",
            "operations": [{"type": "PLACE_COMPONENT", "data": {
                "lib_id": "L:S", "reference": "U1", "value": "v",
                "x": 2_000_000_000, "y": 0, "rotation": 0.0,
            }}],
        }
        with self.assertRaises(ApplyError) as cm:
            engine.apply_response(api, bad)
        self.assertEqual(cm.exception.code, "response_invalid")
        self.assertEqual(api.serialize(), before)

    def test_duplicate_ref_in_plan(self):
        api = FakeSchematicApi()
        engine = ApplyEngine()
        before = api.serialize()
        bad = {
            "protocol_version": PROTOCOL_VERSION,
            "success": True,
            "intent": "generate",
            "operations": [_place("U1", 0, 0), _place("U1", 1000, 0)],
        }
        with self.assertRaises(ApplyError) as cm:
            engine.apply_response(api, bad)
        self.assertEqual(cm.exception.code, "response_invalid")
        self.assertEqual(api.serialize(), before)


# ── Pre-applied-snapshot safety net ────────────────────────────────────────


class PreSnapshotGuardTest(unittest.TestCase):
    def test_caller_passed_snapshot_must_match_after_rollback(self):
        api = FakeSchematicApi()
        engine = ApplyEngine()
        snap = api.serialize()

        api.inject_failure(op_kind="add_wire")
        plan = _mk_response([_place("A", 0, 0), _wire(0, 0, 1000, 0)])
        with self.assertRaises(ApplyError):
            engine.apply_validated(api, plan, pre_state_snapshot=snap)
        self.assertEqual(api.serialize(), snap)


if __name__ == "__main__":  # pragma: no cover
    unittest.main()
