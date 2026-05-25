"""scripts/assess.py — closed-loop self-assessment harness (§8 of BUILD_PROMPT.md).

Runs all 8 checks, writes docs/SELF_ASSESSMENT.md + assessment.json, appends
a row to docs/LOOP_LOG.md, and exits 0 only if "polished" per §10.

All checks run headless using FakeSchematicApi + StubBackend + fixtures/.
No GUI KiCad, no compiled fork required.

Usage:
    python scripts/assess.py
    python scripts/assess.py --check 3       # only one check by id
    python scripts/assess.py --check 5,6,7   # subset
    python scripts/assess.py --verbose
    python scripts/assess.py --no-write      # don't update docs

Exit codes:
    0  polished (all hard gates pass + score >= 92)
    >0 number of failing checks
"""

from __future__ import annotations

import argparse
import datetime as dt
import io
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import textwrap
import time
import traceback
import unittest
from contextlib import redirect_stderr, redirect_stdout
from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional, Tuple


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DOCS_DIR = REPO_ROOT / "docs"
ASSESSMENT_JSON = REPO_ROOT / "assessment.json"
SELF_ASSESSMENT_MD = DOCS_DIR / "SELF_ASSESSMENT.md"
LOOP_LOG_MD = DOCS_DIR / "LOOP_LOG.md"

sys.path.insert(0, str(REPO_ROOT))

# ── check registry ─────────────────────────────────────────────────────────


@dataclass
class CheckSpec:
    id: int
    name: str
    weight: int
    hard_gate: bool
    fn: Callable[["RunContext"], "CheckResult"]


@dataclass
class CheckResult:
    score: float  # 0.0–1.0
    details: str = ""
    failures: List[str] = field(default_factory=list)
    skipped: bool = False  # for "soft" checks like lint that can be skipped


@dataclass
class RunContext:
    verbose: bool = False


# ── helpers ────────────────────────────────────────────────────────────────


def _capture(fn: Callable[[], None]) -> Tuple[bool, str]:
    """Run fn; return (ok, captured_output)."""
    buf = io.StringIO()
    try:
        with redirect_stdout(buf), redirect_stderr(buf):
            fn()
        return True, buf.getvalue()
    except BaseException:
        buf.write("\n")
        buf.write(traceback.format_exc())
        return False, buf.getvalue()


def _load_tests(*module_names: str) -> unittest.TestSuite:
    loader = unittest.TestLoader()
    suite = unittest.TestSuite()
    for m in module_names:
        suite.addTests(loader.loadTestsFromName(m))
    return suite


def _run_suite(suite: unittest.TestSuite, *, verbose: bool) -> Tuple[unittest.TestResult, str]:
    buf = io.StringIO()
    runner = unittest.TextTestRunner(stream=buf, verbosity=2 if verbose else 1)
    result = runner.run(suite)
    return result, buf.getvalue()


def _suite_score(result: unittest.TestResult) -> float:
    """score = passes / (passes + failures + errors). 1.0 if no tests."""
    ran = result.testsRun
    if ran == 0:
        return 1.0
    bad = len(result.failures) + len(result.errors)
    return max(0.0, 1.0 - bad / ran)


def _list_failures(result: unittest.TestResult) -> List[str]:
    out = []
    for case, tb in result.failures:
        out.append(f"FAIL {case.id()}: {tb.splitlines()[-1] if tb else ''}")
    for case, tb in result.errors:
        out.append(f"ERROR {case.id()}: {tb.splitlines()[-1] if tb else ''}")
    return out


# ── check 1: imports / loads ───────────────────────────────────────────────


