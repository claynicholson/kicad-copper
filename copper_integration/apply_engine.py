"""
ApplyEngine — turn a validated CopperResponse into atomic SchematicApi calls.

Mirrors the semantics of COPPER_CHAT_PANEL::ExecuteOperations on the C++ side
but with the fail-closed validation policy from ADR-004:

    1. validate the response (PROTOCOL.md hard-rejects).
    2. open one commit on the SchematicApi.
    3. for each op, call the right SchematicApi method.
    4. if any op raises SchematicError → abort_commit → return ApplyError.
    5. if all ops succeed → push_commit → ApplyResult(ok=True).

Also verifies the §10 quality bar (check 7) on the resulting state:
    - no two SCH_SYMBOLs share the same anchor.
    - every PLACE_COMPONENT.reference is uniquely realized.
    - no new symbol overlaps any pre-existing symbol's anchor.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Tuple

from .schematic_api import SchematicApi, SchematicError, CommitToken
from .validators import (
    ValidatedOperation,
    ValidatedResponse,
    ValidationError,
    validate_response,
    check_no_symbol_overlap,
)


class ApplyError(Exception):
    """Raised when ApplyEngine cannot apply a plan. Carries a typed code
    so the UI can surface an actionable message."""

    def __init__(self, message: str, *, code: str = "", path: str = ""):
        super().__init__(message)
        self.code = code
        self.path = path

    def __repr__(self) -> str:  # pragma: no cover
        return f"ApplyError(code={self.code!r}, path={self.path!r}, msg={self.args[0]!r})"


@dataclass
class ApplyResult:
    ok: bool
    commit_id: Optional[str] = None
    label: str = ""
    placed_symbols: int = 0
    wires: int = 0
    labels: int = 0
    junctions: int = 0
    power_symbols: int = 0
    warnings: List[str] = field(default_factory=list)


class ApplyEngine:
    """Stateless. One instance can apply many plans; safe to reuse across
    SchematicApis (each call takes its own api)."""

    def __init__(self, *, default_label: str = "Copper AI: Execute plan"):
        self._default_label = default_label

    # ── primary entrypoints ──

    def apply_response(
        self,
        api: SchematicApi,
        response_obj: Any,
        *,
        label: Optional[str] = None,
        pre_state_snapshot: Optional[bytes] = None,
    ) -> ApplyResult:
        """Validate + apply in one call. response_obj is a dict (parsed JSON)
        or an already-validated ValidatedResponse.

        If pre_state_snapshot is provided AND the apply fails for any reason,
        the engine asserts that api.serialize() byte-equals pre_state_snapshot
        (a redundant safety net — the real guarantee comes from abort_commit).
        """

        if isinstance(response_obj, ValidatedResponse):
            validated = response_obj
        else:
            try:
                validated = validate_response(response_obj)
            except ValidationError as ve:
                raise ApplyError(
                    f"response validation failed: {ve}",
                    code="response_invalid",
                    path=ve.path,
                ) from ve

        return self.apply_validated(api, validated, label=label,
                                    pre_state_snapshot=pre_state_snapshot)

    def apply_validated(
        self,
        api: SchematicApi,
        validated: ValidatedResponse,
        *,
        label: Optional[str] = None,
        pre_state_snapshot: Optional[bytes] = None,
    ) -> ApplyResult:
        if not validated.success:
            raise ApplyError(
                f"backend reported failure: {validated.error or '(no message)'}",
                code="backend_failure",
            )

        ops = validated.operations
        if not ops:
            # Empty plan is treated as a no-op success — matches the C++
            # behavior (`if( aOperations.empty() ) return;` would be the
            # natural read; we explicitly do nothing here).
            return ApplyResult(ok=True, commit_id=None, label=label or self._default_label)

        commit_label = label or self._default_label
        pre = pre_state_snapshot
        if pre is None:
            pre = api.serialize()

        # Pre-flight: check no PLACE_COMPONENT reference collides with the
        # existing schematic, before opening the commit. This is one of the
        # few cases where we want a clean error before any state mutation.
        existing_refs = {s.reference for s in api.list_symbols()}
        for i, op in enumerate(ops):
            if op.type == "PLACE_COMPONENT":
                ref = op.data.get("reference", "")
                if ref in existing_refs:
                    raise ApplyError(
                        f"plan reference {ref!r} already exists on the schematic",
                        code="ref_already_on_sheet",
                        path=f"$.operations[{i}].data.reference",
                    )

        # Pre-flight overlap with existing symbols.
        existing_anchors = {(s.x, s.y) for s in api.list_symbols()}
        for i, op in enumerate(ops):
            if op.type == "PLACE_COMPONENT":
                anchor = (op.data.get("x", 0), op.data.get("y", 0))
                if anchor in existing_anchors:
                    raise ApplyError(
                        f"plan would place a symbol on top of existing item at {anchor}",
                        code="overlap_with_existing",
                        path=f"$.operations[{i}].data",
                    )

        # Open the commit and execute.
        token = api.begin_commit(commit_label)
        result = ApplyResult(ok=False, commit_id=None, label=commit_label)

        try:
            for i, op in enumerate(ops):
                self._dispatch(api, token, op, op_index=i)
                if op.type == "PLACE_COMPONENT":
                    result.placed_symbols += 1
                elif op.type == "ADD_WIRE":
                    result.wires += 1
                elif op.type == "ADD_LABEL":
                    result.labels += 1
                elif op.type == "ADD_JUNCTION":
                    result.junctions += 1
                elif op.type == "ADD_POWER_SYMBOL":
                    result.power_symbols += 1
        except SchematicError as se:
            api.abort_commit(token)
            assert api.serialize() == pre, (
                "abort_commit failed to restore byte-equal state — fatal bug"
            )
            raise ApplyError(
                f"apply failed mid-plan: {se}",
                code="op_failed",
            ) from se
        except Exception as e:
            api.abort_commit(token)
            assert api.serialize() == pre, (
                "abort_commit failed to restore byte-equal state — fatal bug"
            )
            raise ApplyError(
                f"unexpected error during apply: {e}",
                code="unexpected",
            ) from e

        # All ops succeeded; quality gate before push.
        try:
            self._post_apply_quality_check(api, token, validated)
        except ApplyError:
            api.abort_commit(token)
            assert api.serialize() == pre, (
                "abort_commit failed to restore byte-equal state — fatal bug"
            )
            raise

        cid = api.push_commit(token)
        result.ok = True
        result.commit_id = cid
        return result

    # ── dispatch ──

    def _dispatch(
        self,
        api: SchematicApi,
        token: CommitToken,
        op: ValidatedOperation,
        *,
        op_index: int,
    ) -> None:
        t = op.type
        d = op.data
        if t == "PLACE_COMPONENT":
            api.place_component(token, d)
        elif t == "ADD_WIRE":
            api.add_wire(token, d)
        elif t == "ADD_LABEL":
            api.add_label(token, d)
        elif t == "ADD_JUNCTION":
            api.add_junction(token, d)
        elif t == "ADD_POWER_SYMBOL":
            api.add_power_symbol(token, d)
        else:  # pragma: no cover — validator should have caught this
            raise SchematicError(f"unknown op type {t!r} at index {op_index}")

    # ── post-apply quality gate (check 7) ──

    def _post_apply_quality_check(
        self,
        api: SchematicApi,
        token: CommitToken,
        validated: ValidatedResponse,
    ) -> None:
        """Run §10 quality bar against the about-to-be-pushed batch.

        We can't directly inspect the pending batch via the SchematicApi
        contract (it would leak the commit abstraction). Instead, we
        reconstruct what the post-push state will look like by reading
        committed state plus the validated ops. This keeps the check
        contained to the public surface.
        """

        existing = list(api.list_symbols())
        # Build the "after" set: existing + about-to-place from this plan.
        after: List[Tuple[str, int, int]] = []
        for s in existing:
            after.append((s.reference, s.x, s.y))
        for op in validated.operations:
            if op.type == "PLACE_COMPONENT":
                after.append((
                    op.data.get("reference", ""),
                    op.data.get("x", 0),
                    op.data.get("y", 0),
                ))
            elif op.type == "ADD_POWER_SYMBOL":
                # Power symbols are auto-referenced; they overlap is checked
                # by anchor only — different power-symbols ARE allowed to
                # share anchors? No — KiCad expects them at distinct points.
                # We use a synthetic ref to feed the same overlap check.
                ref = f"#PWR_pending_{op.data.get('net_name', '')}_{op.data.get('x', 0)}_{op.data.get('y', 0)}"
                after.append((ref, op.data.get("x", 0), op.data.get("y", 0)))

        overlaps = check_no_symbol_overlap(after)
        if overlaps:
            a, b = overlaps[0]
            raise ApplyError(
                f"plan would create overlapping symbols at the same anchor "
                f"({a!r} and {b!r})",
                code="symbol_overlap",
            )

        # All plan-declared references unique within after-state — already
        # ensured by the validator + the pre-flight existing-ref check, but
        # we re-verify cheaply here as a belt-and-braces check.
        refs = [r for r, _, _ in after if not r.startswith("#PWR_pending_")]
        if len(set(refs)) != len(refs):
            from collections import Counter
            dup = next(r for r, c in Counter(refs).items() if c > 1)
            raise ApplyError(
                f"duplicate reference after apply: {dup!r}",
                code="ref_duplicate_after_apply",
            )
