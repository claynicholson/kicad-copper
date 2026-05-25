"""
copper_integration — headless harness for the kicad-copper plugin.

See ../docs/ARCHITECTURE.md and ../docs/DECISIONS.md (ADR-001) for why this
exists in Python while the production plugin is C++.

Public API:
    - SchematicApi, FakeSchematicApi, CommitToken from .schematic_api
    - BackendClient, BackendError, BackendHttpError, BackendNetworkError,
      BackendSchemaError, Event, Stage, Message, PlanEvent, Done, ErrorEvent
      from .backend_client
    - StubBackend from .stub_backend
    - validate_response, ValidationError from .validators
    - ApplyEngine, ApplyResult from .apply_engine
    - Controller, ControllerState from .controller
    - Settings, resolve_api_url, resolve_api_token from .settings
"""

from __future__ import annotations

__version__ = "0.1.0"
PROTOCOL_VERSION = 1

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