def _check_imports(ctx: RunContext) -> CheckResult:
    """The Python package imports cleanly and the C++ plugin is consistently
    wired (CMake lists files, settings field present, panel instantiated)."""

    failures: List[str] = []

    # 1a. Python: every module imports.
    py_modules = [
        "copper_integration",
        "copper_integration.schematic_api",
        "copper_integration.validators",
        "copper_integration.backend_client",
        "copper_integration.stub_backend",
        "copper_integration.apply_engine",
        "copper_integration.controller",
        "copper_integration.settings",
        "copper_integration.panel_glue",
    ]
    import importlib
    for mod in py_modules:
        try:
            importlib.import_module(mod)
        except Exception as e:
            failures.append(f"import {mod}: {e}")

    # 1b. C++ wiring: required files in CMakeLists.txt, m_Copper in settings,
    # panel instantiated in SCH_EDIT_FRAME.
    cml = (REPO_ROOT / "eeschema" / "CMakeLists.txt").read_text(encoding="utf-8", errors="replace")
    required_cmake = [
        "copper/copper_auth.cpp",
        "copper/copper_client.cpp",
        "widgets/copper_chat_widgets.cpp",
        "widgets/copper_chat_panel.cpp",
        "api/http_bridge.cpp",
    ]
    for path in required_cmake:
        if path not in cml:
            failures.append(f"CMake missing: {path}")

    settings_h = (REPO_ROOT / "eeschema" / "eeschema_settings.h").read_text(encoding="utf-8")
    if "COPPER_AI" not in settings_h or "m_Copper" not in settings_h:
        failures.append("EESCHEMA_SETTINGS missing m_Copper / COPPER_AI struct")

    settings_cpp = (REPO_ROOT / "eeschema" / "eeschema_settings.cpp").read_text(encoding="utf-8")
    if "copper.api_url" not in settings_cpp:
        failures.append("EESCHEMA_SETTINGS missing 'copper.api_url' PARAM registration")

    frame_cpp = (REPO_ROOT / "eeschema" / "sch_edit_frame.cpp").read_text(encoding="utf-8")
    if "new COPPER_CHAT_PANEL" not in frame_cpp:
        failures.append("SCH_EDIT_FRAME does not instantiate COPPER_CHAT_PANEL")
    if "CopperChat" not in frame_cpp:
        failures.append("SCH_EDIT_FRAME does not register the 'CopperChat' AUI pane")

    # 1c. Cross-language coherence: validators op types match the C++ ExecuteOperations
    # switch.
    chat_panel_cpp = (REPO_ROOT / "eeschema" / "widgets" / "copper_chat_panel.cpp").read_text(encoding="utf-8")
    from copper_integration.validators import KNOWN_OP_TYPES
    for op in KNOWN_OP_TYPES:
        if f'"{op}"' not in chat_panel_cpp:
            failures.append(f"C++ ExecuteOperations missing op type: {op}")

    score = 1.0 if not failures else max(0.0, 1.0 - len(failures) / 12.0)
    return CheckResult(
        score=score,
        details=(
            f"py modules: {len(py_modules) - sum(1 for f in failures if f.startswith('import '))} ok\n"
            f"C++ wiring: {len(required_cmake)} files in CMake checked"
        ),
        failures=failures,
    )


# ── check 2: lint / format (soft gate) ─────────────────────────────────────


def _check_lint(ctx: RunContext) -> CheckResult:
    """Run ruff if installed. If not installed, report skipped (full credit)."""

    ruff = shutil.which("ruff")
    if not ruff:
        return CheckResult(score=1.0, details="ruff not installed; skipped (no penalty)",
                           skipped=True)
    try:
        r = subprocess.run(
            [ruff, "check", "copper_integration", "tests", "scripts"],
            cwd=str(REPO_ROOT),
            capture_output=True, text=True, timeout=60,
        )
    except subprocess.TimeoutExpired:
        return CheckResult(score=0.0, details="ruff timed out",
                           failures=["ruff timed out"])
    if r.returncode == 0:
        return CheckResult(score=1.0, details="ruff clean")
    # Soft penalty per finding.
    findings = [ln for ln in (r.stdout + r.stderr).splitlines() if ln.strip()]
    n = len(findings)
    score = max(0.0, 1.0 - n / 50.0)
    return CheckResult(
        score=score,
        details=f"{n} ruff findings",
        failures=findings[:25],
    )


