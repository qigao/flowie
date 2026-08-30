#include "flowie_control_acl_internal.h"
#include "flowie_control_data_transfer_internal.h"
#include "flowie_control_store_internal.h"
#include "flowie_control_test_turbodb.h"

#include "tinytest.h"
#include "turbo_error.h"

#include <stdlib.h>
#include <string.h>

static flowie_control_store_t *data_test_store_open(const char *path) {
  flowie_control_test_turbodb_t database;
  flowie_control_store_config_t config = FLOWIE_CONTROL_STORE_CONFIG_INIT;
  flowie_control_store_t *store = NULL;
  check_equal(flowie_control_test_turbodb_init(&database, path), 0);
  config.database = &database.config;
  check_equal(flowie_control_store_open(&config, &store), TURBO_OK);
  return store;
}

spec("Flowie Control Domain data transfer") {
  it("exports, dry-runs, and imports declarative Domain data without credentials") {
    static const char acl[] =
        "user device-7 allow {\n"
        "  read topic root-a/devices/%u/event\n"
        "}";
    char *source_path = tt_make_temp_file("flowie-data-source", ".sqlite3");
    char *target_path = tt_make_temp_file("flowie-data-target", ".sqlite3");
    char *rejected_target_path = tt_make_temp_file("flowie-data-rejected", ".sqlite3");
    char *busy_target_path = tt_make_temp_file("flowie-data-busy", ".sqlite3");
    char *archive_path = tt_make_temp_file("flowie-domain", ".db");
    flowie_control_store_t *source;
    flowie_control_store_t *target;
    flowie_control_store_t *rejected_target;
    flowie_control_store_t *busy_target;
    const flowie_control_repository_t *repository;
    flowie_control_command_result_t command_result = FLOWIE_CONTROL_COMMAND_RESULT_INIT;
    flowie_control_policy_publish_result_t publish_result =
        FLOWIE_CONTROL_POLICY_PUBLISH_RESULT_INIT;
    flowie_control_domain_create_command_t domain = FLOWIE_CONTROL_DOMAIN_CREATE_COMMAND_INIT;
    flowie_control_user_create_command_t user = FLOWIE_CONTROL_USER_CREATE_COMMAND_INIT;
    flowie_control_user_disable_command_t disable_user = FLOWIE_CONTROL_USER_DISABLE_COMMAND_INIT;
    flowie_control_group_create_command_t group = FLOWIE_CONTROL_GROUP_CREATE_COMMAND_INIT;
    flowie_control_membership_add_command_t membership =
        FLOWIE_CONTROL_MEMBERSHIP_ADD_COMMAND_INIT;
    flowie_control_role_create_command_t role = FLOWIE_CONTROL_ROLE_CREATE_COMMAND_INIT;
    flowie_control_user_role_add_command_t assignment =
        FLOWIE_CONTROL_USER_ROLE_ADD_COMMAND_INIT;
    flowie_control_policy_subject_rule_put_command_t rule =
        FLOWIE_CONTROL_POLICY_SUBJECT_RULE_PUT_COMMAND_INIT;
    flowie_control_policy_publish_command_t publish = FLOWIE_CONTROL_POLICY_PUBLISH_COMMAND_INIT;
    flowie_control_acl_document_t document = FLOWIE_CONTROL_ACL_DOCUMENT_INIT;
    flowie_control_data_transfer_result_t exported = FLOWIE_CONTROL_DATA_TRANSFER_RESULT_INIT;
    flowie_control_data_transfer_result_t imported = FLOWIE_CONTROL_DATA_TRANSFER_RESULT_INIT;
    flowie_control_user_view_t users[2] = {FLOWIE_CONTROL_USER_VIEW_INIT,
                                           FLOWIE_CONTROL_USER_VIEW_INIT};
    flowie_control_membership_view_t memberships[1] = {FLOWIE_CONTROL_MEMBERSHIP_VIEW_INIT};
    flowie_control_policy_status_t status = FLOWIE_CONTROL_POLICY_STATUS_INIT;
    uint64_t revision = 0u;
    size_t count = 0u;
    int more = 0;

    check_not_null(source_path);
    check_not_null(target_path);
    check_not_null(rejected_target_path);
    check_not_null(busy_target_path);
    check_not_null(archive_path);
    check_equal(tt_remove_file(archive_path), 0);
    source = data_test_store_open(source_path);
    target = data_test_store_open(target_path);
    rejected_target = data_test_store_open(rejected_target_path);
    busy_target = data_test_store_open(busy_target_path);
    repository = flowie_control_store_repository(source);

    domain.domain_id = "root-a";
    domain.actor = "test";
    domain.request_id = "data-domain";
    domain.occurred_at = 1u;
    check_equal(repository->user->domain_create(repository->ctx, &domain, &command_result),
                TURBO_OK);
    user.domain_id = "root-a";
    user.principal_id = "device-7";
    user.principal_type = "device";
    user.actor = "test";
    user.request_id = "data-user";
    user.expected_revision = 1u;
    user.occurred_at = 2u;
    check_equal(repository->user->create(repository->ctx, &user, &command_result), TURBO_OK);
    group.domain_id = "root-a";
    group.group_id = "operators";
    group.actor = "test";
    group.request_id = "data-group";
    group.expected_revision = 2u;
    group.occurred_at = 3u;
    check_equal(repository->group->create(repository->ctx, &group, &command_result), TURBO_OK);
    membership.domain_id = "root-a";
    membership.principal_id = "device-7";
    membership.group_id = "operators";
    membership.actor = "test";
    membership.request_id = "data-membership";
    membership.expected_revision = 3u;
    membership.occurred_at = 4u;
    check_equal(repository->group->membership_add(repository->ctx, &membership, &command_result),
                TURBO_OK);
    role.domain_id = "root-a";
    role.role_id = "reader";
    role.actor = "test";
    role.request_id = "data-role";
    role.expected_revision = 4u;
    role.occurred_at = 5u;
    check_equal(repository->role->create(repository->ctx, &role, &command_result), TURBO_OK);
    assignment.domain_id = "root-a";
    assignment.principal_id = "device-7";
    assignment.role_id = "reader";
    assignment.actor = "test";
    assignment.request_id = "data-assignment";
    assignment.expected_revision = 5u;
    assignment.occurred_at = 6u;
    check_equal(repository->role->assignment_add(repository->ctx, &assignment, &command_result),
                TURBO_OK);
    check_equal(flowie_control_acl_parse(acl, sizeof(acl) - 1u, &document), TURBO_OK);
    rule.domain_id = "root-a";
    rule.ordinal = 10u;
    rule.document = &document;
    rule.actor = "test";
    rule.request_id = "data-policy";
    rule.expected_revision = 6u;
    rule.occurred_at = 7u;
    check_equal(repository->policy->subject_rule_put(repository->ctx, &rule, &command_result),
                TURBO_OK);
    publish.domain_id = "root-a";
    publish.actor = "test";
    publish.request_id = "data-publish";
    publish.expected_revision = 7u;
    publish.occurred_at = 8u;
    publish.expires_at = 20000u;
    check_equal(repository->policy->publish(repository->ctx, &publish, &publish_result), TURBO_OK);
    user.principal_id = "device-disabled";
    user.request_id = "data-disabled-user";
    user.expected_revision = 8u;
    user.occurred_at = 9u;
    check_equal(repository->user->create(repository->ctx, &user, &command_result), TURBO_OK);
    disable_user.domain_id = "root-a";
    disable_user.principal_id = "device-disabled";
    disable_user.actor = "test";
    disable_user.request_id = "data-disable-user";
    disable_user.expected_revision = 9u;
    disable_user.occurred_at = 10u;
    check_equal(repository->user->disable(repository->ctx, &disable_user, &command_result),
                TURBO_OK);

    check_equal(flowie_control_data_export(repository, "root-a", archive_path, &exported),
                TURBO_OK);
    check_equal(exported.source_revision, 10u);
    check_equal(exported.user_count, 2u);
    check_equal(exported.membership_count, 1u);
    check_equal(exported.assignment_count, 1u);
    check_equal(exported.policy_rule_count, 1u);
    check_true(exported.policy_published);

    repository = flowie_control_store_repository(target);
    check_equal(repository->audit->revision(repository->ctx, &revision), TURBO_OK);
    check_equal(revision, 0u);
    check_equal(flowie_control_data_import(repository, archive_path, 1, &imported), TURBO_OK);
    check_false(imported.mutated);
    check_equal(repository->audit->revision(repository->ctx, &revision), TURBO_OK);
    check_equal(revision, 0u);

    imported = (flowie_control_data_transfer_result_t)FLOWIE_CONTROL_DATA_TRANSFER_RESULT_INIT;
    check_equal(flowie_control_data_import(repository, archive_path, 0, &imported), TURBO_OK);
    check_true(imported.mutated);
    check_equal(imported.user_count, 2u);
    check_equal(repository->user->list(repository->ctx, "root-a", NULL, users, 2u, &count, &more),
                TURBO_OK);
    check_equal(count, 2u);
    check_equal(users[0].principal_id, "device-7");
    check_true(users[0].enabled);
    check_equal(users[1].principal_id, "device-disabled");
    check_false(users[1].enabled);
    check_equal(repository->group->membership_list(repository->ctx, "root-a", NULL, NULL,
                                                    memberships, 1u, &count, &more),
                TURBO_OK);
    check_equal(count, 1u);
    check_equal(memberships[0].group_id, "operators");
    check_equal(repository->policy->status(repository->ctx, "root-a", &status), TURBO_OK);
    check_equal(status.policy_version, 1u);
    check_equal(status.expires_at, 20000u);

    revision = imported.target_revision;
    imported = (flowie_control_data_transfer_result_t)FLOWIE_CONTROL_DATA_TRANSFER_RESULT_INIT;
    check_equal(flowie_control_data_import(repository, archive_path, 0, &imported), TURBO_OK);
    check_false(imported.mutated);
    check_equal(imported.target_revision, revision);

    repository = flowie_control_store_repository(busy_target);
    domain.domain_id = "unrelated";
    domain.request_id = "unrelated-domain";
    domain.expected_revision = 0u;
    domain.occurred_at = 1u;
    check_equal(repository->user->domain_create(repository->ctx, &domain, &command_result),
                TURBO_OK);
    imported = (flowie_control_data_transfer_result_t)FLOWIE_CONTROL_DATA_TRANSFER_RESULT_INIT;
    check_equal(flowie_control_data_import(repository, archive_path, 1, &imported), TURBO_EBUSY);
    check_equal(repository->audit->revision(repository->ctx, &revision), TURBO_OK);
    check_equal(revision, 1u);

    {
      flowie_control_database_t *archive = NULL;
      check_equal(flowie_control_test_database_open(archive_path, &archive), FLOWIE_CONTROL_DB_OK);
      check_equal(flowie_control_database_exec(
                      archive,
                      "UPDATE flowie_control_data_policy_rule "
                      "SET rule_document=rule_document || char(10)",
                      NULL, NULL, NULL),
                  FLOWIE_CONTROL_DB_OK);
      check_equal(flowie_control_database_close(archive), FLOWIE_CONTROL_DB_OK);
    }
    imported = (flowie_control_data_transfer_result_t)FLOWIE_CONTROL_DATA_TRANSFER_RESULT_INIT;
    repository = flowie_control_store_repository(rejected_target);
    check_equal(flowie_control_data_import(repository, archive_path, 0, &imported), TURBO_EPROTO);
    check_equal(repository->audit->revision(repository->ctx, &revision), TURBO_OK);
    check_equal(revision, 0u);

    flowie_control_store_destroy(source);
    flowie_control_store_destroy(target);
    flowie_control_store_destroy(rejected_target);
    flowie_control_store_destroy(busy_target);
    check_equal(tt_remove_file(source_path), 0);
    check_equal(tt_remove_file(target_path), 0);
    check_equal(tt_remove_file(rejected_target_path), 0);
    check_equal(tt_remove_file(busy_target_path), 0);
    check_equal(tt_remove_file(archive_path), 0);
    free(source_path);
    free(target_path);
    free(rejected_target_path);
    free(busy_target_path);
    free(archive_path);
  }
}
