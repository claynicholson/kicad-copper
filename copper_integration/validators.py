"""
Response + operation validation. Implements the "Hard rejects" list from
docs/PROTOCOL.md.

The C++ side does the same checks in CopperResponse::fromJson +
COPPER_CHAT_PANEL::ExecuteOperations (after M3). This module is the
canonical reference — both sides must stay in sync.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Dict, List, Optional, Tuple

PROTOCOL_VERSION = 1
COORD_MIN = -1_000_000_000
COORD_MAX = 1_000_000_000

KNOWN_OP_TYPES = (
    "PLACE_COMPONENT",
    "ADD_WIRE",
    "ADD_LABEL",
    "ADD_JUNCTION",
    "ADD_POWER_SYMBOL",
)

KNOWN_LABEL_TYPES = ("local", "global", "hierarchical")
KNOWN_ROTATIONS = (0.0, 90.0, 180.0, 270.0)


class ValidationError(Exception):
    """Raised when a backend response or operation fails PROTOCOL.md checks.

    Carries .path to point at the offending field, and .code for
    machine-friendly identifying.
    """

    def __init__(self, message: str, *, path: str = "", code: str = ""):
        super().__init__(message)
        self.path = path
        self.code = code

    def __repr__(self) -> str:  # pragma: no cover — debug aid
        return f"ValidationError(code={self.code!r}, path={self.path!r}, msg={self.args[0]!r})"


@dataclass
class ValidatedOperation:
    type: str
    data: Dict[str, Any]


@dataclass
class ValidatedResponse:
    protocol_version: int
    success: bool
    intent: str
    message: str
    operations: List[ValidatedOperation]
    plan_steps: List[Dict[str, Any]]
    placement_info: str
    error: str
    erc: Optional[Dict[str, Any]]
    raw: Dict[str, Any]  # the original parsed dict, for diagnostics


# ── public entrypoints ─────────────────────────────────────────────────────


def validate_response(obj: Any) -> ValidatedResponse:
    """Validate a CopperResponse-shaped dict. Returns a ValidatedResponse on
    success; raises ValidationError on the first failure."""

    if not isinstance(obj, dict):
        raise ValidationError(
            f"response must be a JSON object, got {type(obj).__name__}",
            code="not_object",
        )

    pv = obj.get("protocol_version")
    if pv is None:
        raise ValidationError(
            "missing protocol_version", path="$.protocol_version", code="no_version",
        )
    if not isinstance(pv, int) or isinstance(pv, bool):
        raise ValidationError(
            f"protocol_version must be int, got {type(pv).__name__}",
            path="$.protocol_version", code="bad_version_type",
        )
    if pv > PROTOCOL_VERSION:
        raise ValidationError(
            f"unsupported protocol_version {pv} (we speak {PROTOCOL_VERSION})",
            path="$.protocol_version", code="future_version",
        )
    if pv < 1:
        raise ValidationError(
            f"protocol_version must be >= 1, got {pv}",
            path="$.protocol_version", code="past_version",
        )

    success = obj.get("success", True)
    if not isinstance(success, bool):
        raise ValidationError(
            f"success must be bool, got {type(success).__name__}",
            path="$.success", code="bad_success_type",
        )

    intent = obj.get("intent", "")
    if not isinstance(intent, str):
        raise ValidationError(
            f"intent must be string, got {type(intent).__name__}",
            path="$.intent", code="bad_intent_type",
        )

    message = obj.get("message", "")
    if not isinstance(message, str):
        raise ValidationError(
            f"message must be string, got {type(message).__name__}",
            path="$.message", code="bad_message_type",
        )

    error = obj.get("error", "")
    if not isinstance(error, str):
        raise ValidationError(
            f"error must be string, got {type(error).__name__}",
            path="$.error", code="bad_error_type",
        )

    # operations must be array if present
    ops_raw = obj.get("operations", [])
    if not isinstance(ops_raw, list):
        raise ValidationError(
            "operations must be an array",
            path="$.operations", code="ops_not_array",
        )

    # Each op validates individually.
    ops: List[ValidatedOperation] = []
    seen_refs: List[str] = []
    for i, op in enumerate(ops_raw):
        v = validate_operation(op, path=f"$.operations[{i}]", _seen_refs=seen_refs)
        ops.append(v)

    # Plan (optional)
    plan_steps: List[Dict[str, Any]] = []
    placement_info = ""
    plan_obj = obj.get("plan")
    if plan_obj is not None:
        if not isinstance(plan_obj, dict):
            raise ValidationError(
                "plan must be an object",
                path="$.plan", code="plan_not_object",
            )
        ps = plan_obj.get("steps", [])
        if not isinstance(ps, list):
            raise ValidationError(
                "plan.steps must be an array",
                path="$.plan.steps", code="plan_steps_not_array",
            )
        for j, s in enumerate(ps):
            if not isinstance(s, dict):
                raise ValidationError(
                    "plan step must be an object",
                    path=f"$.plan.steps[{j}]", code="plan_step_not_object",
                )
            idx = s.get("index", 0)
            if not isinstance(idx, int) or isinstance(idx, bool):
                raise ValidationError(
                    "plan step index must be int",
                    path=f"$.plan.steps[{j}].index", code="plan_step_bad_index",
                )
            desc = s.get("description", "")
            if not isinstance(desc, str):
                raise ValidationError(
                    "plan step description must be string",
                    path=f"$.plan.steps[{j}].description",
                    code="plan_step_bad_desc",
                )
            plan_steps.append({"index": idx, "description": desc})
        pi = plan_obj.get("placement_info", "")
        if not isinstance(pi, str):
            raise ValidationError(
                "plan.placement_info must be string",
                path="$.plan.placement_info",
                code="plan_placement_bad_type",
            )
        placement_info = pi

    erc = obj.get("erc")
    if erc is not None and not isinstance(erc, dict):
        raise ValidationError(
            "erc must be an object",
            path="$.erc", code="erc_not_object",
        )

    # success=False is allowed (failure response) but only if validations pass.
    return ValidatedResponse(
        protocol_version=pv,
        success=success,
        intent=intent,
        message=message,
        operations=ops,
        plan_steps=plan_steps,
        placement_info=placement_info,
        error=error,
        erc=erc,
        raw=obj,
    )


def validate_operation(
    op: Any,
    *,
    path: str = "$",
    _seen_refs: Optional[List[str]] = None,
) -> ValidatedOperation:
    """Validate a single op. _seen_refs is shared across a plan to enforce
    uniqueness; pass [] when validating one op standalone."""

    if not isinstance(op, dict):
        raise ValidationError(
            f"operation must be an object, got {type(op).__name__}",
            path=path, code="op_not_object",
        )
    t = op.get("type")
    if not isinstance(t, str) or not t:
        raise ValidationError(
            "operation.type must be non-empty string",
            path=f"{path}.type", code="op_no_type",
        )
    if t not in KNOWN_OP_TYPES:
        raise ValidationError(
            f"unknown operation type {t!r}",
            path=f"{path}.type", code="op_unknown_type",
        )
    data = op.get("data", {})
    if not isinstance(data, dict):
        raise ValidationError(
            "operation.data must be an object",
            path=f"{path}.data", code="op_bad_data",
        )

    if t == "PLACE_COMPONENT":
        _validate_place(data, path, _seen_refs)
    elif t == "ADD_WIRE":
        _validate_wire(data, path)
    elif t == "ADD_LABEL":
        _validate_label(data, path)
    elif t == "ADD_JUNCTION":
        _validate_junction(data, path)
    elif t == "ADD_POWER_SYMBOL":
        _validate_power(data, path)

    return ValidatedOperation(type=t, data=data)


# ── per-type validators ───────────────────────────────────────────────────


def _validate_coord(v: Any, *, name: str, path: str) -> None:
    if not isinstance(v, int) or isinstance(v, bool):
        raise ValidationError(
            f"{name} must be int, got {type(v).__name__}",
            path=f"{path}.{name}", code="bad_coord_type",
        )
    if v < COORD_MIN or v > COORD_MAX:
        raise ValidationError(
            f"{name}={v} out of range [{COORD_MIN}, {COORD_MAX}]",
            path=f"{path}.{name}", code="coord_out_of_range",
        )


def _validate_place(data: Dict[str, Any], path: str, seen_refs: Optional[List[str]]) -> None:
    lib_id = data.get("lib_id", "")
    if not isinstance(lib_id, str) or not lib_id:
        raise ValidationError(
            "PLACE_COMPONENT: lib_id missing or empty",
            path=f"{path}.data.lib_id", code="place_no_lib_id",
        )
    if ":" not in lib_id:
        raise ValidationError(
            f"PLACE_COMPONENT: lib_id must be 'lib:symbol', got {lib_id!r}",
            path=f"{path}.data.lib_id", code="place_bad_lib_id",
        )

    ref = data.get("reference", "")
    if not isinstance(ref, str) or not ref:
        raise ValidationError(
            "PLACE_COMPONENT: reference missing or empty",
            path=f"{path}.data.reference", code="place_no_ref",
        )

    if seen_refs is not None:
        if ref in seen_refs:
            raise ValidationError(
                f"PLACE_COMPONENT: duplicate reference {ref!r}",
                path=f"{path}.data.reference", code="place_dup_ref",
            )
        seen_refs.append(ref)

    val = data.get("value", "")
    if not isinstance(val, str):
        raise ValidationError(
            "PLACE_COMPONENT: value must be string",
            path=f"{path}.data.value", code="place_bad_value",
        )

    _validate_coord(data.get("x", 0), name="x", path=f"{path}.data")
    _validate_coord(data.get("y", 0), name="y", path=f"{path}.data")

    rot = data.get("rotation", 0.0)
    # Accept int 0/90/180/270 too — coerce to float for compare
    if isinstance(rot, bool):
        raise ValidationError(
            "PLACE_COMPONENT: rotation must be number",
            path=f"{path}.data.rotation", code="place_bad_rot",
        )
    if not isinstance(rot, (int, float)):
        raise ValidationError(
            f"PLACE_COMPONENT: rotation must be number, got {type(rot).__name__}",
            path=f"{path}.data.rotation", code="place_bad_rot",
        )
    if float(rot) not in KNOWN_ROTATIONS:
        raise ValidationError(
            f"PLACE_COMPONENT: rotation {rot} not in {KNOWN_ROTATIONS}",
            path=f"{path}.data.rotation", code="place_bad_rot_value",
        )


def _validate_wire(data: Dict[str, Any], path: str) -> None:
    _validate_coord(data.get("start_x", 0), name="start_x", path=f"{path}.data")
    _validate_coord(data.get("start_y", 0), name="start_y", path=f"{path}.data")
    _validate_coord(data.get("end_x", 0), name="end_x", path=f"{path}.data")
    _validate_coord(data.get("end_y", 0), name="end_y", path=f"{path}.data")
    sx, sy = data.get("start_x", 0), data.get("start_y", 0)
    ex, ey = data.get("end_x", 0), data.get("end_y", 0)
    if (sx, sy) == (ex, ey):
        raise ValidationError(
            f"ADD_WIRE: zero-length wire at ({sx},{sy})",
            path=f"{path}.data", code="wire_zero_length",
        )


def _validate_label(data: Dict[str, Any], path: str) -> None:
    name = data.get("name", "")
    if not isinstance(name, str) or not name:
        raise ValidationError(
            "ADD_LABEL: name missing or empty",
            path=f"{path}.data.name", code="label_no_name",
        )
    if len(name) > 64:
        raise ValidationError(
            f"ADD_LABEL: name longer than 64 chars ({len(name)})",
            path=f"{path}.data.name", code="label_long_name",
        )
    _validate_coord(data.get("x", 0), name="x", path=f"{path}.data")
    _validate_coord(data.get("y", 0), name="y", path=f"{path}.data")
    kind = data.get("label_type", "local")
    if kind not in KNOWN_LABEL_TYPES:
        raise ValidationError(
            f"ADD_LABEL: label_type {kind!r} not in {KNOWN_LABEL_TYPES}",
            path=f"{path}.data.label_type", code="label_bad_type",
        )


def _validate_junction(data: Dict[str, Any], path: str) -> None:
    _validate_coord(data.get("x", 0), name="x", path=f"{path}.data")
    _validate_coord(data.get("y", 0), name="y", path=f"{path}.data")


def _validate_power(data: Dict[str, Any], path: str) -> None:
    net = data.get("net_name", "")
    if not isinstance(net, str) or not net:
        raise ValidationError(
            "ADD_POWER_SYMBOL: net_name missing or empty",
            path=f"{path}.data.net_name", code="power_no_net",
        )
    _validate_coord(data.get("x", 0), name="x", path=f"{path}.data")
    _validate_coord(data.get("y", 0), name="y", path=f"{path}.data")


# ── geometry checks used by Check 7 (applied-board quality) ────────────────


def check_no_symbol_overlap(symbols: List["Tuple[str, int, int]"]) -> List[Tuple[str, str]]:
    """Return [(ref_a, ref_b), …] for any pair of symbols sharing the same
    anchor point. Empty list = no overlap."""
    by_pos: Dict[Tuple[int, int], List[str]] = {}
    for ref, x, y in symbols:
        by_pos.setdefault((x, y), []).append(ref)
    out: List[Tuple[str, str]] = []
    for refs in by_pos.values():
        if len(refs) > 1:
            for i in range(len(refs)):
                for j in range(i + 1, len(refs)):
                    out.append((refs[i], refs[j]))
    return out