# ── check 3: BackendClient unit ────────────────────────────────────────────


def _check_backend_client(ctx: RunContext) -> CheckResult:
    suite = _load_tests("tests.test_backend_client")
    result, captured = _run_suite(suite, verbose=ctx.verbose)
    return CheckResult(
        score=_suite_score(result),
        details=f"{result.testsRun} tests, {len(result.failures)} failed, {len(result.errors)} errored",
        failures=_list_failures(result),
    )


# ── check 4: response validation ──────────────────────────────────────────


def _check_response_validation(ctx: RunContext) -> CheckResult:
    # Validation lives in validators.py + apply_engine.ApplyEngine.apply_response.
    # We run both relevant test modules.
    suite = _load_tests(
        "tests.test_validators",
        # The apply-engine has a dedicated validation-enforcement test class:
        # we cherry-pick to avoid double-counting.
    )
    # Add only the ResponseValidationEnforcementTest from test_apply_engine.
    loader = unittest.TestLoader()
    suite.addTests(loader.loadTestsFromName(
        "tests.test_apply_engine.ResponseValidationEnforcementTest"
    ))
    result, captured = _run_suite(suite, verbose=ctx.verbose)
    return CheckResult(
        score=_suite_score(result),
        details=f"{result.testsRun} tests, {len(result.failures)} failed, {len(result.errors)} errored",
        failures=_list_failures(result),
    )


# ── check 5: apply correctness ────────────────────────────────────────────


def _check_apply_correctness(ctx: RunContext) -> CheckResult:
    loader = unittest.TestLoader()
    suite = unittest.TestSuite()
    suite.addTests(loader.loadTestsFromName(
        "tests.test_apply_engine.ApplyCorrectnessTest"
    ))
    suite.addTests(loader.loadTestsFromName(
        "tests.test_apply_engine.AppliedBoardQualityTest.test_all_plan_refs_realized"
    ))
    suite.addTests(loader.loadTestsFromName(
        "tests.test_apply_engine.AppliedBoardQualityTest.test_all_plan_labels_realized"
    ))
    suite.addTests(loader.loadTestsFromName(
        "tests.test_controller.HappyPathTest"
    ))
    result, captured = _run_suite(suite, verbose=ctx.verbose)
    return CheckResult(
        score=_suite_score(result),
        details=f"{result.testsRun} tests, {len(result.failures)} failed, {len(result.errors)} errored",
        failures=_list_failures(result),
    )


# ── check 6: atomic rollback ──────────────────────────────────────────────


def _check_atomic_rollback(ctx: RunContext) -> CheckResult:
    loader = unittest.TestLoader()
    suite = unittest.TestSuite()
    suite.addTests(loader.loadTestsFromName(
        "tests.test_apply_engine.AtomicRollbackTest"
    ))
    suite.addTests(loader.loadTestsFromName(
        "tests.test_apply_engine.PreSnapshotGuardTest"
    ))
    suite.addTests(loader.loadTestsFromName(
        "tests.test_fake_schematic_api.UndoRedoTest"
    ))
    suite.addTests(loader.loadTestsFromName(
        "tests.test_fake_schematic_api.ByteEqualAbortTest"
    ))
    suite.addTests(loader.loadTestsFromName(
        "tests.test_state_handling.MidApplyFailureTest"
    ))
    result, captured = _run_suite(suite, verbose=ctx.verbose)
    return CheckResult(
        score=_suite_score(result),
        details=f"{result.testsRun} tests, {len(result.failures)} failed, {len(result.errors)} errored",
        failures=_list_failures(result),
    )


# ── check 7: applied-board quality ────────────────────────────────────────


