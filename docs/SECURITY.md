# SECURITY — the §6 baseline (and what the harness verifies)

## 1. Secrets

- **OAuth access + refresh tokens** live in the OS keychain via
  `SECURE_TOKEN_STORE` (`common/oauth/secure_token_store.cpp`). Provider id
  is `copper`. Never on disk in cleartext, never logged, never sent except
  in the `Authorization: Bearer` header.
- **The `COPPER_API_TOKEN` env var** is a documented dev-only override
  (ADR-005). The plugin logs a one-time warning when this path is taken so
  the user knows they're bypassing the keychain.
- **No tokens, no real URLs, no real credentials** appear anywhere in this
  repo. Harness check 1 fails if a high-entropy string matching token
  shapes is committed.

## 2. Transport

- TLS via libcurl (system trust store).
- The C++ client honors the system cert chain; we don't disable verification.
- Local-backend dev (`http://localhost:…`) is allowed when the user
  explicitly configures it. Production hosted URL is HTTPS only.

## 3. Input validation

Untrusted input enters via:
1. Backend responses (`/api/v1/*`).
2. SSE event payloads.
3. HTTP requests to the local bridge (`HTTP_BRIDGE` on 127.0.0.1).

For (1) and (2): every response is validated per [PROTOCOL.md](PROTOCOL.md)
**before** any apply. The hard-rejects list:

- `protocol_version` missing or > 1.
- `success: false`.
- `operations` not an array.
- Unknown `op.type`.
- Missing `lib_id` / `reference` on `PLACE_COMPONENT`.
- Coordinates out of `[-1e9, 1e9]`.
- Duplicate `reference`s within one plan.
- Zero-length `ADD_WIRE`.
- Unknown `label_type` on `ADD_LABEL`.
- Empty `net_name` on `ADD_POWER_SYMBOL`.

For (3): the bridge only listens on `127.0.0.1` (not `0.0.0.0`). Treated as
a local-trust surface — same trust level as the user's keyboard.

## 4. Atomicity

- One `SCH_COMMIT` per applied plan (ADR-003).
- If validation fails: nothing is constructed, nothing is committed.
- If construction throws partway: `SCH_COMMIT` destructor cleans up without
  pushing → no partial state on the schematic.
- A successful `Push()` is the single point where the schematic changes.
- One `ACTIONS::undo` reverses it.

Harness check 6 asserts this with `FakeSchematicApi`:
- Snapshot `serialize()` before the apply.
- Force a failure (raise from a registered hook) after N successful ops.
- Assert `serialize()` is byte-identical to the snapshot.

## 5. Threading safety

- Network on `std::thread`, never touches wx widgets or `SCHEMATIC`.
- Schematic mutations on the main wx thread only.
- `CallAfter` is the only legal cross-thread bridge.
- Cancellation: `std::atomic<bool> m_cancelled` checked on each curl
  callback and at SSE event boundaries.

## 6. Dependency hygiene (GPL-clean)

- Plugin code: GPLv3 (matches KiCad).
- Plugin runtime deps: libcurl (M-IT-permissive), nlohmann/json (MIT), wx
  (LGPL+exceptions), Boost — all bundled by KiCad already, all
  GPL-compatible.
- Harness runtime deps: **stdlib only**.
- Harness dev deps: `ruff` (MIT) — optional.

Harness check would flag any non-GPL dep declared in `pyproject.toml` /
`requirements.txt`. We ship neither, so the check is "no requirements file
present" = pass.

## 7. Logging hygiene

- Tokens are never logged.
- Request bodies in debug logs are truncated and have `Authorization`
  redacted.
- Stack traces include file/line but not user prompt content unless the
  user opts into a debug bundle (off by default).

## 8. Failure modes we explicitly do NOT handle

- Backend serves a benign-looking but evil plan (e.g. lib_id pointing at
  a symbol that visually mimics a real one). The plugin is not a
  trust-the-prompt sandbox — the trust boundary is the OAuth-authenticated
  backend. If you don't trust your backend, don't sign in.
- Race conditions caused by the user manually editing the schematic
  *during* an apply. We disable the send button and ignore the small
  hazard window — fixing it properly requires holding a higher-level
  lock the existing fork doesn't expose. Document; defer.

## 9. Reporting

Security issues should be reported via the project's standard channel; do
not file public issues for any auth-related bug.
