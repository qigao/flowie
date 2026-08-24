# Flowie and CoroNet Debug Logging Implementation Plan

> Execute this plan in the current session while preserving the existing standalone-server and deployment edits.

**Goal:** Make CoroNet/Flowie diagnostics identifiable and bounded, expose server log/inflight configuration, upload a full Linux Debug build, and observe the reproduced load behavior on `root@eu`.

**Spec:** Current user request; repository constraints in `AGENTS.md`; deployment contract in `flowie/LINUX_REMOTE_TEST_RUNBOOK.md`.

**Architecture:** The standalone server owns the process-wide tlog lifecycle and selects the runtime level. CoroNet and Flowie library code only publish through the already-configured default logger. The initial diagnostic build kept transport close events at DEBUG and emitted Flowie resource isolation as a sampled WARN without protocol identity or payload fields. Subsequent scale evidence showed that normal close events dominate log volume; the long-running replacement policy is defined in `docs/FLOWIE_LONG_RUNNING_LOGGING.md`.

**Tech Stack:** C11, TurboUtils tlog, CoroNet, Flowie MQTT endpoint, TinyTest/CTest, CMake Presets, Docker BuildKit, Linux native runtime.

---

### Task 1: Lock diagnostic contracts with failing tests

**Files:**
- Modify: `server/tests/CMakeLists.txt`
- Modify: `flowie/tests/test_flowie_endpoint.c`
- Create: `../turbonet/CoroNet/tests/test_coronet_logging.c`

1. Require `flowie_server --check --log-level DEBUG` to emit a named DEBUG effective-configuration record.
2. Capture the slow-subscriber isolation event and require a named, redacted, sampled Flowie warning.
3. Trigger an epoll stream close and require the CoroNet component name to be present.
4. Run the focused tests before production changes and retain the expected failures.

### Task 2: Implement bounded named logging

**Files:**
- Modify: `server/flowie_server.c`
- Modify: `flowie/src/flowie_endpoint.c`
- Modify: `../turbonet/CoroNet/src/turbo_stream_epoll.c`

1. Initialize a process logger with full Debug source formatting, explicit sink ownership, and shutdown flush/destroy.
2. Add validated `--log-level` and `--max-inflight` options and log the effective listener/session limits.
3. Emit sampled Flowie slow-subscriber warnings at the resource-consumption boundary.
4. Add a stable `CoroNet.epoll` component to terminal epoll diagnostics without raising their level.

### Task 3: Keep deployment configuration consistent

**Files:**
- Modify: `deploy/server/docker-entrypoint.sh`
- Modify: `deploy/server/Dockerfile`
- Modify: `deploy/server/compose.yml`
- Modify: `deploy/server/README.md`
- Modify: `deploy/server/tests/test-docker-entrypoint.sh`
- Modify: `flowie/LINUX_REMOTE_TEST_RUNBOOK.md`

1. Forward `FLOWIE_LOG_LEVEL` and `FLOWIE_MAX_INFLIGHT_PER_SESSION` to the standalone server.
2. Parameterize the existing multi-repository image build so Debug dependencies and Flowie use their public Debug install presets.
3. Document Debug build, native extraction, runtime log inspection, and bounded-log checks.

### Task 4: Verify locally and observe on EU

**Files:**
- Evidence: `/root/dev/runs/20260824T054750Z/artifacts/flowie-debug-logging/`

1. Run focused CoroNet and Flowie tests, then adjacent server/endpoint regressions.
2. Upload current source archives and build the Linux Debug image under `/root/dev`.
3. Extract the Debug executable and libraries, replace only the run-scoped native Flowie process, and run focused 0/1/2 plus multi-client load.
4. Inspect level/component/source formatting, event counts, tlog drop indicators, sensitive-field matches, and PostgreSQL/nginx health.

### Observed outcome and superseding policy

The 384-client tier emitted 1,320 lines. Normal CoroNet close-path diagnostics contributed 1,192 lines (90.3%) and
213,192 bytes (86.9%); all 424 nonzero read statuses were normal `TURBO_EOF`. This invalidates the initial assumption
that retaining every transport close at DEBUG is sufficiently bounded for a long-running process. Preserve this plan as
the diagnostic-build history, but use `docs/FLOWIE_LONG_RUNNING_LOGGING.md` for production event ownership, budgets,
accuracy rules, and staged changes.
