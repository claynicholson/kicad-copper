"""Tests for the PROTOCOL.md hard-rejects in validators.py.

Each case asserts that an invalid response/op shape raises ValidationError
with a stable .code so the harness can match by code."""

from __future__ import annotations

import unittest

from copper_integration.validators import (
    PROTOCOL_VERSION,
    ValidationError,
    validate_operation,
    validate_response,
)


def _ok_response(**overrides):
    base = {
        "protocol_version": 1,
        "success": True,
        "intent": "chat",
        "message": "hello",
        "operations": [],
        "error": "",
    }
    base.update(overrides)
    return base


class ResponseShapeTest(unittest.TestCase):
    def test_minimal_ok(self):
        v = validate_response(_ok_response())
        self.assertTrue(v.success)
        self.assertEqual(v.intent, "chat")

    def test_missing_protocol_version(self):
        with self.assertRaises(ValidationError) as cm:
            validate_response({"success": True, "intent": "chat"})
        self.assertEqual(cm.exception.code, "no_version")

    def test_future_protocol_version(self):
        with self.assertRaises(ValidationError) as cm:
            validate_response(_ok_response(protocol_version=PROTOCOL_VERSION + 1))
        self.assertEqual(cm.exception.code, "future_version")

    def test_protocol_version_wrong_type(self):
        with self.assertRaises(ValidationError) as cm:
            validate_response(_ok_response(protocol_version="1"))
        self.assertEqual(cm.exception.code, "bad_version_type")

    def test_protocol_version_zero(self):
        with self.assertRaises(ValidationError):
            validate_response(_ok_response(protocol_version=0))

    def test_not_object(self):
        with self.assertRaises(ValidationError) as cm:
            validate_response("not an object")
        self.assertEqual(cm.exception.code, "not_object")

    def test_success_wrong_type(self):
        with self.assertRaises(ValidationError) as cm:
            validate_response(_ok_response(success="yes"))
        self.assertEqual(cm.exception.code, "bad_success_type")

    def test_operations_not_array(self):
        with self.assertRaises(ValidationError) as cm:
            validate_response(_ok_response(operations={"type": "PLACE_COMPONENT"}))
        self.assertEqual(cm.exception.code, "ops_not_array")

    def test_plan_steps_validate(self):
        v = validate_response(_ok_response(plan={
            "steps": [{"index": 0, "description": "step 0"}],
            "placement_info": "",
        }))
        self.assertEqual(len(v.plan_steps), 1)

    def test_plan_step_index_must_be_int(self):
        with self.assertRaises(ValidationError) as cm:
            validate_response(_ok_response(plan={
                "steps": [{"index": "0", "description": "x"}],
                "placement_info": "",
            }))
        self.assertEqual(cm.exception.code, "plan_step_bad_index")

    def test_extra_unknown_field_tolerated(self):
        # Forward-compatibility — unknown fields don't reject.
        v = validate_response({**_ok_response(), "future_field": 42})
        self.assertTrue(v.success)


class PlaceComponentTest(unittest.TestCase):
    def _op(self, **data_overrides):
        data = {
            "lib_id": "L:S", "reference": "U1", "value": "x",
            "x": 0, "y": 0, "rotation": 0.0,
        }
        data.update(data_overrides)
        return {"type": "PLACE_COMPONENT", "data": data}

    def test_ok(self):
        v = validate_operation(self._op(), _seen_refs=[])
        self.assertEqual(v.type, "PLACE_COMPONENT")

    def test_missing_lib_id(self):
        with self.assertRaises(ValidationError) as cm:
            validate_operation(self._op(lib_id=""), _seen_refs=[])
        self.assertEqual(cm.exception.code, "place_no_lib_id")

    def test_bad_lib_id_no_colon(self):
        with self.assertRaises(ValidationError) as cm:
            validate_operation(self._op(lib_id="nocolon"), _seen_refs=[])
        self.assertEqual(cm.exception.code, "place_bad_lib_id")

    def test_missing_ref(self):
        with self.assertRaises(ValidationError) as cm:
            validate_operation(self._op(reference=""), _seen_refs=[])
        self.assertEqual(cm.exception.code, "place_no_ref")

    def test_duplicate_ref(self):
        seen = []
        validate_operation(self._op(reference="U1"), _seen_refs=seen)
        with self.assertRaises(ValidationError) as cm:
            validate_operation(self._op(reference="U1", x=1000), _seen_refs=seen)
        self.assertEqual(cm.exception.code, "place_dup_ref")

    def test_coord_too_big(self):
        with self.assertRaises(ValidationError) as cm:
            validate_operation(self._op(x=2_000_000_000), _seen_refs=[])
        self.assertEqual(cm.exception.code, "coord_out_of_range")

    def test_coord_wrong_type(self):
        with self.assertRaises(ValidationError) as cm:
            validate_operation(self._op(x=1.5), _seen_refs=[])
        self.assertEqual(cm.exception.code, "bad_coord_type")

    def test_coord_bool_rejected(self):
        with self.assertRaises(ValidationError):
            validate_operation(self._op(x=True), _seen_refs=[])

    def test_bad_rotation(self):
        with self.assertRaises(ValidationError) as cm:
            validate_operation(self._op(rotation=45.0), _seen_refs=[])
        self.assertEqual(cm.exception.code, "place_bad_rot_value")

    def test_rotation_int_accepted(self):
        v = validate_operation(self._op(rotation=90), _seen_refs=[])
        self.assertEqual(v.type, "PLACE_COMPONENT")

    def test_rotation_bool_rejected(self):
        with self.assertRaises(ValidationError):
            validate_operation(self._op(rotation=True), _seen_refs=[])