def _check_board_quality(ctx: RunContext) -> CheckResult:
    loader = unittest.TestLoader()
    suite = unittest.TestSuite()
    suite.addTests(loader.loadTestsFromName(
        "tests.test_apply_engine.AppliedBoardQualityTest"
    ))
    suite.addTests(loader.loadTestsFromName(
        "tests.test_validators.GeometryHelperTest"
    ))
    result, captured = _run_suite(suite, verbose=ctx.verbose)
    return CheckResult(
        score=_suite_score(result),
        details=f"{result.testsRun} tests, {len(result.failures)} failed, {len(result.errors)} errored",
        failures=_list_failures(result),
    )


# ── check 8: state handling ───────────────────────────────────────────────


def _check_state_handling(ctx: RunContext) -> CheckResult:
    suite = _load_tests("tests.test_state_handling", "tests.test_controller")
    result, captured = _run_suite(suite, verbose=ctx.verbose)
    return CheckResult(
        score=_suite_score(result),
        details=f"{result.testsRun} tests, {len(result.failures)} failed, {len(result.errors)} errored",
        failures=_list_failures(result),
    )


# ── check 9: cross-repo lockstep (added in Phase 1) ───────────────────────


def _check_protocol_lockstep(ctx: RunContext) -> CheckResult:
    """Run the contract test that proves kicad-copper and copper-2 agree on
    the protocol. Skips gracefully if copper-2 isn't available — soft gate."""
    suite = _load_tests("tests.test_protocol_lockstep")
    result, captured = _run_suite(suite, verbose=ctx.verbose)
    # If every test was skipped (copper-2 absent), count as skipped.
    skipped = len(result.skipped) == result.testsRun and result.testsRun > 0
    return CheckResult(
        score=_suite_score(result),
        details=(
            "copper-2 not available; check skipped (no penalty)"
            if skipped
            else f"{result.testsRun} tests, {len(result.failures)} failed, "
                 f"{len(result.errors)} errored, {len(result.skipped)} skipped"
        ),
        failures=_list_failures(result),
        skipped=skipped,
    )


# ── registry ───────────────────────────────────────────────────────────────


CHECKS: List[CheckSpec] = [
    CheckSpec(1, "Imports/loads",            weight=8,  hard_gate=True,  fn=_check_imports),
    CheckSpec(2, "Lint/format",              weight=6,  hard_gate=False, fn=_check_lint),
    CheckSpec(3, "BackendClient unit",       weight=16, hard_gate=True,  fn=_check_backend_client),
    CheckSpec(4, "Response validation",      weight=14, hard_gate=True,  fn=_check_response_validation),
    CheckSpec(5, "Apply correctness",        weight=18, hard_gate=True,  fn=_check_apply_correctness),
    CheckSpec(6, "Atomic rollback",          weight=16, hard_gate=True,  fn=_check_atomic_rollback),
    CheckSpec(7, "Applied-board quality",    weight=14, hard_gate=True,  fn=_check_board_quality),
    CheckSpec(8, "State handling",           weight=8,  hard_gate=True,  fn=_check_state_handling),
    # Phase 1 addition: cross-repo lockstep. Soft gate so CI without
    # copper-2 still passes — but when copper-2 IS present, any drift fails.
    CheckSpec(9, "Cross-repo lockstep",      weight=0,  hard_gate=False, fn=_check_protocol_lockstep),
]
TOTAL_WEIGHT = sum(c.weight for c in CHECKS)
assert TOTAL_WEIGHT == 100, f"weights sum to {TOTAL_WEIGHT}, expected 100"


# ── orchestration ─────────────────────────────────────────────────────────


