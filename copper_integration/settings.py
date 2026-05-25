"""Settings resolution: env var > user setting > default. ADR-005."""

from __future__ import annotations

import os
import warnings
from dataclasses import dataclass
from typing import Optional


DEFAULT_API_URL = "https://api.copper.dev"


@dataclass
class Settings:
    """Stand-in for EESCHEMA_SETTINGS::m_Copper on the C++ side. Tests build
    these by hand; production reads from the wx settings JSON.

    Mirror the field naming used in C++ so cross-language search works."""

    api_url: str = DEFAULT_API_URL
    saved_token: Optional[str] = None
    timeout_seconds: float = 60.0  # for non-streaming endpoints
    stream_idle_timeout_seconds: float = 120.0  # for SSE


def resolve_api_url(settings: Optional[Settings] = None) -> str:
    """env > settings > default."""
    env = os.environ.get("COPPER_API_URL")
    if env:
        return env
    if settings is not None and settings.api_url:
        return settings.api_url
    return DEFAULT_API_URL


def resolve_api_token(settings: Optional[Settings] = None) -> Optional[str]:
    """env (dev override) > settings.saved_token > None.

    If the env var is used, emit a one-time warning since this bypasses the
    OS keychain (per ADR-005 + SECURITY.md §1)."""
    env = os.environ.get("COPPER_API_TOKEN")
    if env:
        _warn_env_token_once()
        return env
    if settings is not None and settings.saved_token:
        return settings.saved_token
    return None


_warned_env_token = False


def _warn_env_token_once() -> None:
    global _warned_env_token
    if _warned_env_token:
        return
    _warned_env_token = True
    warnings.warn(
        "Using COPPER_API_TOKEN env var bypasses the OS keychain. "
        "Intended for development only.",
        UserWarning,
        stacklevel=3,
    )


def reset_env_warning_for_tests() -> None:
    """Test helper: clear the once-flag so a second test can observe the warning."""
    global _warned_env_token
    _warned_env_token = False