class WireTest(unittest.TestCase):
    def test_ok(self):
        validate_operation(
            {"type": "ADD_WIRE", "data": {"start_x": 0, "start_y": 0, "end_x": 1000, "end_y": 0}}
        )

    def test_zero_length(self):
        with self.assertRaises(ValidationError) as cm:
            validate_operation(
                {"type": "ADD_WIRE", "data": {"start_x": 0, "start_y": 0, "end_x": 0, "end_y": 0}}
            )
        self.assertEqual(cm.exception.code, "wire_zero_length")


class LabelTest(unittest.TestCase):
    def test_ok_local(self):
        validate_operation({"type": "ADD_LABEL", "data": {
            "name": "X", "x": 0, "y": 0, "label_type": "local"
        }})

    def test_ok_global(self):
        validate_operation({"type": "ADD_LABEL", "data": {
            "name": "X", "x": 0, "y": 0, "label_type": "global"
        }})

    def test_empty_name(self):
        with self.assertRaises(ValidationError) as cm:
            validate_operation({"type": "ADD_LABEL", "data": {
                "name": "", "x": 0, "y": 0, "label_type": "local"
            }})
        self.assertEqual(cm.exception.code, "label_no_name")

    def test_long_name(self):
        with self.assertRaises(ValidationError) as cm:
            validate_operation({"type": "ADD_LABEL", "data": {
                "name": "x" * 65, "x": 0, "y": 0, "label_type": "local"
            }})
        self.assertEqual(cm.exception.code, "label_long_name")

    def test_bad_label_type(self):
        with self.assertRaises(ValidationError) as cm:
            validate_operation({"type": "ADD_LABEL", "data": {
                "name": "X", "x": 0, "y": 0, "label_type": "weird"
            }})
        self.assertEqual(cm.exception.code, "label_bad_type")


class PowerTest(unittest.TestCase):
    def test_ok(self):
        validate_operation({"type": "ADD_POWER_SYMBOL", "data": {
            "net_name": "VCC", "x": 0, "y": 0
        }})

    def test_empty_net(self):
        with self.assertRaises(ValidationError) as cm:
            validate_operation({"type": "ADD_POWER_SYMBOL", "data": {
                "net_name": "", "x": 0, "y": 0
            }})
        self.assertEqual(cm.exception.code, "power_no_net")


class OpKindTest(unittest.TestCase):
    def test_unknown_type(self):
        with self.assertRaises(ValidationError) as cm:
            validate_operation({"type": "FLY_SHIP", "data": {}})
        self.assertEqual(cm.exception.code, "op_unknown_type")

    def test_no_type(self):
        with self.assertRaises(ValidationError) as cm:
            validate_operation({"data": {}})
        self.assertEqual(cm.exception.code, "op_no_type")

    def test_not_object(self):
        with self.assertRaises(ValidationError) as cm:
            validate_operation([])
        self.assertEqual(cm.exception.code, "op_not_object")


class GeometryHelperTest(unittest.TestCase):
    def test_no_overlap(self):
        from copper_integration.validators import check_no_symbol_overlap
        out = check_no_symbol_overlap([("U1", 0, 0), ("U2", 1, 0)])
        self.assertEqual(out, [])

    def test_overlap(self):
        from copper_integration.validators import check_no_symbol_overlap
        out = check_no_symbol_overlap([("U1", 0, 0), ("U2", 0, 0)])
        self.assertEqual(out, [("U1", "U2")])


if __name__ == "__main__":  # pragma: no cover
    unittest.main()
