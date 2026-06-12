"""
SchematicApi seam — the mutation surface the apply layer talks to.

Two implementations:
    - SchematicApi: abstract base (matches the contract the C++ side honors
      via SCH_COMMIT + SCH_SYMBOL/SCH_LINE/SCH_LABEL/SCH_JUNCTION).
    - FakeSchematicApi: in-memory implementation for headless tests.

The fake mirrors KiCad's SCH_COMMIT semantics:
    - begin_commit() starts a batch.
    - Per-op mutations are buffered inside the batch.
    - push_commit() atomically applies the batch + records an undo entry.
    - abort_commit() discards the batch with zero side effects.
    - undo() reverses the last pushed commit atomically.

Invariant tested by check 6 (atomic rollback):
    serialize() byte-equal before/after any abort_commit or any failed
    apply where push_commit was never called.

See docs/ARCHITECTURE.md §6a and docs/PROTOCOL.md for the op data shapes.
"""

from __future__ import annotations

import copy
import hashlib
import itertools
import json
import threading
from abc import ABC, abstractmethod
from dataclasses import dataclass, field, asdict
from typing import Any, Dict, List, Optional


COORD_MIN = -1_000_000_000  # -1e9 nm — see PROTOCOL.md §coordinates
COORD_MAX = 1_000_000_000


# ── exceptions ─────────────────────────────────────────────────────────────

class SchematicError(Exception):
    """Raised when the schematic API receives invalid input or sees a state
    violation. Distinct from ValidationError (which is the pre-apply check)."""


# ── value types ────────────────────────────────────────────────────────────

@dataclass(frozen=True)
class Symbol:
    id: str
    lib_id: str
    reference: str
    value: str
    x: int
    y: int
    rotation: float = 0.0
    footprint: str = ""  # KiCad footprint id 'Lib:Name', or "" (e.g. power symbols)
    is_power: bool = False  # ADD_POWER_SYMBOL sets True

    def to_dict(self) -> Dict[str, Any]:
        return asdict(self)


@dataclass(frozen=True)
class Wire:
    id: str
    start_x: int
    start_y: int
    end_x: int
    end_y: int

    def to_dict(self) -> Dict[str, Any]:
        return asdict(self)


@dataclass(frozen=True)
class Label:
    id: str
    name: str
    x: int
    y: int
    label_type: str  # 'local' | 'global' | 'hierarchical'

    def to_dict(self) -> Dict[str, Any]:
        return asdict(self)


@dataclass(frozen=True)
class Junction:
    id: str
    x: int
    y: int

    def to_dict(self) -> Dict[str, Any]:
        return asdict(self)


# ── commit token ───────────────────────────────────────────────────────────

@dataclass
class CommitToken:
    """Opaque-ish handle returned by begin_commit(). Tests inspect .label
    and .id for debugging; production code treats it as opaque."""
    id: int
    label: str
    closed: bool = False  # True once push or abort completed


# ── abstract interface ────────────────────────────────────────────────────

class SchematicApi(ABC):
    """The mutation surface used by ApplyEngine.

    Concrete impls (C++ in production, FakeSchematicApi in tests) honor
    the same atomic-commit semantics: nothing is visible until push_commit;
    abort_commit is byte-equivalent to never having opened the commit.
    """

    @abstractmethod
    def begin_commit(self, label: str) -> CommitToken: ...

    @abstractmethod
    def abort_commit(self, token: CommitToken) -> None: ...

    @abstractmethod
    def push_commit(self, token: CommitToken) -> str:
        """Returns the commit id (a stable string for testing)."""

    @abstractmethod
    def undo(self) -> Optional[str]:
        """Reverse the last pushed commit. Returns its id, or None if there
        is nothing to undo. After undo(), redo() may bring it back (impl-
        dependent)."""

    @abstractmethod
    def redo(self) -> Optional[str]: ...

    # within-commit operations — must be inside an open commit
    @abstractmethod
    def place_component(self, token: CommitToken, data: Dict[str, Any]) -> str: ...

    @abstractmethod
    def add_wire(self, token: CommitToken, data: Dict[str, Any]) -> str: ...

    @abstractmethod
    def add_label(self, token: CommitToken, data: Dict[str, Any]) -> str: ...

    @abstractmethod
    def add_junction(self, token: CommitToken, data: Dict[str, Any]) -> str: ...

    @abstractmethod
    def add_power_symbol(self, token: CommitToken, data: Dict[str, Any]) -> str: ...

    # read-only queries
    @abstractmethod
    def list_symbols(self) -> List[Symbol]: ...

    @abstractmethod
    def list_wires(self) -> List[Wire]: ...

    @abstractmethod
    def list_labels(self) -> List[Label]: ...

    @abstractmethod
    def list_junctions(self) -> List[Junction]: ...

    @abstractmethod
    def serialize(self) -> bytes:
        """Deterministic byte serialization of the committed state — used by
        rollback tests for byte-equality assertions. Excludes the undo
        stack on purpose: only the visible schematic matters."""


