"""Tests for FakeSchematicApi — the headless mutation surface.

Covers:
- commit lifecycle (begin/abort/push)
- byte-equal abort
- atomic apply (all-or-nothing)
- undo / redo
- per-op error rejection
- failure injection (used by check 6)
"""

from __future__ import annotations

import unittest

from copper_integration.schematic_api import (
    FakeSchematicApi,
    SchematicError,
    CommitToken,
)


def _plan_data():
    """Some canonical op data for use across tests."""
    return {
        "rp2040": {
            "lib_id": "MCU_RaspberryPi:RP2040",
            "reference": "U1",
            "value": "RP2040",
            "x": 12_700_000, "y": 12_700_000,
            "rotation": 0.0,
        },
        "imu": {
            "lib_id": "Sensor:LSM6DSO",
            "reference": "U2",
            "value": "LSM6DSO",
            "x": 25_400_000, "y": 12_700_000,
        },
        "wire": {
            "start_x": 0, "start_y": 0, "end_x": 2_540_000, "end_y": 0,
        },
        "label": {
            "name": "SDA", "x": 0, "y": 0, "label_type": "local",
        },
        "global_label": {
            "name": "VBUS", "x": 0, "y": 0, "label_type": "global",
        },
        "junction": {"x": 2_540_000, "y": 0},
        "power_vcc": {"net_name": "VCC", "x": 0, "y": -2_540_000},
    }


class CommitLifecycleTest(unittest.TestCase):
    def test_begin_returns_open_token(self):
        api = FakeSchematicApi()
        tok = api.begin_commit("test")
        self.assertIsInstance(tok, CommitToken)
        self.assertFalse(tok.closed)
        self.assertEqual(tok.label, "test")
        self.assertEqual(api.open_commit_count(), 1)

    def test_push_closes_token_and_records_undo(self):
        api = FakeSchematicApi()
        tok = api.begin_commit("t")
        api.place_component(tok, _plan_data()["rp2040"])
        cid = api.push_commit(tok)
        self.assertTrue(tok.closed)
        self.assertEqual(api.open_commit_count(), 0)
        self.assertEqual(api.committed_commit_count(), 1)
        self.assertTrue(cid)

    def test_abort_closes_token_and_drops_state(self):
        api = FakeSchematicApi()
        before = api.snapshot_hash()
        tok = api.begin_commit("t")
        api.place_component(tok, _plan_data()["rp2040"])
        api.abort_commit(tok)
        self.assertTrue(tok.closed)
        self.assertEqual(api.snapshot_hash(), before)
        self.assertEqual(api.list_symbols(), [])

    def test_empty_label_rejected(self):
        api = FakeSchematicApi()
        with self.assertRaises(SchematicError):
            api.begin_commit("")

    def test_op_after_close_rejected(self):
        api = FakeSchematicApi()
        tok = api.begin_commit("t")
        api.push_commit(tok)
        with self.assertRaises(SchematicError):
            api.place_component(tok, _plan_data()["rp2040"])

    def test_double_close_rejected(self):
        api = FakeSchematicApi()
        tok = api.begin_commit("t")
        api.push_commit(tok)
        with self.assertRaises(SchematicError):
            api.push_commit(tok)
        with self.assertRaises(SchematicError):
            api.abort_commit(tok)


class AtomicApplyTest(unittest.TestCase):
    def test_within_commit_state_not_visible(self):
        """Items inside an open commit must not appear in list_symbols()."""
        api = FakeSchematicApi()
        tok = api.begin_commit("t")
        api.place_component(tok, _plan_data()["rp2040"])
        self.assertEqual(api.list_symbols(), [])
        api.push_commit(tok)
        self.assertEqual(len(api.list_symbols()), 1)

    def test_push_applies_all_kinds(self):
        api = FakeSchematicApi()
        d = _plan_data()
        tok = api.begin_commit("t")
        api.place_component(tok, d["rp2040"])
        api.add_wire(tok, d["wire"])
        api.add_label(tok, d["label"])
        api.add_junction(tok, d["junction"])
        api.add_power_symbol(tok, d["power_vcc"])
        api.push_commit(tok)
        self.assertEqual(len(api.list_symbols()), 2)  # rp2040 + power
        self.assertEqual(len(api.list_wires()), 1)
        self.assertEqual(len(api.list_labels()), 1)
        self.assertEqual(len(api.list_junctions()), 1)