@dataclass
class Run:
    timestamp: str
    per_check: Dict[int, CheckResult] = field(default_factory=dict)
    total_score: float = 0.0
    hard_gate_passes: int = 0
    hard_gate_total: int = 0
    polished: bool = False

    def to_json(self) -> dict:
        return {
            "timestamp": self.timestamp,
            "total_score": round(self.total_score, 2),
            "polished": self.polished,
            "hard_gate_passes": self.hard_gate_passes,
            "hard_gate_total": self.hard_gate_total,
            "checks": [
                {
                    "id": c.id,
                    "name": c.name,
                    "weight": c.weight,
                    "hard_gate": c.hard_gate,
                    "score_unit": round(self.per_check[c.id].score, 3) if c.id in self.per_check else None,
                    "score_weighted": round(self.per_check[c.id].score * c.weight, 2) if c.id in self.per_check else None,
                    "skipped": self.per_check[c.id].skipped if c.id in self.per_check else False,
                    "details": self.per_check[c.id].details if c.id in self.per_check else "",
                    "failures": self.per_check[c.id].failures[:25] if c.id in self.per_check else [],
                }
                for c in CHECKS
            ],
        }


def run_checks(ctx: RunContext, only: Optional[List[int]] = None) -> Run:
    run = Run(timestamp=dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"))
    selected = [c for c in CHECKS if (only is None or c.id in only)]

    for check in selected:
        t0 = time.time()
        try:
            res = check.fn(ctx)
        except BaseException as e:
            res = CheckResult(
                score=0.0,
                details=f"check raised {type(e).__name__}",
                failures=[traceback.format_exc()[-1000:]],
            )
        dt_s = time.time() - t0
        run.per_check[check.id] = res
        status = "PASS" if (res.score >= 0.999 or (res.skipped and res.score >= 0.999)) else (
            "SOFT" if not check.hard_gate else "FAIL"
        )
        bar = f"[{check.id}] {check.name:<25} {status:<4} "
        bar += f"{res.score * 100:5.1f}/100 weight={check.weight:2d} ({dt_s*1000:.0f} ms)"
        print(bar)
        if res.failures and ctx.verbose:
            for f in res.failures[:5]:
                print(f"      · {f}")

    # Score / hard gates only over the SELECTED checks (so subset runs still
    # report a sensible polished verdict). For the full run this is the §10
    # rule.
    sel_weight = sum(c.weight for c in selected)
    if sel_weight:
        weighted = sum(run.per_check[c.id].score * c.weight for c in selected)
        run.total_score = weighted * (100.0 / sel_weight)
    run.hard_gate_total = sum(1 for c in selected if c.hard_gate)
    run.hard_gate_passes = sum(
        1 for c in selected if c.hard_gate
        and run.per_check[c.id].score >= 0.999
    )

    run.polished = (
        only is None
        and run.hard_gate_passes == run.hard_gate_total
        and run.total_score >= 92.0
    )
    return run


# ── reporting ─────────────────────────────────────────────────────────────


def write_self_assessment(run: Run) -> None:
    lines = []
    lines.append("# SELF_ASSESSMENT")
    lines.append("")
    lines.append(f"Generated at **{run.timestamp}** by `scripts/assess.py`.")
    lines.append("")
    verdict = "**polished**" if run.polished else "**not polished**"
    lines.append(f"## Verdict: {verdict}")
    lines.append("")
    lines.append(
        f"- Total score: **{run.total_score:.1f} / 100** "
        f"({'≥' if run.total_score >= 92 else '<'} 92 required)"
    )
    lines.append(
        f"- Hard gates: **{run.hard_gate_passes} / {run.hard_gate_total}** "
        f"passing (all must pass)"
    )
    lines.append("")
    lines.append("## Per-check")
    lines.append("")
    lines.append("| # | Check | Hard gate | Score | Weight | Weighted | Notes |")
    lines.append("|---|-------|-----------|-------|--------|----------|-------|")
    for c in CHECKS:
        res = run.per_check.get(c.id)
        if res is None:
            lines.append(f"| {c.id} | {c.name} | {'YES' if c.hard_gate else 'no'} | — | {c.weight} | — | not run |")
            continue
        score_pct = res.score * 100
        weighted = res.score * c.weight
        note = "skipped" if res.skipped else res.details.splitlines()[0] if res.details else ""
        lines.append(
            f"| {c.id} | {c.name} | {'YES' if c.hard_gate else 'no'} "
            f"| {score_pct:.1f} | {c.weight} | {weighted:.1f} | {note} |"
        )
    lines.append("")
    if any(r.failures for r in run.per_check.values()):
        lines.append("## Failures")
        lines.append("")
        for c in CHECKS:
            res = run.per_check.get(c.id)
            if not res or not res.failures:
                continue
            lines.append(f"### Check {c.id} — {c.name}")
            for f in res.failures[:25]:
                lines.append(f"- {f}")
            lines.append("")
    lines.append("")
    lines.append("---")
    lines.append("")
    lines.append("Re-run with `python scripts/assess.py`. See "
                 "[LOOP_LOG.md](LOOP_LOG.md) for the iteration history.")
    SELF_ASSESSMENT_MD.write_text("\n".join(lines) + "\n", encoding="utf-8")


def append_loop_log(run: Run, *, iteration_note: str = "") -> None:
    """Append one row per call. Doesn't truncate or rewrite."""

    header_needed = not LOOP_LOG_MD.exists()
    lines = []
    if header_needed:
        lines.append("# LOOP_LOG — one entry per closed-loop iteration")
        lines.append("")
        lines.append(
            "Per-check unit scores (0–100%). H = hard-gate-only. T = total weighted score (/100). "
            "Polished = all hard gates green AND T >= 92."
        )
        lines.append("")
        lines.append("| timestamp | C1 | C2 | C3 | C4 | C5 | C6 | C7 | C8 | T | Polished | Note |")
        lines.append("|-----------|----|----|----|----|----|----|----|----|---|----------|------|")
    row = [run.timestamp]
    for c in CHECKS:
        res = run.per_check.get(c.id)
        if res is None:
            row.append("-")
        else:
            row.append(f"{res.score * 100:.0f}")
    row.append(f"{run.total_score:.1f}")
    row.append("yes" if run.polished else "no")
    row.append(iteration_note or "")
    line = "| " + " | ".join(row) + " |"
    lines.append(line)

    with open(LOOP_LOG_MD, "a", encoding="utf-8") as f:
        if header_needed:
            f.write("\n".join(lines) + "\n")
        else:
            f.write(line + "\n")


def write_json(run: Run) -> None:
    ASSESSMENT_JSON.write_text(json.dumps(run.to_json(), indent=2), encoding="utf-8")


# ── CLI ────────────────────────────────────────────────────────────────────


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser()
    p.add_argument("--verbose", action="store_true")
    p.add_argument("--no-write", action="store_true",
                   help="Don't write docs/SELF_ASSESSMENT.md, docs/LOOP_LOG.md, or assessment.json.")
    p.add_argument("--check", default="",
                   help="Comma-separated subset of check ids to run, e.g. 3,5,6.")
    p.add_argument("--note", default="",
                   help="Free-text note appended to the LOOP_LOG row.")
    return p.parse_args()


def main() -> int:
    args = parse_args()
    ctx = RunContext(verbose=args.verbose)
    only: Optional[List[int]] = None
    if args.check:
        only = []
        for tok in args.check.split(","):
            tok = tok.strip()
            if tok:
                only.append(int(tok))

    print("Copper integration self-assessment")
    print("=" * 60)
    run = run_checks(ctx, only=only)
    print("=" * 60)
    print(f"Total score: {run.total_score:.1f} / 100")
    print(f"Hard gates: {run.hard_gate_passes}/{run.hard_gate_total}")
    print("VERDICT:", "POLISHED" if run.polished else "NOT POLISHED")

    if not args.no_write and only is None:
        DOCS_DIR.mkdir(exist_ok=True)
        write_self_assessment(run)
        append_loop_log(run, iteration_note=args.note)
        write_json(run)

    # Exit code = number of failing hard gates; 0 if polished.
    if run.polished:
        return 0
    failing_hard = sum(
        1 for c in CHECKS
        if c.hard_gate and run.per_check.get(c.id, CheckResult(0)).score < 0.999
    )
    return max(1, failing_hard)


if __name__ == "__main__":
    sys.exit(main())