# ── in-memory fake (the rest of the harness depends on this) ──────────────

class FakeSchematicApi(SchematicApi):
    """In-memory SchematicApi for headless tests.

    Concurrency: a single lock guards all state. The C++ SCH_COMMIT runs
    on the wx main thread so it's effectively serialized; the fake matches.

    Library resolution: the fake doesn't load real symbols, so it accepts
    any lib_id the validator allowed. If you want to test "symbol not in
    library" failures, register the missing lib_id via register_missing_lib_id().
    """

    # Optional failure injection (test-only):
    # set fail_on_op_kind to e.g. 'place_component' and any call to that
    # method raises SchematicError. Used by the atomic-rollback check (6).
    def __init__(self) -> None:
        self._lock = threading.RLock()
        self._next_id = itertools.count(1)
        self._next_commit_id = itertools.count(1)

        # The committed state.
        self._symbols: Dict[str, Symbol] = {}
        self._wires: Dict[str, Wire] = {}
        self._labels: Dict[str, Label] = {}
        self._junctions: Dict[str, Junction] = {}

        # Pending batch per open commit.
        # token.id -> dict with the four item-kind lists.
        self._pending: Dict[int, Dict[str, list]] = {}

        # Undo / redo stacks of recorded commits.
        # Each entry: dict with label, id, additions per kind.
        self._undo: List[Dict[str, Any]] = []
        self._redo: List[Dict[str, Any]] = []

        # Test hooks
        self._missing_lib_ids: set = set()
        self.fail_on_op_kind: Optional[str] = None
        self.fail_after_n_in_commit: Optional[int] = None
        self._op_count_in_commit: Dict[int, int] = {}

    # ── test hooks ──

    def register_missing_lib_id(self, lib_id: str) -> None:
        """Tell the fake to reject this lib_id with SchematicError, modelling
        the C++ GetLibSymbol() returning nullptr."""
        self._missing_lib_ids.add(lib_id)

    def inject_failure(
        self,
        op_kind: Optional[str] = None,
        after_n_in_commit: Optional[int] = None,
    ) -> None:
        """Configure the next commit to fail. op_kind is one of
        'place_component', 'add_wire', 'add_label', 'add_junction',
        'add_power_symbol'. If after_n_in_commit is set, the failure fires
        on the (after_n_in_commit + 1)-th call of any kind inside the commit."""
        self.fail_on_op_kind = op_kind
        self.fail_after_n_in_commit = after_n_in_commit

    # ── commit lifecycle ──

    def begin_commit(self, label: str) -> CommitToken:
        if not isinstance(label, str) or not label.strip():
            raise SchematicError("commit label must be a non-empty string")

        with self._lock:
            tid = next(self._next_commit_id)
            self._pending[tid] = {
                "symbols": [],
                "wires": [],
                "labels": [],
                "junctions": [],
            }
            self._op_count_in_commit[tid] = 0
            return CommitToken(id=tid, label=label)

    def abort_commit(self, token: CommitToken) -> None:
        with self._lock:
            if token.closed:
                raise SchematicError("commit already closed")
            self._pending.pop(token.id, None)
            self._op_count_in_commit.pop(token.id, None)
            token.closed = True

    def push_commit(self, token: CommitToken) -> str:
        with self._lock:
            if token.closed:
                raise SchematicError("commit already closed")
            batch = self._pending.pop(token.id, None)
            if batch is None:
                raise SchematicError(f"no pending batch for commit {token.id}")

            self._op_count_in_commit.pop(token.id, None)

            cid = f"c{token.id}"

            # Atomic apply: all-or-nothing relative to the committed state.
            for s in batch["symbols"]:
                self._symbols[s.id] = s
            for w in batch["wires"]:
                self._wires[w.id] = w
            for la in batch["labels"]:
                self._labels[la.id] = la
            for j in batch["junctions"]:
                self._junctions[j.id] = j

            # Track for undo. Clear redo (standard undo-stack semantics).
            self._undo.append({"label": token.label, "id": cid, **batch})
            self._redo.clear()

            token.closed = True
            return cid

    def undo(self) -> Optional[str]:
        with self._lock:
            if not self._undo:
                return None
            entry = self._undo.pop()
            for s in entry["symbols"]:
                self._symbols.pop(s.id, None)
            for w in entry["wires"]:
                self._wires.pop(w.id, None)
            for la in entry["labels"]:
                self._labels.pop(la.id, None)
            for j in entry["junctions"]:
                self._junctions.pop(j.id, None)
            self._redo.append(entry)
            return entry["id"]

    def redo(self) -> Optional[str]:
        with self._lock:
            if not self._redo:
                return None
            entry = self._redo.pop()
            for s in entry["symbols"]:
                self._symbols[s.id] = s
            for w in entry["wires"]:
                self._wires[w.id] = w
            for la in entry["labels"]:
                self._labels[la.id] = la
            for j in entry["junctions"]:
                self._junctions[j.id] = j
            self._undo.append(entry)
            return entry["id"]

    # ── operations ──

    def _check_token(self, token: CommitToken) -> Dict[str, list]:
        if token.closed:
            raise SchematicError("commit already closed")
        batch = self._pending.get(token.id)
        if batch is None:
            raise SchematicError(f"unknown commit token {token.id}")
        return batch

    def _maybe_fail(self, token: CommitToken, kind: str) -> None:
        n = self._op_count_in_commit.get(token.id, 0)
        if (
            self.fail_after_n_in_commit is not None
            and n >= self.fail_after_n_in_commit
        ):
            raise SchematicError(
                f"injected failure after {self.fail_after_n_in_commit} ops"
            )
        if self.fail_on_op_kind == kind:
            raise SchematicError(f"injected failure on {kind}")

    def _bump(self, token: CommitToken) -> None:
        self._op_count_in_commit[token.id] = (
            self._op_count_in_commit.get(token.id, 0) + 1
        )

    def _check_coords(self, *coords: int) -> None:
        for c in coords:
            if not isinstance(c, int) or isinstance(c, bool):
                raise SchematicError(f"coordinate must be int, got {type(c).__name__}")
            if c < COORD_MIN or c > COORD_MAX:
                raise SchematicError(
                    f"coordinate {c} out of range [{COORD_MIN}, {COORD_MAX}]"
                )

    def place_component(self, token: CommitToken, data: Dict[str, Any]) -> str:
        with self._lock:
            batch = self._check_token(token)
            self._maybe_fail(token, "place_component")

            lib_id = data.get("lib_id", "")
            if not lib_id:
                raise SchematicError("PLACE_COMPONENT: missing lib_id")
            if lib_id in self._missing_lib_ids:
                raise SchematicError(f"PLACE_COMPONENT: symbol not in library: {lib_id}")

            ref = data.get("reference", "")
            if not ref:
                raise SchematicError("PLACE_COMPONENT: missing reference")

            # Uniqueness across both committed state and the open commit's batch.
            existing_refs = {s.reference for s in self._symbols.values()}
            existing_refs |= {s.reference for s in batch["symbols"]}
            if ref in existing_refs:
                raise SchematicError(f"PLACE_COMPONENT: duplicate reference {ref}")

            x = data.get("x", 0)
            y = data.get("y", 0)
            self._check_coords(x, y)

            rotation = float(data.get("rotation", 0.0))
            if rotation not in (0.0, 90.0, 180.0, 270.0):
                raise SchematicError(f"PLACE_COMPONENT: invalid rotation {rotation}")

            # Optional, additive: missing means "" (no footprint assigned).
            footprint = data.get("footprint", "")
            if not isinstance(footprint, str):
                raise SchematicError(
                    f"PLACE_COMPONENT: footprint must be str, "
                    f"got {type(footprint).__name__}"
                )
            if footprint and ":" not in footprint:
                raise SchematicError(
                    f"PLACE_COMPONENT: footprint must be 'Lib:Name' "
                    f"(or empty), got {footprint!r}"
                )

            sym = Symbol(
                id=f"S{next(self._next_id)}",
                lib_id=lib_id,
                reference=ref,
                value=data.get("value", ref),
                x=x,
                y=y,
                rotation=rotation,
                footprint=footprint,
                is_power=False,
            )
            batch["symbols"].append(sym)
            self._bump(token)
            return sym.id

    def add_wire(self, token: CommitToken, data: Dict[str, Any]) -> str:
        with self._lock:
            batch = self._check_token(token)
            self._maybe_fail(token, "add_wire")

            sx, sy = data.get("start_x", 0), data.get("start_y", 0)
            ex, ey = data.get("end_x", 0), data.get("end_y", 0)
            self._check_coords(sx, sy, ex, ey)
            if (sx, sy) == (ex, ey):
                raise SchematicError("ADD_WIRE: zero-length wire")

            wire = Wire(
                id=f"W{next(self._next_id)}",
                start_x=sx, start_y=sy, end_x=ex, end_y=ey,
            )
            batch["wires"].append(wire)
            self._bump(token)
            return wire.id

    def add_label(self, token: CommitToken, data: Dict[str, Any]) -> str:
        with self._lock:
            batch = self._check_token(token)
            self._maybe_fail(token, "add_label")

            name = data.get("name", "")
            if not name:
                raise SchematicError("ADD_LABEL: missing name")
            if len(name) > 64:
                raise SchematicError("ADD_LABEL: name longer than 64 chars")

            x, y = data.get("x", 0), data.get("y", 0)
            self._check_coords(x, y)

            kind = data.get("label_type", "local")
            if kind not in ("local", "global", "hierarchical"):
                raise SchematicError(f"ADD_LABEL: bad label_type {kind!r}")

            la = Label(
                id=f"L{next(self._next_id)}",
                name=name, x=x, y=y, label_type=kind,
            )
            batch["labels"].append(la)
            self._bump(token)
            return la.id

    def add_junction(self, token: CommitToken, data: Dict[str, Any]) -> str:
        with self._lock:
            batch = self._check_token(token)
            self._maybe_fail(token, "add_junction")

            x, y = data.get("x", 0), data.get("y", 0)
            self._check_coords(x, y)

            j = Junction(id=f"J{next(self._next_id)}", x=x, y=y)
            batch["junctions"].append(j)
            self._bump(token)
            return j.id

    def add_power_symbol(self, token: CommitToken, data: Dict[str, Any]) -> str:
        with self._lock:
            batch = self._check_token(token)
            self._maybe_fail(token, "add_power_symbol")

            net = data.get("net_name", "")
            if not net:
                raise SchematicError("ADD_POWER_SYMBOL: empty net_name")

            x, y = data.get("x", 0), data.get("y", 0)
            self._check_coords(x, y)

            lib_id = f"power:{net}"
            if lib_id in self._missing_lib_ids:
                raise SchematicError(
                    f"ADD_POWER_SYMBOL: symbol not in library: {lib_id}"
                )

            ref = f"#PWR{next(self._next_id):04d}"
            sym = Symbol(
                id=f"S{next(self._next_id)}",
                lib_id=lib_id,
                reference=ref,
                value=net,
                x=x, y=y,
                rotation=0.0,
                is_power=True,
            )
            batch["symbols"].append(sym)
            self._bump(token)
            return sym.id

    # ── queries ──

    def list_symbols(self) -> List[Symbol]:
        with self._lock:
            return sorted(self._symbols.values(), key=lambda s: s.id)

    def list_wires(self) -> List[Wire]:
        with self._lock:
            return sorted(self._wires.values(), key=lambda w: w.id)

    def list_labels(self) -> List[Label]:
        with self._lock:
            return sorted(self._labels.values(), key=lambda la: la.id)

    def list_junctions(self) -> List[Junction]:
        with self._lock:
            return sorted(self._junctions.values(), key=lambda j: j.id)

    def serialize(self) -> bytes:
        """Deterministic JSON over (symbols, wires, labels, junctions),
        sorted by id. Excludes the undo stack — only the visible state
        matters for rollback tests."""
        with self._lock:
            state = {
                "symbols": [s.to_dict() for s in self.list_symbols()],
                "wires": [w.to_dict() for w in self.list_wires()],
                "labels": [la.to_dict() for la in self.list_labels()],
                "junctions": [j.to_dict() for j in self.list_junctions()],
            }
            return json.dumps(state, sort_keys=True, separators=(",", ":")).encode(
                "utf-8"
            )

    def snapshot_hash(self) -> str:
        return hashlib.sha256(self.serialize()).hexdigest()

    # ── debug helpers (not part of the abstract surface) ──

    def open_commit_count(self) -> int:
        with self._lock:
            return len(self._pending)

    def committed_commit_count(self) -> int:
        with self._lock:
            return len(self._undo)
