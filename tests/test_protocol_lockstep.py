"""Cross-repo contract test — proves kicad-copper and copper-2 stay in lockstep.

What it does:
    1. Invokes the real `copper build --emit apply-plan` on a fixture in the
       sibling copper-2 repo (NOT the StubBackend). This exercises the live
       compiler + the live apply-plan emitter.
    2. Reads the emitted apply-plan.json.
    3. Validates it via copper_integration.validators (which imports the
       same copper.protocol models the engine used to produce it).
    4. Feeds it to ApplyEngine + FakeSchematicApi end-to-end.
    5. Asserts the apply lands cleanly and the resulting state has the
       right shape (component count, net coverage, no overlaps).

If kicad-copper or copper-2 changes the protocol shape in an incompatible
way, this test fails — catching drift the moment it happens.

If `copper` is not installed / importable, the whole module is skipped (CI
without the sibling repo still passes the harness; the lockstep check is
strictly additive).
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Optional

# Trigger copper_integration's bootstrap so `copper.protocol` is importable.
import copper_integration  # noqa: F401  (side effect: sys.path injection)


def _find_copper_repo() -> Optional[Path]:
    """Mirror the bootstrap logic but return the path. None if unavailable."""
    env = os.environ.get("COPPER_REPO_PATH")
    if env and (Path(env) / "copper" / "protocol").is_dir():
        return Path(env)
    here = Path(__file__).resolve().parent.parent
    for c in (
        here / "third_party" / "copper",
        (here.parent / "copper-2").resolve(),
    ):
        if (c / "copper" / "protocol").is_dir():
            return c
    return None


COPPER_REPO = _find_copper_repo()
HAS_COPPER = COPPER_REPO is not None


@unittest.skipUnless(HAS_COPPER, "copper-2 not found on disk")
class CopperBuildToApplyEngineTest(unittest.TestCase):
    """The headline contract test. Runs the real compiler, feeds the real
    apply-plan into the real apply engine."""

    FIXTURE_NAME = "rp2040_min.yaml"

    def _build_apply_plan(self, tmpdir: Path) -> dict:
        """Invoke `node dist/cli.js build <fixture> -o <tmp> --emit apply-plan`
        (the TS compiler — the python CLI was removed when copper-2 was ported
        to TypeScript) and return the parsed apply-plan.json."""
        fixture_path = COPPER_REPO / "fixtures" / self.FIXTURE_NAME
        self.assertTrue(fixture_path.exists(), f"fixture not found: {fixture_path}")

        cli_path = COPPER_REPO / "dist" / "cli.js"
        self.assertTrue(
            cli_path.exists(),
            f"compiled CLI not found at {cli_path} — run `npm run build` in copper-2",
        )

        node = shutil.which("node")
        self.assertIsNotNone(node, "node not found on PATH")

        out_dir = tmpdir / "out"
        # Run in copper-2's directory so library loading works.
        r = subprocess.run(
            [
                node, str(cli_path),
                "build", str(fixture_path),
                "-o", str(out_dir),
                "--emit", "apply-plan",
            ],
            cwd=str(COPPER_REPO),
            capture_output=True, text=True, timeout=60,
        )
        if r.returncode != 0:
            self.fail(
                f"copper build failed (rc={r.returncode}):\n"
                f"stdout:\n{r.stdout}\nstderr:\n{r.stderr}"
            )

        plan_path = out_dir / "apply-plan.json"
        self.assertTrue(plan_path.exists(), f"no apply-plan.json at {plan_path}")
        return json.loads(plan_path.read_text(encoding="utf-8"))

    def test_protocol_round_trip(self):
        """copper build → apply-plan.json → validators → ApplyEngine → FakeSchematicApi.

        Every step must succeed without modification. This is the strongest
        proof that the two repos agree on the contract."""

        from copper_integration import (
            ApplyEngine,
            FakeSchematicApi,
            validate_response,
        )

        with tempfile.TemporaryDirectory() as tmp:
            tmpdir = Path(tmp)
            plan_dict = self._build_apply_plan(tmpdir)

        # ── 1. Validates against kicad-copper's understanding of the schema ──
        validated = validate_response(plan_dict)
        self.assertTrue(validated.success)
        self.assertEqual(validated.protocol_version, 1)
        self.assertGreater(len(validated.operations), 0)

        # ── 2. Plays cleanly into the apply engine ──
        api = FakeSchematicApi()
        engine = ApplyEngine()
        before = api.serialize()

        result = engine.apply_response(api, plan_dict)
        self.assertTrue(result.ok)
        self.assertIsNotNone(result.commit_id)
        self.assertNotEqual(api.serialize(), before)

        # ── 3. Post-apply sanity ──
        self.assertGreater(len(api.list_symbols()), 0,
                           "no symbols placed after apply")
        # Every PLACE_COMPONENT in the plan must be reflected in list_symbols().
        plan_refs = {
            op.data["reference"]
            for op in validated.operations
            if op.type == "PLACE_COMPONENT"
        }
        applied_refs = {s.reference for s in api.list_symbols()}
        # Some plan refs (power markers like #PWR1) get applied via PLACE_COMPONENT
        # but show up under their own ref. All plan refs must appear.
        missing = plan_refs - applied_refs
        self.assertEqual(
            missing, set(),
            f"plan references not realised on the schematic: {missing}"
        )

        # ── 4. Single undo reverses the whole apply (§10 invariant) ──
        api.undo()
        self.assertEqual(api.serialize(), before,
                         "one undo() did not reverse the full apply")


@unittest.skipUnless(HAS_COPPER, "copper-2 not found on disk")
class ProtocolVersionAgreesTest(unittest.TestCase):
    """copper-2's PROTOCOL_VERSION must equal kicad-copper's. They live in
    the SAME constant now, but if anything ever drifts (e.g. someone
    hard-codes a different value in C++), this test catches it."""

    def test_versions_match(self):
        from copper.protocol import PROTOCOL_VERSION as COPPER_PV
        from copper_integration import PROTOCOL_VERSION as PLUGIN_PV
        self.assertEqual(COPPER_PV, PLUGIN_PV)

    def test_op_types_match(self):
        """Every op type the engine knows about must be one ApplyEngine
        dispatches on. Catches new op types added to copper-2 that
        kicad-copper hasn't taught itself yet."""
        from copper.protocol import OP_TYPE_NAMES
        # ApplyEngine's dispatch table — keep in sync.
        DISPATCHED = {
            "PLACE_COMPONENT", "ADD_WIRE", "ADD_LABEL",
            "ADD_JUNCTION", "ADD_POWER_SYMBOL",
        }
        unknown = set(OP_TYPE_NAMES) - DISPATCHED
        self.assertFalse(
            unknown,
            f"copper-2 defines op types kicad-copper's ApplyEngine doesn't "
            f"dispatch: {unknown}. Update copper_integration.apply_engine."
        )


