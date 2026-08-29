# Policy Dry-Run Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an authoritative, side-effect-free Flowie policy dry-run API and expose it through PicImpact SuperAdmin backend and frontend.

**Architecture:** Flowie Control owns authoritative policy semantics. `control.policy.dry_run` reads one repository snapshot, overlays bounded candidate `put`/`delete` changes in memory, returns structured diagnostics, and never writes draft, audit, revision, or published policy state. PicImpact expands the user-supplied device ACL template for enabled station/printer principals, plans ordinals from the current Flowie draft, calls the RPC through its backend-only Management session, and renders the returned result in the SuperAdmin UI.

**Tech Stack:** C11, TurboDB repository port, TurboHTTP JSON-RPC, TinyTest, CMake Presets, Go/Gin/GORM, React/TypeScript/Vitest.

**Spec:** `flowie/MANAGEMENT_RPC_API.md`

## Global Constraints

- `control.policy.validate` remains unchanged and continues to validate the stored draft.
- `control.policy.dry_run` requires `policy_admin`; security/system administrators retain inherited authorization.
- Structural JSON errors return JSON-RPC `-32602`; candidate policy failures return a successful result with `valid:false` and diagnostics.
- The dry-run path performs no draft, audit, revision, session, or published-policy mutation.
- Candidate change count is bounded at 256; final expanded rules remain bounded by `FLOWIE_SECURITY_MAX_RULES`.
- PicImpact frontend never receives or sends the Flowie Management credential/session; it calls only the protected PicImpact REST endpoint.
- No new dependencies or fallback protocol are introduced.

---

### Task 1: Flowie repository dry-run contract

**Files:**
- Modify: `control/flowie_control_store_internal.h`
- Modify: `control/flowie_control_repository_internal.h`
- Modify: `control/flowie_control_repository.c`
- Modify: `control/tests/test_flowie_control_store.c`
- Modify: `control/tests/test_flowie_control_repository.c`

**Interfaces:**
- Consumes: canonical ACL documents and the existing draft stored under one Domain.
- Produces: `flowie_control_store_policy_dry_run(...)`, candidate change/diagnostic/result structs, and repository `policy->dry_run`.

- [ ] **Step 1: Write failing store and repository contract tests**

```c
flowie_control_policy_dry_run_change_t changes[] = {
    FLOWIE_CONTROL_POLICY_DRY_RUN_PUT(10u, "user device-7 allow {\n  read topic root-a/new/#\n}"),
};
flowie_control_policy_dry_run_result_t dry_run =
    FLOWIE_CONTROL_POLICY_DRY_RUN_RESULT_INIT(diagnostics, 4u);
check_equal(flowie_control_store_policy_dry_run(store, "root-a", changes, 1u, &dry_run),
            TURBO_OK);
check_true(dry_run.valid);
check_equal(dry_run.rule_count, 2u);
```

- [ ] **Step 2: Run the focused test and verify RED**

Run: build the existing `test_flowie_control_store` and `test_flowie_control_repository` targets through the active user preset, then filter CTest to those executables.

Expected: compilation fails because the dry-run contract does not exist.

- [ ] **Step 3: Implement the bounded read-only overlay**

```c
int flowie_control_store_policy_dry_run(
    flowie_control_store_t *store, const char *domain_id,
    const flowie_control_policy_dry_run_change_t *changes, size_t change_count,
    flowie_control_policy_dry_run_result_t *result);
```

The implementation validates every candidate canonical document, streams the current draft once, excludes replaced/deleted typed subjects, checks delete targets and ordinal collisions, aggregates expanded/deny counts, and commits a SELECT-only transaction. Candidate failures append typed diagnostics and return `TURBO_OK` with `valid == 0`; repository failures return the existing Turbo error code.

- [ ] **Step 4: Run focused store/repository tests and verify GREEN**

Expected: candidate replacement is valid, invalid subjects produce diagnostics, and revision/audit/draft snapshots remain byte-for-byte unchanged.

- [ ] **Step 5: Commit the repository contract slice**

```text
feat(control): add read-only policy dry-run repository contract
```

### Task 2: Flowie Management RPC and documentation

**Files:**
- Modify: `control/flowie_control_management_service_internal.h`
- Modify: `control/flowie_control_management_service.c`
- Modify: `control/flowie_control_management_rpc.c`
- Modify: `control/tests/flowie_control_management_repository_contract.h`
- Modify: `control/tests/test_flowie_control_management_rpc.c`
- Modify: `flowie/MANAGEMENT_RPC_API.md`
- Modify: `flowie/THIRD_PARTY_INTEGRATION.md`
- Modify: `flowie/ACL_GRAMMAR.md`

**Interfaces:**
- Consumes: repository `policy->dry_run` and JSON candidate changes.
- Produces: JSON-RPC method `control.policy.dry_run`.

- [ ] **Step 1: Write failing Management service/RPC tests**

```json
{
  "jsonrpc": "2.0",
  "method": "control.policy.dry_run",
  "params": {
    "domain_id": "root-a",
    "changes": [{
      "operation": "put",
      "subject_kind": "user",
      "subject_id": "device-7",
      "ordinal": 10,
      "connection": "allow",
      "entries": [{"effect":"allow","access":"read","topic":"root-a/new/#"}]
    }]
  },
  "id": 1
}
```

Assert policy administrators receive `valid:true`, viewers receive `-32003`, unknown fields receive `-32602`, semantic failures receive `valid:false`, and the CoroNet owner remains responsive while repository validation runs on the policy executor.

- [ ] **Step 2: Run the focused Management RPC test and verify RED**

Expected: method-not-found or missing symbols.

- [ ] **Step 3: Implement RPC parsing, async execution, and result serialization**

