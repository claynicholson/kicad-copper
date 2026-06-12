"""validators.py — backward-compat shim over copper.protocol (Phase 1 L2).

Before Phase 1 this file owned the validation rules in ~280 lines of hand-
rolled checks. After Phase 1 the source of truth is copper-2's Pydantic
models. We expose the same names so existing tests / code don't need to
change import paths.

Why a shim instead of full deletion: existing tests + ApplyEngine import
`validate_response` / `validate_operation` / `ValidationError` /
`check_no_symbol_overlap` / the rotation + label-type constants. Pydantic
gives equivalent validation but with different exception types and a
different success object. The shim adapts:

    raw dict   → validate_response → ValidatedResponse (mirror of old shape)
    raw op     → validate_operation → ValidatedOperation
    Pydantic ValidationError → our ValidationError (with .path + .code)

Same semantics, same hard-rejects (PROTOCOL.md), one source of truth.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Dict, List, Optional, Tuple

# Pull canonical names from the protocol package. The path injection runs
# at `copper_integration.__init__` import time.
from copper.protocol import (  # noqa: E402
    ALLOWED_ROTATIONS as _ALLOWED_ROTATIONS_TUPLE,
    COORD_MAX,
    COORD_MIN,
    OP_TYPE_NAMES as KNOWN_OP_TYPES,
    PROTOCOL_VERSION,
)
from copper.protocol.apply_plan import (  # noqa: E402
    ApplyPlan as _PydApplyPlan,
    AddJunction as _PydAddJunction,
    AddLabel as _PydAddLabel,
    AddNoConnect as _PydAddNoConnect,
    AddPinLabel as _PydAddPinLabel,
    AddPowerSymbol as _PydAddPowerSymbol,
    AddWire as _PydAddWire,
    PlaceComponent as _PydPlaceComponent,
    PlacementHints as _PydPlacementHints,
)
from pydantic import ValidationError as _PydValidationError  # noqa: E402


# Compat: tests reference both the tuple and the literal sequence.
KNOWN_ROTATIONS = _ALLOWED_ROTATIONS_TUPLE
KNOWN_LABEL_TYPES = ("local", "global", "hierarchical")


class ValidationError(Exception):
    """Same shape as the pre-Phase-1 exception so existing tests pass.
    `.path` points at the offending JSON Pointer-ish location; `.code` is
    a stable identifier the harness matches on."""

    def __init__(self, message: str, *, path: str = "", code: str = ""):
        super().__init__(message)
        self.path = path
        self.code = code

    def __repr__(self) -> str:  # pragma: no cover
        return (
            f"ValidationError(code={self.code!r}, path={self.path!r}, "
            f"msg={self.args[0]!r})"
        )


@dataclass
class ValidatedOperation:
    type: str
    data: Dict[str, Any]


@dataclass
class ValidatedResponse:
    """Same shape the legacy harness exposed — drives ApplyEngine and tests."""
    protocol_version: int
    success: bool
    intent: str
    message: str
    operations: List[ValidatedOperation]
    plan_steps: List[Dict[str, Any]]
    placement_info: str
    error: str
    erc: Optional[Dict[str, Any]]
    raw: Dict[str, Any]


# ── op-type → Pydantic model for single-op validation ──────────────────────

_PYD_BY_TYPE = {
    "PLACE_COMPONENT": _PydPlaceComponent,
    "ADD_WIRE": _PydAddWire,
    "ADD_LABEL": _PydAddLabel,
    "ADD_JUNCTION": _PydAddJunction,
    "ADD_POWER_SYMBOL": _PydAddPowerSymbol,
    "ADD_PIN_LABEL": _PydAddPinLabel,
    "ADD_NO_CONNECT": _PydAddNoConnect,
    "PLACEMENT_HINTS": _PydPlacementHints,
}


def _normalize_op_for_pydantic(op: Any) -> Dict[str, Any]:
    """The wire shape is `{type, data}` with op-specific fields under `data`.
    Pydantic models are flat (type + fields at the top level). Flatten."""
    if not isinstance(op, dict):
        raise ValidationError(
            f"operation must be an object, got {type(op).__name__}",
            code="op_not_object",
        )
    t = op.get("type")
    if not isinstance(t, str) or not t:
        raise ValidationError(
            "operation.type must be non-empty string",
            code="op_no_type",
        )
    if t not in KNOWN_OP_TYPES:
        raise ValidationError(
            f"unknown operation type {t!r}",
            code="op_unknown_type",
        )
    data = op.get("data", {})
    if not isinstance(data, dict):
        raise ValidationError(
            "operation.data must be an object",
            code="op_bad_data",
        )
    flat = {"type": t}
    flat.update(data)
    return flat


# ── exception translation: Pydantic → our ValidationError with stable codes ─

# Map (op_type, pydantic_loc_tail, pydantic_error_type) → (.code, friendly msg).
# loc_tail is the LAST element of the pydantic-reported location. Add cases
# here as new validation rules land in copper.protocol. Default is
# code="schema" with the raw pydantic message.
# Pydantic int-related error types: 'int_type' (wrong type), 'int_from_float'
# (float passed, has fractional part). Both map to the legacy bad_coord_type
# code so existing tests don't have to know the distinction.
_INT_TYPE_ERRORS = ("int_type", "int_from_float", "int_parsing")
_RANGE_ERRORS = ("less_than_equal", "greater_than_equal")

_CODE_FROM_PYD: Dict[Tuple[str, str, str], str] = {
    # PLACE_COMPONENT
    ("PLACE_COMPONENT", "lib_id", "string_too_short"): "place_no_lib_id",
    ("PLACE_COMPONENT", "lib_id", "value_error"): "place_bad_lib_id",
    ("PLACE_COMPONENT", "reference", "string_too_short"): "place_no_ref",
    ("PLACE_COMPONENT", "rotation", "value_error"): "place_bad_rot_value",
    # footprint: 'Lib:Name' format (value_error), must be a string
    # (string_type), max_length (string_too_long) — one stable code.
    ("PLACE_COMPONENT", "footprint", "value_error"): "place_bad_footprint",
    ("PLACE_COMPONENT", "footprint", "string_type"): "place_bad_footprint",
    ("PLACE_COMPONENT", "footprint", "string_too_long"): "place_bad_footprint",
    # ADD_LABEL
    ("ADD_LABEL", "name", "string_too_short"): "label_no_name",
    ("ADD_LABEL", "name", "string_too_long"): "label_long_name",
    ("ADD_LABEL", "label_type", "literal_error"): "label_bad_type",
    # ADD_POWER_SYMBOL
    ("ADD_POWER_SYMBOL", "net_name", "string_too_short"): "power_no_net",
}

# Coord errors are op-agnostic — same code regardless of op type.
_COORD_FIELDS = {"x", "y", "start_x", "start_y", "end_x", "end_y"}


def _convert_pyd_error(pyd_err: _PydValidationError, *, op_type: str, op_index: int) -> ValidationError:
    """Pick the first error from a pydantic ValidationError and translate."""
    errs = pyd_err.errors()
    first = errs[0] if errs else {}
    loc = first.get("loc", ())
    msg = first.get("msg", str(pyd_err))
    etype = first.get("type", "")
    tail = str(loc[-1]) if loc else ""

    # Whole-model wire validation (zero-length): pydantic raises with loc=()
    # and msg containing "zero length"
    if op_type == "ADD_WIRE" and "zero length" in msg.lower():
        return ValidationError(
            f"ADD_WIRE: zero-length wire",
            path=f"$.operations[{op_index}].data",
            code="wire_zero_length",
        )

    if op_type == "PLACE_COMPONENT" and tail == "rotation":
        code = "place_bad_rot_value"
    elif op_type == "PLACE_COMPONENT" and "must be 'lib:symbol'" in msg:
        code = "place_bad_lib_id"
    elif op_type == "PLACE_COMPONENT" and tail == "lib_id" and "non-empty" in msg:
        code = "place_no_lib_id"
    elif tail in _COORD_FIELDS and etype in _INT_TYPE_ERRORS:
        code = "bad_coord_type"
    elif tail in _COORD_FIELDS and etype in _RANGE_ERRORS:
        code = "coord_out_of_range"
    else:
        code = _CODE_FROM_PYD.get((op_type, tail, etype), "schema")

    return ValidationError(
        f"{op_type}: {msg}",
        path=f"$.operations[{op_index}].data.{tail}" if tail else f"$.operations[{op_index}].data",
        code=code,
    )


def validate_operation(
    op: Any,
    *,
    path: str = "$",
    _seen_refs: Optional[List[str]] = None,
) -> ValidatedOperation:
    """Validate one op against the canonical Pydantic model for its type.
    Enforces PLACE_COMPONENT reference uniqueness when _seen_refs is passed."""
    flat = _normalize_op_for_pydantic(op)
    t = flat["type"]
    model = _PYD_BY_TYPE[t]
    try:
        model.model_validate(flat)
    except _PydValidationError as e:
        # We don't know the index here; the caller's path arg is purely
        # cosmetic. Use a dummy index 0.
        raise _convert_pyd_error(e, op_type=t, op_index=0) from e

    if t == "PLACE_COMPONENT" and _seen_refs is not None:
        ref = flat.get("reference", "")
        if ref in _seen_refs:
            raise ValidationError(
                f"PLACE_COMPONENT: duplicate reference {ref!r}",
                path=f"{path}.data.reference",
                code="place_dup_ref",
            )
        _seen_refs.append(ref)

    return ValidatedOperation(type=t, data=op.get("data", {}))


def validate_response(obj: Any) -> ValidatedResponse:
    """Validate the full response against Pydantic ApplyPlan, then re-shape
    into the legacy ValidatedResponse dataclass so existing callers don't
    change."""
    if not isinstance(obj, dict):
        raise ValidationError(
            f"response must be a JSON object, got {type(obj).__name__}",
            code="not_object",
        )

    # PROTOCOL.md specifies `protocol_version` is required. Pydantic's Literal
    # type lets missing field default to PROTOCOL_VERSION, which would mask
    # the missing-version case from the existing test suite. Pre-check.
    pv = obj.get("protocol_version")
    if pv is None:
        raise ValidationError(
            "missing protocol_version", path="$.protocol_version",
            code="no_version",
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

    # Other pre-checks the legacy validator had — Pydantic's reports are less
    # specific so we keep these for stable test codes.
    for field, want_type, code in (
        ("success", bool, "bad_success_type"),
        ("intent", str, "bad_intent_type"),
        ("message", str, "bad_message_type"),
        ("error", str, "bad_error_type"),
    ):
        if field in obj and not isinstance(obj[field], want_type):
            raise ValidationError(
                f"{field} must be {want_type.__name__}, "
                f"got {type(obj[field]).__name__}",
                path=f"$.{field}", code=code,
            )

    ops_raw = obj.get("operations", [])
    if not isinstance(ops_raw, list):
        raise ValidationError(
            "operations must be an array",
            path="$.operations", code="ops_not_array",
        )

    # Per-op validation with cross-op reference tracking.
    ops: List[ValidatedOperation] = []
    seen_refs: List[str] = []
    for i, op in enumerate(ops_raw):
        try:
            v = validate_operation(op, path=f"$.operations[{i}]",
                                   _seen_refs=seen_refs)
        except ValidationError as ve:
            # Preserve the path with the real index.
            ve.path = ve.path.replace("operations[0]", f"operations[{i}]") if ve.path else f"$.operations[{i}]"
            raise
        ops.append(v)

    # Now run the whole-plan Pydantic validation — catches anything we missed
    # (e.g. the duplicate-reference root validator on ApplyPlan, plan-step
    # type checks, etc.).
    try:
        full = _PydApplyPlan.model_validate(obj)
    except _PydValidationError as e:
        first = e.errors()[0] if e.errors() else {}
        msg = first.get("msg", str(e))
        loc = first.get("loc", ())
        etype = first.get("type", "")

        # Translate common whole-plan errors to stable .code values so tests
        # can match on them. Pydantic's loc is a tuple — match on prefixes.
        code = "schema"
        if len(loc) >= 4 and loc[0] == "plan" and loc[1] == "steps":
            field = loc[3] if len(loc) > 3 else ""
            if field == "index" and etype in _INT_TYPE_ERRORS:
                code = "plan_step_bad_index"
            elif field == "description":
                code = "plan_step_bad_desc"
            else:
                code = "plan_step_not_object"
        elif len(loc) >= 1 and loc[0] == "operations":
            code = "schema"  # per-op problems already caught upstream
        elif "Value error" in msg and "duplicate reference" in msg:
            code = "place_dup_ref"

        path = "$." + ".".join(str(x) for x in loc) if loc else "$"
        raise ValidationError(f"schema: {msg}", path=path, code=code) from e

    # Re-shape into the legacy dataclass.
    plan_steps = [{"index": s.index, "description": s.description}
                  for s in full.plan.steps]
    erc = full.erc.model_dump() if full.erc is not None else None

    return ValidatedResponse(
        protocol_version=full.protocol_version,
        success=full.success,
        intent=full.intent,
        message=full.message,
        operations=ops,
        plan_steps=plan_steps,
        placement_info=full.plan.placement_info,
        error=full.error,
        erc=erc,
        raw=obj,
    )


# ── geometry helper retained from the legacy file (no copper.protocol equiv) ─


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