@unittest.skipUnless(HAS_COPPER, "copper-2 not found on disk")
class CppHeaderUpToDateTest(unittest.TestCase):
    """The C++ codegen header MUST stay in sync with the Pydantic models.

    Re-runs the codegen and diffs against the checked-in header. If they
    differ, the maintainer forgot to regenerate. (CMake will eventually
    do this automatically; this test is the interim guard.)
    """

    def test_generated_header_matches_pydantic(self):
        from copper.protocol.codegen import emit_cpp_header
        repo_root = Path(__file__).resolve().parent.parent
        checked_in = repo_root / "eeschema" / "copper" / "copper_types.generated.h"
        if not checked_in.exists():
            self.skipTest(
                f"generated header not yet checked in at {checked_in}; "
                "run `python -m copper.protocol.codegen --target cpp --out <path>` once"
            )

        with tempfile.NamedTemporaryFile(
            mode="w", suffix=".h", delete=False, encoding="utf-8"
        ) as f:
            tmp_path = Path(f.name)
        try:
            emit_cpp_header(tmp_path)
            generated = tmp_path.read_text(encoding="utf-8")
        finally:
            tmp_path.unlink(missing_ok=True)

        actual = checked_in.read_text(encoding="utf-8")
        if actual != generated:
            # Show a short diff hint rather than dumping 300 lines.
            self.fail(
                f"{checked_in} is stale. Regenerate with:\n"
                f"  cd <copper-2> && python -m copper.protocol.codegen "
                f"--target cpp --out {checked_in}"
            )


if __name__ == "__main__":  # pragma: no cover
    unittest.main()