```json
{
  "valid": false,
  "store_revision": 42,
  "rule_count": 0,
  "deny_rule_count": 0,
  "diagnostics": [{
    "code": "subject_not_found",
    "change_index": 0,
    "subject_kind": "user",
    "subject_id": "device-7",
    "field": "subject_id",
    "message": "Policy subject does not exist"
  }]
}
```

Deep-copy candidate strings before handing work to the policy executor so request JSON may be released after timeout without use-after-free.

- [ ] **Step 4: Run focused Management tests and verify GREEN**

- [ ] **Step 5: Update the three public integration documents**

Document parameters, permissions, normal invalid-policy results, JSON-RPC errors, bounds, and the no-state-change guarantee.

- [ ] **Step 6: Commit the RPC slice**

```text
feat(control): expose policy dry-run management RPC
```

### Task 3: PicImpact backend dry-run adapter

**Files:**
- Modify: `backend/internal/services/mqttsync/flowie.go`
- Modify: `backend/internal/services/mqttsync/rules.go`
- Modify: `backend/internal/services/mqttsync/service.go`
- Modify: `backend/internal/services/mqttsync/flowie_test.go`
- Modify: `backend/internal/services/mqttsync/service_test.go`
- Modify: `backend/internal/api/mqtt_service_account.go`
- Modify: `backend/internal/api/mqtt_service_account_test.go`
- Modify: `backend/cmd/server/main.go`
- Modify: `docs/flowie-auth-acl-integration.md`

**Interfaces:**
- Consumes: Flowie `control.policy.dry_run` and the user-supplied template.
- Produces: `POST /api/mqtt/service-account/acl-template/dry-run`.

- [ ] **Step 1: Write failing client/service/API tests**

```go
request := httptest.NewRequest(http.MethodPost,
    "/api/mqtt/service-account/acl-template/dry-run",
    strings.NewReader(`{"template":"user {{.Username}} allow"}`))
```

Assert the backend expands enabled station/printer principals only, preserves existing typed-subject ordinals, allocates new ordinals from the configured start, sends one bounded `changes` array, does not persist the template or mark principals pending, maps semantic diagnostics to HTTP 200, maps unavailable Flowie to 503, and maps RPC/transport failures to 502.

- [ ] **Step 2: Run focused Go tests and verify RED**

Run: `go test ./internal/services/mqttsync ./internal/api -run 'DryRun' -count=1`

Expected: missing dry-run functions/routes.

- [ ] **Step 3: Implement the Flowie client contract**

```go
func (c *FlowieClient) DryRunPolicy(
    ctx context.Context, changes []flowiePolicyDryRunChange,
) (flowiePolicyDryRunResult, error)
```

Reuse Management login/session refresh and root-scope injection. Do not publish or call subject-rule writes.

- [ ] **Step 4: Implement template expansion and REST mapping**

Return `valid`, `validation_level`, `template_sha256`, summary counts, structured diagnostics, previews, and remote status/revision. Empty input means the post-clear environment/built-in template, not the currently persisted SuperAdmin override.

- [ ] **Step 5: Run focused Go tests and verify GREEN**

- [ ] **Step 6: Commit the backend slice**

```text
feat(mqtt): expose authoritative ACL template dry-run
```

### Task 4: PicImpact frontend validation workflow

**Files:**
- Modify: `frontend/src/types/flowie.ts`
- Modify: `frontend/src/pages/MqttServiceAccount.tsx`
- Modify: `frontend/src/pages/MqttServiceAccount.test.tsx`

**Interfaces:**
- Consumes: PicImpact ACL template dry-run REST result.
- Produces: SuperAdmin validation action, summary, previews, and diagnostics.

- [ ] **Step 1: Write the failing Vitest behavior test**

```tsx
await user.click(screen.getByRole("button", {name: "校验 ACL 模板"}));
expect(api.post).toHaveBeenCalledWith(
  "/mqtt/service-account/acl-template/dry-run",
  {template: editorValue},
);
expect(await screen.findByText("Policy subject does not exist")).toBeInTheDocument();
```

- [ ] **Step 2: Run the focused test and verify RED**

Run: `npm test -- src/pages/MqttServiceAccount.test.tsx`

Expected: validation button/result is absent.

- [ ] **Step 3: Implement the validation action and result rendering**

Clear stale results whenever editor content changes. Keep validate, save, and resync as separate actions. Render server diagnostics as data, not HTML.

- [ ] **Step 4: Run focused frontend tests and verify GREEN**

- [ ] **Step 5: Commit the frontend slice**

```text
feat(admin): add ACL template dry-run diagnostics
```

### Task 5: Cross-repository verification

**Files:**
- Verify only; no new files expected.

**Interfaces:**
- Consumes: completed Flowie and PicImpact branches.
- Produces: repeatable evidence for review and deployment.

- [ ] **Step 1: Run Flowie focused tests through its user preset**

List presets first, configure the selected user preset, build the relevant Control test targets, then run their CTest filters.

- [ ] **Step 2: Run the complete Flowie preset test suite**

Run the repository's documented user test preset without bypassing CMake presets.

- [ ] **Step 3: Run PicImpact backend verification**

```text
go test ./... -count=1
go vet ./...
```

- [ ] **Step 4: Run PicImpact frontend verification**

```text
npm test
npm run build
```

Run `npm run lint` and report the existing missing-ESLint-configuration infrastructure failure separately if it remains unchanged.

- [ ] **Step 5: Run repository hygiene checks**

Run `git diff --check`, inspect both worktrees, confirm `.codegraph/` and build outputs are not staged, and compare public documentation with the implemented payload.

- [ ] **Step 6: Commit any final documentation corrections and prepare both branches for PRs**