class ByteEqualAbortTest(unittest.TestCase):
    """Check 6 (atomic rollback) starts here."""

    def test_abort_after_many_ops_byte_equal_to_pre_begin(self):
        api = FakeSchematicApi()
        d = _plan_data()

        # First, push one good commit so the snapshot is non-trivial.
        tok = api.begin_commit("setup")
        api.place_component(tok, d["rp2040"])
        api.push_commit(tok)
        before = api.snapshot_hash()

        # Now open a fat commit and abort it.
        tok2 = api.begin_commit("t2")
        api.place_component(tok2, d["imu"])
        api.add_wire(tok2, d["wire"])
        api.add_label(tok2, d["label"])
        api.add_junction(tok2, d["junction"])
        api.add_power_symbol(tok2, d["power_vcc"])
        api.abort_commit(tok2)

        self.assertEqual(api.snapshot_hash(), before)


class InjectedFailureTest(unittest.TestCase):
    def test_injected_failure_on_specific_kind(self):
        api = FakeSchematicApi()
        d = _plan_data()
        api.inject_failure(op_kind="add_wire")
        tok = api.begin_commit("t")
        api.place_component(tok, d["rp2040"])  # ok
        with self.assertRaises(SchematicError):
            api.add_wire(tok, d["wire"])
        # The caller should abort; until they do, the symbol is still in the
        # batch but not committed.
        self.assertEqual(api.list_symbols(), [])

    def test_injected_failure_after_n_ops(self):
        api = FakeSchematicApi()
        d = _plan_data()
        api.inject_failure(after_n_in_commit=2)
        tok = api.begin_commit("t")
        api.place_component(tok, d["rp2040"])
        api.place_component(tok, d["imu"])
        with self.assertRaises(SchematicError):
            api.add_wire(tok, d["wire"])


class UndoRedoTest(unittest.TestCase):
    def test_undo_reverses_one_push(self):
        api = FakeSchematicApi()
        d = _plan_data()
        tok = api.begin_commit("t")
        api.place_component(tok, d["rp2040"])
        api.add_wire(tok, d["wire"])
        api.push_commit(tok)

        cid = api.undo()
        self.assertIsNotNone(cid)
        self.assertEqual(api.list_symbols(), [])
        self.assertEqual(api.list_wires(), [])

    def test_undo_returns_none_on_empty(self):
        api = FakeSchematicApi()
        self.assertIsNone(api.undo())

    def test_redo_restores(self):
        api = FakeSchematicApi()
        d = _plan_data()
        tok = api.begin_commit("t")
        api.place_component(tok, d["rp2040"])
        api.push_commit(tok)

        api.undo()
        self.assertEqual(api.list_symbols(), [])
        api.redo()
        self.assertEqual(len(api.list_symbols()), 1)

    def test_push_clears_redo(self):
        api = FakeSchematicApi()
        d = _plan_data()
        tok = api.begin_commit("t1")
        api.place_component(tok, d["rp2040"])
        api.push_commit(tok)
        api.undo()  # now redo stack has one entry

        tok2 = api.begin_commit("t2")
        api.place_component(tok2, d["imu"])
        api.push_commit(tok2)

        # Redo should now be empty — standard undo-stack semantics.
        self.assertIsNone(api.redo())

    def test_undo_one_commit_undoes_full_plan(self):
        """Check 6 part 2: one undo() reverses the whole apply, no matter
        how many ops it contained."""
        api = FakeSchematicApi()
        d = _plan_data()
        before = api.snapshot_hash()

        tok = api.begin_commit("Copper AI: Execute plan")
        api.place_component(tok, d["rp2040"])
        api.place_component(tok, d["imu"])
        api.add_wire(tok, d["wire"])
        api.add_label(tok, d["label"])
        api.add_label(tok, d["global_label"])
        api.add_junction(tok, d["junction"])
        api.add_power_symbol(tok, d["power_vcc"])
        api.push_commit(tok)
        self.assertNotEqual(api.snapshot_hash(), before)

        api.undo()
        self.assertEqual(api.snapshot_hash(), before)


