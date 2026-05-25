"""
copper_integration — headless harness for the kicad-copper plugin.

See ../docs/ARCHITECTURE.md and ../docs/DECISIONS.md (ADR-001) for why this
exists in Python while the production plugin is C++.

As of Phase 1 (docs/SYNC_PLAN.md), the protocol shapes are owned by the
`copper-2` repo's `copper.protocol` package. We re-export from there so
both repos validate against ONE definition. See path-injection block below.

Public API:
    - SchematicApi, FakeSchematicApi, CommitToken from .schematic_api
    - BackendClient, BackendError, BackendHttpError, BackendNetworkError,
      BackendSchemaError, Event, Stage, Message, PlanEvent, Done, ErrorEvent
      from .backend_client
    - StubBackend from .stub_backend
    - validate_response, ValidationError from .validators (shim → copper.protocol)
    - ApplyEngine, ApplyResult from .apply_engine
    - Controller, ControllerState from .controller
    - Settings, resolve_api_url, resolve_api_token from .settings
"""

from __future__ import annotations

import os as _os
import sys as _sys

# Inject the copper-2 sibling into sys.path so `copper.protocol` imports work.
# In a deployed setup this would be a git submodule under third_party/copper
# pinned to a SHA (see docs/DECISIONS.md ADR-009). For local dev, we look for
# copper-2 at three locations, in order:
#   1. $COPPER_REPO_PATH if set
#   2. <kicad-copper>/third_party/copper (the submodule location)
#   3. <kicad-copper>/../copper-2 (sibling dir, common dev layout)
def _bootstrap_copper_protocol() -> str | None:
    candidates: list[str] = []
    env = _os.environ.get("COPPER_REPO_PATH")
    if env:
        candidates.append(env)
    here = _os.path.dirname(_os.path.dirname(_os.path.abspath(__file__)))
    candidates.append(_os.path.join(here, "third_party", "copper"))
    candidates.append(_os.path.normpath(_os.path.join(here, "..", "copper-2")))
    for c in candidates:
        if _os.path.isdir(_os.path.join(c, "copper", "protocol")):
            if c not in _sys.path:
                # APPEND so the kicad-copper tree's own `tests/` package (and
                # any other local packages) take precedence — copper-2 also
                # has a top-level `tests/`, which would shadow ours if we
                # inserted at the head of sys.path.
                _sys.path.append(c)
            return c
    return None


_copper_root = _bootstrap_copper_protocol()
if _copper_root is None:
    raise ImportError(
        "Cannot find copper-2's `copper.protocol` package. Set "
        "$COPPER_REPO_PATH, add copper-2 as a sibling directory, or wire a "
        "git submodule at third_party/copper. See docs/DECISIONS.md ADR-009."
    )

# Re-export the protocol version constant for back-compat. Source of truth
# is copper.protocol.PROTOCOL_VERSION.
from copper.protocol import PROTOCOL_VERSION  # noqa: E402

__version__ = "0.1.0"

from .schematic_api import (
    SchematicApi,
    FakeSchematicApi,
    CommitToken,
    SchematicError,
    Symbol,
    Wire,
    Label,
    Junction,
)
from .validators import (
    validate_response,
    validate_operation,
    ValidationError,
)
from .backend_client import (
    BackendClient,
    BackendError,
    BackendHttpError,
    BackendNetworkError,
    BackendSchemaError,
    BackendUnauthorized,
    BackendRateLimited,
    Event,
    Stage,
    Message,
    PlanEvent,
    Done,
    ErrorEvent,
    Transport,
)
from .stub_backend import StubBackend
from .apply_engine import ApplyEngine, ApplyResult, ApplyError
from .controller import Controller, ControllerState, ControllerObserver
from .settings import Settings, resolve_api_url, resolve_api_token

__all__ = [
    "PROTOCOL_VERSION",
    "SchematicApi",
    "FakeSchematicApi",
    "CommitToken",
    "SchematicError",
    "Symbol",
    "Wire",
    "Label",
    "Junction",
    "validate_response",
    "validate_operation",
    "ValidationError",
    "BackendClient",
    "BackendError",
    "BackendHttpError",
    "BackendNetworkError",
    "BackendSchemaError",
    "BackendUnauthorized",
    "BackendRateLimited",
    "Event",
    "Stage",
    "Message",
    "PlanEvent",
    "Done",
    "ErrorEvent",
    "Transport",
    "StubBackend",
    "ApplyEngine",
    "ApplyResult",
    "ApplyError",
    "Controller",
    "ControllerState",
    "ControllerObserver",
    "Settings",
    "resolve_api_url",
    "resolve_api_token",
]
