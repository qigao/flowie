# Flowie-Control JWT/JWKS Authenticator Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a fail-closed JWT/JWKS external authenticator to Flowie-Control without copying cjwt, blocking the CoroNet owner lane, or permitting cross-Domain identity mapping.

**Architecture:** Flowie-Control continues to own authentication and local principal mapping. A new provider fetches a bounded JWKS document over coroutine-based HTTPS, validates and installs one immutable key-set snapshot, and offloads JWKS parsing plus JWT signature/claim verification to a bounded `turbo_threadpool`. Every trusted assertion carries an exact Domain binding before the existing repository-backed mapper resolves local roles and groups.

**Tech Stack:** C11, CoroNet, TurboHTTP HttpClient, `TurboHttp::Cjwt`, TurboParser JSON, TurboUtils thread pool/synchronization, TinyTest, CMake Presets.

**Spec:** [Flowie-Control JWT/JWKS authentication](../../FLOWIE_CONTROL_JWT_JWKS_AUTH.md)

---

### Task 1: Specify the security and concurrency contract

**Files:**
- Create: `docs/FLOWIE_CONTROL_JWT_JWKS_AUTH.md`
- Modify: `docs/superpowers/plans/2026-09-01-flowie-control-jwt-jwks-authenticator.md`

- [x] Document trusted inputs, exact claim checks, allowed algorithms, key selection, Domain binding, cache ownership, refresh state, bounded work, shutdown, migration, and rollback.
- [x] Record why `TurboHttp::Cjwt` is linked from the installed package rather than copied from TurboHTTP.

### Task 2: Make external assertions Domain-bound

**Files:**
- Modify: `control/flowie_control_external_authenticator_internal.h`
- Modify: `control/flowie_control_external_authenticator.c`
- Modify: `control/flowie_control_auth_service.c`
- Modify: `control/flowie_control_external_https_authenticator.c`
- Modify: `control/tests/test_flowie_control_external_authenticator.c`
- Modify: `control/tests/test_flowie_control_auth_service.c`
- Modify: `control/tests/test_flowie_control_external_https_authenticator.c`

- [x] Add failing tests proving a missing or mismatched assertion Domain is rejected.
- [x] Add an explicit Domain-binding capability and assertion field.
- [x] Require an exact expected-Domain match when the entry already has a target; otherwise use only the trusted assertion Domain for mapping.
- [x] Update the reserved HTTPS assertion schema to return `domain_id`.
- [x] Remove the external-auth service gate only after the Domain invariant is enforced.
- [x] Run the three focused external-auth tests.

### Task 3: Implement the bounded JWT/JWKS verifier

**Files:**
- Create: `control/flowie_control_jwt_jwks_authenticator_internal.h`
- Create: `control/flowie_control_jwt_jwks_authenticator.c`
- Create: `control/tests/test_flowie_control_jwt_jwks_authenticator.c`
- Modify: `control/CMakeLists.txt`

- [x] Add failing tests for a valid signed token and for wrong issuer, audience, Domain, subject, time window, algorithm, `kid`, key use, duplicate `kid`, account state, revision, and signature.
- [x] Parse a bounded JWKS snapshot with `cjwt_jwks_parse`, reject symmetric/private/duplicate/incompatible keys, and require exact configured asymmetric algorithm metadata.
- [x] Verify with `cjwt_decode_with_jwk` using no insecure options and convert only allowlisted claims to a Domain-bound assertion.
- [x] Copy request token/identity/Domain into each worker job; use reference-counted job completion so timeout cannot create borrowed-pointer use-after-free.
- [x] Use `turbo_threadpool_try_submit`; report queue saturation as `TURBO_EBUSY` and deadline expiry as `TURBO_ETIMEDOUT`.
- [x] Run the focused JWT/JWKS test executable.

### Task 4: Add coroutine HTTPS JWKS refresh

**Files:**
- Modify: `control/flowie_control_jwt_jwks_authenticator_internal.h`
- Modify: `control/flowie_control_jwt_jwks_authenticator.c`
- Modify: `control/tests/test_flowie_control_jwt_jwks_authenticator.c`

- [ ] Extend the current loopback HTTPS initial-fetch/content-type tests with explicit status/size/refresh-race fault injection.
- [x] Fetch only the configured HTTPS origin with redirects and retries disabled, strict peer verification, bounded headers/body, and configured timeout.
- [x] Keep exactly one immutable parsed JWKS snapshot under a read/write lock; only a fully parsed snapshot may replace it.
- [x] Fail closed when no current snapshot exists; do not accept stale keys after the configured refresh deadline.
- [x] Destroy only after request admission stops and the worker pool has drained.

### Task 5: Wire configuration and runtime composition

**Files:**
- Modify: `control/flowie_control_config_internal.h`
- Modify: `control/flowie_control_config.c`
- Modify: `control/flowie_control_runtime.c`
- Modify: `control/CMakeLists.txt`
- Modify: `control/tests/test_flowie_control_config.c`
- Modify: `control/tests/test_flowie_control_runtime.c`

- [x] Add parser/runtime validation tests for `auth.jwt_jwks`, strict field rejection, HTTPS-only URL, exact supported algorithm, bounds, TLS fields, and mutual exclusion with `external_https`.
- [x] Add config fields for URL, issuer, audience, subject type, algorithm, refresh interval, clock skew, response/key/token limits, executor bounds, and TLS CA.
- [x] Compose the JWT/JWKS provider and existing subject mapper when enabled; keep local password and external HTTPS behavior unchanged when absent.
- [x] Disable the endpoint-level executor only for providers that already own their bounded verification executor.
- [x] Run config and runtime tests.

### Task 6: Document deployment and verify regressions

**Files:**
- Modify: `flowie/examples/flowie-control.yml`
- Modify: `deploy/server/control.yml.example`
- Modify: `docs/FLOWIE_CONTROL_JWT_JWKS_AUTH.md`

- [x] Document the MQTT method/token mapping, required JWT claims, key rotation timing, failure behavior, and operational limits.
- [ ] Configure and build with the repository preset, then run focused tests followed by the control-plane security test label. The full build and focused tests pass; the label run is 24/25 because the unchanged `test_flowie_control_management_rpc` hits its existing CoroNet stack guard.
- [x] Inspect `git diff --check`, CodeGraph affected tests, and `git status`; preserve the unrelated `server/flowie_worker_runtime.c` modification.
- [x] Record exact commands and any unverified integration risk in the handoff.