class OpRejectionTest(unittest.TestCase):
    def test_place_missing_lib_id(self):
        api = FakeSchematicApi()
        tok = api.begin_commit("t")
        with self.assertRaises(SchematicError):
            api.place_component(tok, {"reference": "U1", "value": "x", "x": 0, "y": 0})

    def test_place_missing_ref(self):
        api = FakeSchematicApi()
        tok = api.begin_commit("t")
        with self.assertRaises(SchematicError):
            api.place_component(tok, {"lib_id": "lib:s", "value": "x", "x": 0, "y": 0})

    def test_place_duplicate_ref_within_commit(self):
        api = FakeSchematicApi()
        d = _plan_data()
        tok = api.begin_commit("t")
        api.place_component(tok, d["rp2040"])
        bad = dict(d["imu"])
        bad["reference"] = d["rp2040"]["reference"]  # same as already placed
        with self.assertRaises(SchematicError):
            api.place_component(tok, bad)

    def test_place_duplicate_ref_across_commits(self):
        api = FakeSchematicApi()
        d = _plan_data()
        tok = api.begin_commit("t1")
        api.place_component(tok, d["rp2040"])
        api.push_commit(tok)
        tok2 = api.begin_commit("t2")
        bad = dict(d["imu"])
        bad["reference"] = d["rp2040"]["reference"]
        with self.assertRaises(SchematicError):
            api.place_component(tok2, bad)

    def test_place_missing_lib_symbol(self):
        api = FakeSchematicApi()
        api.register_missing_lib_id("Bad:Missing")
        tok = api.begin_commit("t")
        with self.assertRaises(SchematicError):
            api.place_component(tok, {
                "lib_id": "Bad:Missing", "reference": "U9",
                "value": "x", "x": 0, "y": 0,
            })

    def test_place_bad_rotation(self):
        api = FakeSchematicApi()
        tok = api.begin_commit("t")
        with self.assertRaises(SchematicError):
            api.place_component(tok, {
                "lib_id": "L:S", "reference": "U1", "value": "x",
                "x": 0, "y": 0, "rotation": 45.0,
            })

    def test_coord_out_of_range(self):
        api = FakeSchematicApi()
        tok = api.begin_commit("t")
        with self.assertRaises(SchematicError):
            api.add_junction(tok, {"x": 2_000_000_000, "y": 0})

    def test_coord_not_int(self):
        api = FakeSchematicApi()
        tok = api.begin_commit("t")
        with self.assertRaises(SchematicError):
            api.add_junction(tok, {"x": 1.5, "y": 0})

    def test_zero_length_wire(self):
        api = FakeSchematicApi()
        tok = api.begin_commit("t")
        with self.assertRaises(SchematicError):
            api.add_wire(tok, {"start_x": 0, "start_y": 0, "end_x": 0, "end_y": 0})

    def test_bad_label_type(self):
        api = FakeSchematicApi()
        tok = api.begin_commit("t")
        with self.assertRaises(SchematicError):
            api.add_label(tok, {
                "name": "X", "x": 0, "y": 0, "label_type": "weird",
            })

    def test_power_missing_net(self):
        api = FakeSchematicApi()
        tok = api.begin_commit("t")
        with self.assertRaises(SchematicError):
            api.add_power_symbol(tok, {"net_name": "", "x": 0, "y": 0})


class SerializeTest(unittest.TestCase):
    def test_within_instance_stable(self):
        """Calling serialize() twice without intervening mutation is byte-equal."""
        api = FakeSchematicApi()
        d = _plan_data()
        tok = api.begin_commit("t")
        api.place_component(tok, d["rp2040"])
        api.add_wire(tok, d["wire"])
        api.push_commit(tok)
        first = api.serialize()
        second = api.serialize()
        self.assertEqual(first, second)
        self.assertEqual(api.snapshot_hash(), api.snapshot_hash())

    def test_serialize_changes_after_mutation(self):
        api = FakeSchematicApi()
        before = api.serialize()
        tok = api.begin_commit("t")
        api.place_component(tok, _plan_data()["rp2040"])
        api.push_commit(tok)
        self.assertNotEqual(api.serialize(), before)

    def test_serialize_unchanged_during_open_commit(self):
        """Within an unfinished commit, the *committed* state is untouched."""
        api = FakeSchematicApi()
        before = api.serialize()
        tok = api.begin_commit("t")
        api.place_component(tok, _plan_data()["rp2040"])
        # not yet pushed
        self.assertEqual(api.serialize(), before)


if __name__ == "__main__":  # pragma: no cover — manual runner
    unittest.main()
