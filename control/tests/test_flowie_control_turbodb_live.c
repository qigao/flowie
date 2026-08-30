#include "flowie_control_store_internal.h"

#include "orm.h"
#include "tinytest.h"
#include "turbo_error.h"
#include "turbo_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void live_request(char *out, size_t capacity, const char *domain_id,
                         const char *operation) {
  const int written = snprintf(out, capacity, "%s-%s", domain_id, operation);
  check_true(written > 0 && (size_t)written < capacity);
}

spec("Flowie Control TurboDB live contract") {
  it("persists domain group role user and policy state through PostgreSQL") {
    const char *conninfo = getenv("FLOWIE_TURBODB_TEST_CONNINFO");
    char domain_id[64];
    char request_id[96];
    char rule_line[256];
    orm_config_t database;
    orm_option_t option;
    flowie_control_store_config_t store_config = FLOWIE_CONTROL_STORE_CONFIG_INIT;
    flowie_control_store_t *store = NULL;
    flowie_control_command_result_t result = FLOWIE_CONTROL_COMMAND_RESULT_INIT;
    flowie_control_domain_create_command_t domain = FLOWIE_CONTROL_DOMAIN_CREATE_COMMAND_INIT;
    flowie_control_user_create_command_t user = FLOWIE_CONTROL_USER_CREATE_COMMAND_INIT;
    flowie_control_credential_issue_command_t credential_issue =
        FLOWIE_CONTROL_CREDENTIAL_ISSUE_COMMAND_INIT;
    flowie_control_generated_credential_t credential =
        FLOWIE_CONTROL_GENERATED_CREDENTIAL_INIT;
    flowie_control_credential_verify_result_t credential_verified =
        FLOWIE_CONTROL_CREDENTIAL_VERIFY_RESULT_INIT;
    flowie_control_group_create_command_t group = FLOWIE_CONTROL_GROUP_CREATE_COMMAND_INIT;
    flowie_control_membership_add_command_t membership =
        FLOWIE_CONTROL_MEMBERSHIP_ADD_COMMAND_INIT;
    flowie_control_role_create_command_t role = FLOWIE_CONTROL_ROLE_CREATE_COMMAND_INIT;
    flowie_control_user_role_add_command_t user_role =
        FLOWIE_CONTROL_USER_ROLE_ADD_COMMAND_INIT;
    flowie_control_acl_document_t document = FLOWIE_CONTROL_ACL_DOCUMENT_INIT;
    flowie_control_policy_subject_rule_put_command_t rule =
        FLOWIE_CONTROL_POLICY_SUBJECT_RULE_PUT_COMMAND_INIT;
    flowie_control_policy_validation_t validation = FLOWIE_CONTROL_POLICY_VALIDATION_INIT;
    flowie_control_policy_publish_command_t publish = FLOWIE_CONTROL_POLICY_PUBLISH_COMMAND_INIT;
    flowie_control_policy_publish_result_t published =
        FLOWIE_CONTROL_POLICY_PUBLISH_RESULT_INIT;
    flowie_control_effective_groups_view_t groups =
        FLOWIE_CONTROL_EFFECTIVE_GROUPS_VIEW_INIT;
    flowie_control_effective_roles_view_t roles = FLOWIE_CONTROL_EFFECTIVE_ROLES_VIEW_INIT;
    flowie_security_policy_bundle_t bundle = FLOWIE_SECURITY_POLICY_BUNDLE_INIT;
    flowie_control_management_session_record_t session =
        FLOWIE_CONTROL_MANAGEMENT_SESSION_RECORD_INIT;
    flowie_control_management_session_record_t restored_session =
        FLOWIE_CONTROL_MANAGEMENT_SESSION_RECORD_INIT;
    uint64_t revision = 0u;
    uint64_t occurred_at = UINT64_C(4102440000);
    uint64_t session_nonce;

    if (!conninfo || !conninfo[0]) {
      check_true(0 && "FLOWIE_TURBODB_TEST_CONNINFO is required");
      return;
    }
    orm_config(&database);
    option.keyword = orm_view("conninfo");
    option.value = orm_view(conninfo);
    database.driver = orm_view("postgresql");
    database.options = &option;
    database.option_count = 1u;
    store_config.database = &database;
    check_equal(flowie_control_store_open(&store_config, &store), TURBO_OK);
    if (!store) return;
    check_equal(flowie_control_store_current_revision(store, &revision), TURBO_OK);

    (void)snprintf(domain_id, sizeof(domain_id), "live-%llu",
                   (unsigned long long)turbo_hrtime());
    live_request(request_id, sizeof(request_id), domain_id, "domain");
    domain.domain_id = domain_id;
    domain.actor = "live-test";
    domain.request_id = request_id;
    domain.expected_revision = revision;
    domain.occurred_at = occurred_at++;
    check_equal(flowie_control_store_domain_create(store, &domain, &result), TURBO_OK);

    live_request(request_id, sizeof(request_id), domain_id, "user");
    user.domain_id = domain_id;
    user.principal_id = "device";
    user.principal_type = "device";
    user.actor = "live-test";
    user.request_id = request_id;
    user.expected_revision = result.revision;
    user.occurred_at = occurred_at++;
    check_equal(flowie_control_store_user_create(store, &user, &result), TURBO_OK);

    live_request(request_id, sizeof(request_id), domain_id, "credential");
    credential_issue.domain_id = domain_id;
    credential_issue.principal_id = "device";
    credential_issue.actor = "live-test";
    credential_issue.request_id = request_id;
    credential_issue.expected_revision = result.revision;
    credential_issue.occurred_at = occurred_at++;
    check_equal(flowie_control_store_credential_generate(store, &credential_issue, &credential),
                TURBO_OK);
    check_equal(flowie_control_store_credential_verify(store, domain_id, "device",
                                                       credential.token, credential.token_size,
                                                       &credential_verified),
                TURBO_OK);
    result.revision = credential.revision;
    flowie_control_generated_credential_wipe(&credential);

    live_request(request_id, sizeof(request_id), domain_id, "group");
    group.domain_id = domain_id;
    group.group_id = "operators";
    group.actor = "live-test";
    group.request_id = request_id;
    group.expected_revision = result.revision;
    group.occurred_at = occurred_at++;
    check_equal(flowie_control_store_group_create(store, &group, &result), TURBO_OK);

    live_request(request_id, sizeof(request_id), domain_id, "membership");
    membership.domain_id = domain_id;
    membership.principal_id = "device";
    membership.group_id = "operators";
    membership.actor = "live-test";
    membership.request_id = request_id;
    membership.expected_revision = result.revision;
    membership.occurred_at = occurred_at++;
    check_equal(flowie_control_store_membership_add(store, &membership, &result), TURBO_OK);

    live_request(request_id, sizeof(request_id), domain_id, "role");
    role.domain_id = domain_id;
    role.role_id = "reader";
    role.actor = "live-test";
    role.request_id = request_id;
    role.expected_revision = result.revision;
    role.occurred_at = occurred_at++;
    check_equal(flowie_control_store_role_create(store, &role, &result), TURBO_OK);

    live_request(request_id, sizeof(request_id), domain_id, "user-role");
    user_role.domain_id = domain_id;
    user_role.principal_id = "device";
    user_role.role_id = "reader";
    user_role.actor = "live-test";
    user_role.request_id = request_id;
    user_role.expected_revision = result.revision;
    user_role.occurred_at = occurred_at++;
    check_equal(flowie_control_store_user_role_add(store, &user_role, &result), TURBO_OK);

    check_equal(flowie_control_store_effective_groups(store, domain_id, "device", &groups),
                TURBO_OK);
    check_equal(groups.group_count, 1u);
    check_equal(groups.groups[0], "operators");
    check_equal(flowie_control_store_effective_roles(store, domain_id, "device", &roles),
                TURBO_OK);
    check_equal(roles.role_count, 1u);
    check_equal(roles.roles[0], "reader");

    (void)snprintf(rule_line, sizeof(rule_line),
                   "user device allow {\n  read topic %s/groups/operators/devices/%%u/event\n}",
                   domain_id);
    check_equal(flowie_control_acl_parse(rule_line, strlen(rule_line), &document), TURBO_OK);
    live_request(request_id, sizeof(request_id), domain_id, "rule");
    rule.domain_id = domain_id;
    rule.ordinal = 10u;
    rule.document = &document;
    rule.actor = "live-test";
    rule.request_id = request_id;
    rule.expected_revision = result.revision;
    rule.occurred_at = occurred_at++;
    check_equal(flowie_control_store_policy_subject_rule_put(store, &rule, &result), TURBO_OK);
    check_equal(flowie_control_store_policy_validate(store, domain_id, &validation), TURBO_OK);
    check_equal(validation.rule_count, 2u);

    live_request(request_id, sizeof(request_id), domain_id, "publish");
    publish.domain_id = domain_id;
    publish.actor = "live-test";
    publish.request_id = request_id;
    publish.expected_revision = result.revision;
    publish.occurred_at = occurred_at++;
    publish.expires_at = UINT64_C(4102444800);
    check_equal(flowie_control_store_policy_publish(store, &publish, &published), TURBO_OK);
    check_equal(published.policy_version, 1u);
    check_equal(flowie_control_store_policy_bundle_load(store, domain_id, 1u, &bundle), TURBO_OK);
    check_equal(bundle.policy_version, 1u);
    check_equal(bundle.rule_count, 2u);
    flowie_control_store_policy_bundle_release(&bundle);

    session_nonce = turbo_hrtime();
    for (size_t index = 0u; index < sizeof(session.token_digest); ++index)
      session.token_digest[index] =
          (uint8_t)((session_nonce >> ((index % sizeof(session_nonce)) * 8u)) ^ index);
    (void)snprintf(session.domain_id, sizeof(session.domain_id), "%s", domain_id);
    (void)snprintf(session.principal_id, sizeof(session.principal_id), "%s", "device");
    memset(session.csrf, 'c', FLOWIE_CONTROL_MANAGEMENT_SESSION_CSRF_SIZE);
    session.csrf[FLOWIE_CONTROL_MANAGEMENT_SESSION_CSRF_SIZE] = '\0';
    session.expires_at = occurred_at + 600u;
    check_equal(flowie_control_store_management_session_issue(store, &session, 8u, 2u,
                                                              occurred_at),
                TURBO_OK);
    flowie_control_store_destroy(store);
    store = NULL;

    check_equal(flowie_control_store_open(&store_config, &store), TURBO_OK);
    check_equal(flowie_control_store_management_session_resolve(
                    store, session.token_digest, occurred_at + 1u, &restored_session),
                TURBO_OK);
    check_equal(restored_session.domain_id, domain_id);
    check_equal(restored_session.principal_id, "device");
    check_equal(restored_session.csrf, session.csrf);
    check_equal(flowie_control_store_management_session_revoke(store, session.token_digest),
                TURBO_OK);
    flowie_control_store_destroy(store);
  }
}
