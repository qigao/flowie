#include "flowie_control_store_internal.h"

#include "flowie_control_credential_internal.h"
#include "flowie_control_database_internal.h"
#include "flowie_control_repository_internal.h"
#include "flowie_control_store_schema_internal.h"
#include "flowie_control_validation_internal.h"

#include "salts_error.h"
#include "salts_str.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FLOWIE_CONTROL_OPERATION_MAX = 31 };

static const char FLOWIE_CONTROL_OPERATION_USER_CREATE[] = "user.create";
static const char FLOWIE_CONTROL_OPERATION_USER_DISABLE[] = "user.disable";
static const char FLOWIE_CONTROL_OPERATION_DOMAIN_CREATE[] = "domain.create";
static const char FLOWIE_CONTROL_OPERATION_GROUP_CREATE[] = "group.create";
static const char FLOWIE_CONTROL_OPERATION_GROUP_DELETE[] = "group.delete";
static const char FLOWIE_CONTROL_OPERATION_MEMBERSHIP_ADD[] = "membership.add";
static const char FLOWIE_CONTROL_OPERATION_MEMBERSHIP_REMOVE[] = "membership.remove";
static const char FLOWIE_CONTROL_OPERATION_ROLE_CREATE[] = "role.create";
static const char FLOWIE_CONTROL_OPERATION_ROLE_DISABLE[] = "role.disable";
static const char FLOWIE_CONTROL_OPERATION_USER_ROLE_ADD[] = "user_role.add";
static const char FLOWIE_CONTROL_OPERATION_USER_ROLE_REMOVE[] = "user_role.remove";
static const char FLOWIE_CONTROL_OPERATION_CREDENTIAL_GENERATE[] = "credential.generate";
static const char FLOWIE_CONTROL_OPERATION_CREDENTIAL_ROTATE[] = "credential.rotate";
static const char FLOWIE_CONTROL_OPERATION_CREDENTIAL_REVOKE[] = "credential.revoke";
static const char FLOWIE_CONTROL_OPERATION_POLICY_SUBJECT_RULE_PUT[] = "policy.subject_rule.put";
static const char FLOWIE_CONTROL_OPERATION_POLICY_SUBJECT_RULE_DELETE[] =
    "policy.subject_rule.delete";
static const char FLOWIE_CONTROL_OPERATION_POLICY_PUBLISH[] = "policy.publish";
static const char FLOWIE_CONTROL_TARGET_DOMAIN[] = "domain";
static const char FLOWIE_CONTROL_TARGET_GROUP[] = "group";
static const char FLOWIE_CONTROL_TARGET_ROLE[] = "role";
static const char FLOWIE_CONTROL_TARGET_CREDENTIAL[] = "credential";
static const char FLOWIE_CONTROL_DETAIL_ARGON2ID[] = "argon2id";
struct flowie_control_store_s {
  orm_config_t database_config;
  orm_option_t *database_options;
  tstr database_driver;
  tstr *database_option_keywords;
  tstr *database_option_values;
  flowie_control_repository_t repository;
};

typedef struct flowie_control_credential_record_s {
  flowie_control_credential_kdf_params_t params;
  uint8_t salt[FLOWIE_CONTROL_CREDENTIAL_SALT_SIZE];
  uint8_t verifier[FLOWIE_CONTROL_CREDENTIAL_VERIFIER_SIZE];
  uint64_t user_revision;
  uint64_t credential_revision;
  int user_enabled;
  int credential_exists;
  int credential_enabled;
} flowie_control_credential_record_t;

typedef struct flowie_control_policy_bundle_owner_s {
  flowie_security_rule_t *rules;
} flowie_control_policy_bundle_owner_t;

static int flowie_control_database_status(int status) {
  int primary = status & 0xff;
  if (primary == FLOWIE_CONTROL_DB_BUSY || primary == FLOWIE_CONTROL_DB_LOCKED) return SALTS_EBUSY;
  if (primary == FLOWIE_CONTROL_DB_NOMEM) return SALTS_ENOMEM;
  if (primary == FLOWIE_CONTROL_DB_CONSTRAINT || primary == FLOWIE_CONTROL_DB_MISMATCH ||
      primary == FLOWIE_CONTROL_DB_RANGE)
    return SALTS_EINVAL;
  return SALTS_EIO;
}

static int flowie_control_column_text_equal(const flowie_control_statement_t *statement, int column,
                                            const char *expected) {
  const unsigned char *actual;
  size_t expected_size;
  int actual_size;
  if (!statement || !expected) return 0;
  expected_size = strlen(expected);
  actual_size = flowie_control_database_column_bytes(statement, column);
  if (actual_size < 0 || (size_t)actual_size != expected_size ||
      flowie_control_database_column_type(statement, column) != FLOWIE_CONTROL_DB_TEXT)
    return 0;
  if (expected_size == 0u) return 1;
  actual = flowie_control_database_column_text(statement, column);
  return actual && memcmp(actual, expected, expected_size) == 0;
}

static int flowie_control_schema_probe(flowie_control_database_t *database, const char *sql) {
  flowie_control_statement_t *statement = NULL;
  int status;
  if (!database || !sql) return 0;
  status = flowie_control_database_prepare(database, sql, -1, &statement, NULL);
  if (status == FLOWIE_CONTROL_DB_OK) status = flowie_control_database_step(statement);
  (void)flowie_control_database_finalize(statement);
  return status == FLOWIE_CONTROL_DB_ROW || status == FLOWIE_CONTROL_DB_DONE;
}

static int flowie_control_schema_preflight(flowie_control_database_t *database,
                                           int *initialized_out) {
  static const char version_probe[] =
      "SELECT singleton FROM flowie_control_schema_version WHERE 1=0";
  static const char *const table_probes[] = {
      "SELECT singleton FROM flowie_control_meta WHERE 1=0",
      "SELECT domain_id FROM flowie_control_domain WHERE 1=0",
      "SELECT domain_id FROM flowie_control_user WHERE 1=0",
      "SELECT domain_id FROM flowie_control_credential WHERE 1=0",
      "SELECT domain_id FROM flowie_control_group WHERE 1=0",
      "SELECT domain_id FROM flowie_control_membership WHERE 1=0",
      "SELECT domain_id FROM flowie_control_role WHERE 1=0",
      "SELECT domain_id FROM flowie_control_user_role WHERE 1=0",
      "SELECT singleton FROM flowie_control_management_session_sequence WHERE 1=0",
      "SELECT token_digest FROM flowie_control_management_session WHERE 1=0",
      "SELECT request_id FROM flowie_control_audit WHERE 1=0",
      "SELECT domain_id FROM flowie_control_policy_draft WHERE 1=0",
      "SELECT namespace_name FROM flowie_control_published_bundle WHERE 1=0",
      "SELECT namespace_name FROM flowie_control_published_rule WHERE 1=0",
      "SELECT request_id FROM flowie_control_policy_publish_result WHERE 1=0",
  };
  int has_version;
  if (initialized_out) *initialized_out = 0;
  if (!database || !initialized_out) return SALTS_EINVAL;
  has_version = flowie_control_schema_probe(database, version_probe);
  for (size_t index = 0u; index < sizeof(table_probes) / sizeof(table_probes[0]); ++index) {
    int has_table = flowie_control_schema_probe(database, table_probes[index]);
    if (has_version != has_table) return SALTS_EPROTO;
  }
  if (has_version) {
    *initialized_out = 1;
    return SALTS_OK;
  }
  return SALTS_OK;
}

static int flowie_control_schema_initialize(flowie_control_database_t *database,
                                            const char *driver) {
  const char *schema;
  int status;
  if (!database || !driver) return SALTS_EINVAL;
  schema = flowie_control_store_schema_sql(driver, NULL);
  if (!schema) return SALTS_EINVAL;
  status = flowie_control_database_exec(database, schema, NULL, NULL, NULL);
  if (status == FLOWIE_CONTROL_DB_OK) return SALTS_OK;
  return flowie_control_database_status(status);
}

static int flowie_control_schema_validate(flowie_control_database_t *database) {
  flowie_control_statement_t *statement = NULL;
  int status;
  int rc;
  if (!database) return SALTS_EINVAL;
  status = flowie_control_database_prepare(
      database, "SELECT version,fingerprint FROM flowie_control_schema_version WHERE singleton=1",
      -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) return flowie_control_database_status(status);
  status = flowie_control_database_step(statement);
  if (status != FLOWIE_CONTROL_DB_ROW ||
      flowie_control_database_column_type(statement, 0) != FLOWIE_CONTROL_DB_INTEGER ||
      flowie_control_database_column_type(statement, 1) != FLOWIE_CONTROL_DB_TEXT ||
      flowie_control_database_column_int(statement, 0) != FLOWIE_CONTROL_TURBODB_SCHEMA_VERSION ||
      !flowie_control_column_text_equal(statement, 1, FLOWIE_CONTROL_TURBODB_SCHEMA_FINGERPRINT) ||
      flowie_control_database_step(statement) != FLOWIE_CONTROL_DB_DONE) {
    rc = SALTS_EPROTO;
    goto done;
  }
  rc = SALTS_OK;
done:
  (void)flowie_control_database_finalize(statement);
  return rc;
}

static int flowie_control_open_database(const flowie_control_store_t *store,
                                        flowie_control_database_t **out) {
  flowie_control_database_t *database = NULL;
  int status;
  if (out) *out = NULL;
  if (!store || !store->database_driver || !out) return SALTS_EINVAL;
  status = flowie_control_database_open(&store->database_config, &database);
  if (status != FLOWIE_CONTROL_DB_OK) {
    if (database) (void)flowie_control_database_close(database);
    return flowie_control_database_status(status);
  }
  *out = database;
  return SALTS_OK;
}

static void flowie_control_store_database_config_destroy(flowie_control_store_t *store) {
  if (!store) return;
  for (uint32_t index = 0u; index < store->database_config.option_count; ++index) {
    if (store->database_option_keywords) tstr_freep(&store->database_option_keywords[index]);
    if (store->database_option_values && store->database_option_values[index]) {
      /* Resolved TurboDB options may contain credentials and must not survive release. */
      volatile unsigned char *cursor =
          (volatile unsigned char *)store->database_option_values[index];
      size_t remaining = tstr_len(store->database_option_values[index]);
      while (remaining-- != 0u)
        *cursor++ = 0u;
      tstr_freep(&store->database_option_values[index]);
    }
  }
  free(store->database_option_keywords);
  free(store->database_option_values);
  free(store->database_options);
  tstr_freep(&store->database_driver);
  memset(&store->database_config, 0, sizeof(store->database_config));
}

static int flowie_control_store_database_config_copy(flowie_control_store_t *store,
                                                     const orm_config_t *source) {
  if (!store || !source || source->struct_size != sizeof(*source) ||
      source->abi_version != ORM_C_ABI_VERSION || !source->driver.data ||
      source->driver.len == 0u || (source->option_count != 0u && !source->options))
    return SALTS_EINVAL;
  orm_config(&store->database_config);
  store->database_config.option_count = source->option_count;
  store->database_config.max_parameters = source->max_parameters;
  store->database_config.max_columns = source->max_columns;
  store->database_config.max_predicates = source->max_predicates;
  store->database_config.max_assignments = source->max_assignments;
  store->database_config.max_query_bytes = source->max_query_bytes;
  store->database_config.max_parameter_bytes = source->max_parameter_bytes;
  store->database_config.max_result_rows = source->max_result_rows;
  store->database_config.max_result_bytes = source->max_result_bytes;
  store->database_driver = tstr_new_len(source->driver.data, source->driver.len);
  if (!store->database_driver) return SALTS_ENOMEM;
  store->database_config.driver =
      (orm_string_view_t){store->database_driver, tstr_len(store->database_driver)};
  if (source->option_count == 0u) {
    store->database_config.options = NULL;
    return SALTS_OK;
  }
  store->database_options =
      (orm_option_t *)calloc(source->option_count, sizeof(*store->database_options));
  store->database_option_keywords =
      (tstr *)calloc(source->option_count, sizeof(*store->database_option_keywords));
  store->database_option_values =
      (tstr *)calloc(source->option_count, sizeof(*store->database_option_values));
  if (!store->database_options || !store->database_option_keywords ||
      !store->database_option_values)
    return SALTS_ENOMEM;
  for (uint32_t index = 0u; index < source->option_count; ++index) {
    const orm_option_t *input = &source->options[index];
    if (!input->keyword.data || input->keyword.len == 0u ||
        (!input->value.data && input->value.len != 0u))
      return SALTS_EINVAL;
    store->database_option_keywords[index] = tstr_new_len(input->keyword.data, input->keyword.len);
    store->database_option_values[index] =
        tstr_new_len(input->value.data ? input->value.data : "", input->value.len);
    if (!store->database_option_keywords[index] || !store->database_option_values[index])
      return SALTS_ENOMEM;
    store->database_options[index].keyword = (orm_string_view_t){
        store->database_option_keywords[index], tstr_len(store->database_option_keywords[index])};
    store->database_options[index].value = (orm_string_view_t){
        store->database_option_values[index], tstr_len(store->database_option_values[index])};
  }
  store->database_config.options = store->database_options;
  return SALTS_OK;
}

static int flowie_control_bind_text(flowie_control_statement_t *statement, int index,
                                    const char *value) {
  int status =
      flowie_control_database_bind_text(statement, index, value, -1, FLOWIE_CONTROL_DB_TRANSIENT);
  return status == FLOWIE_CONTROL_DB_OK ? SALTS_OK : flowie_control_database_status(status);
}

static int flowie_control_bind_blob(flowie_control_statement_t *statement, int index,
                                    const void *value, size_t size) {
  int status;
  if (!statement || (!value && size != 0u) || size > (size_t)INT_MAX) return SALTS_EINVAL;
  status = flowie_control_database_bind_blob(statement, index, value, (int)size,
                                             FLOWIE_CONTROL_DB_TRANSIENT);
  return status == FLOWIE_CONTROL_DB_OK ? SALTS_OK : flowie_control_database_status(status);
}

static int flowie_control_copy_column(flowie_control_statement_t *statement, int column, char *out,
                                      size_t capacity) {
  const unsigned char *text;
  int length;
  if (!statement || !out || capacity == 0u ||
      flowie_control_database_column_type(statement, column) != FLOWIE_CONTROL_DB_TEXT)
    return SALTS_EPROTO;
  text = flowie_control_database_column_text(statement, column);
  length = flowie_control_database_column_bytes(statement, column);
  if (!text || length <= 0 || (size_t)length >= capacity || memchr(text, '\0', (size_t)length))
    return SALTS_EPROTO;
  memcpy(out, text, (size_t)length);
  out[length] = '\0';
  return SALTS_OK;
}

static int flowie_control_read_revision(flowie_control_database_t *database,
                                        uint64_t *revision_out) {
  flowie_control_statement_t *statement = NULL;
  int status;
  int rc = SALTS_EIO;
  if (!database || !revision_out) return SALTS_EINVAL;
  status = flowie_control_database_prepare(
      database, "SELECT revision FROM flowie_control_meta WHERE singleton=1", -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) return flowie_control_database_status(status);
  status = flowie_control_database_step(statement);
  if (status == FLOWIE_CONTROL_DB_ROW &&
      flowie_control_database_column_type(statement, 0) == FLOWIE_CONTROL_DB_INTEGER &&
      flowie_control_database_column_int64(statement, 0) >= 0) {
    *revision_out = (uint64_t)flowie_control_database_column_int64(statement, 0);
    rc = SALTS_OK;
  } else {
    rc = status == FLOWIE_CONTROL_DB_ROW ? SALTS_EPROTO : flowie_control_database_status(status);
  }
  (void)flowie_control_database_finalize(statement);
  return rc;
}

static int flowie_control_advance_revision(flowie_control_database_t *database, uint64_t current,
                                           uint64_t *next_out) {
  flowie_control_statement_t *statement = NULL;
  uint64_t next;
  int status;
  int rc;
  if (!database || !next_out || current >= (uint64_t)INT64_MAX) return SALTS_ERANGE;
  next = current + 1u;
  status = flowie_control_database_prepare(
      database, "UPDATE flowie_control_meta SET revision=?1 WHERE singleton=1 AND revision=?2", -1,
      &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) return flowie_control_database_status(status);
  status = flowie_control_database_bind_int64(statement, 1, (int64_t)next);
  if (status == FLOWIE_CONTROL_DB_OK)
    status = flowie_control_database_bind_int64(statement, 2, (int64_t)current);
  if (status == FLOWIE_CONTROL_DB_OK) status = flowie_control_database_step(statement);
  rc = status == FLOWIE_CONTROL_DB_DONE && flowie_control_database_changes(database) == 1
           ? SALTS_OK
           : (status == FLOWIE_CONTROL_DB_DONE ? SALTS_EBUSY
                                               : flowie_control_database_status(status));
  (void)flowie_control_database_finalize(statement);
  if (rc == SALTS_OK) *next_out = next;
  return rc;
}

static int flowie_control_replay(flowie_control_database_t *database, const char *request_id,
                                 const char *actor, const char *operation, const char *domain_id,
                                 const char *target_id, const char *target_detail,
                                 flowie_control_command_result_t *result, int *found_out) {
  flowie_control_statement_t *statement = NULL;
  int status;
  int rc = SALTS_EIO;
  if (!database || !request_id || !actor || !operation || !domain_id || !target_id || !result ||
      !found_out)
    return SALTS_EINVAL;
  *found_out = 0;
  status = flowie_control_database_prepare(
      database,
      "SELECT actor,operation,domain_id,target_id,target_detail,result_revision "
      "FROM flowie_control_audit WHERE request_id=?1",
      -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) return flowie_control_database_status(status);
  rc = flowie_control_bind_text(statement, 1, request_id);
  if (rc != SALTS_OK) goto done;
  status = flowie_control_database_step(statement);
  if (status == FLOWIE_CONTROL_DB_DONE) {
    rc = SALTS_OK;
    goto done;
  }
  if (status != FLOWIE_CONTROL_DB_ROW ||
      flowie_control_database_column_type(statement, 0) != FLOWIE_CONTROL_DB_TEXT ||
      flowie_control_database_column_type(statement, 1) != FLOWIE_CONTROL_DB_TEXT ||
      flowie_control_database_column_type(statement, 2) != FLOWIE_CONTROL_DB_TEXT ||
      flowie_control_database_column_type(statement, 3) != FLOWIE_CONTROL_DB_TEXT ||
      flowie_control_database_column_type(statement, 4) != FLOWIE_CONTROL_DB_TEXT ||
      flowie_control_database_column_type(statement, 5) != FLOWIE_CONTROL_DB_INTEGER ||
      flowie_control_database_column_int64(statement, 5) <= 0) {
    rc = status == FLOWIE_CONTROL_DB_ROW ? SALTS_EPROTO : flowie_control_database_status(status);
    goto done;
  }
  if (!flowie_control_column_text_equal(statement, 0, actor) ||
      !flowie_control_column_text_equal(statement, 1, operation) ||
      !flowie_control_column_text_equal(statement, 2, domain_id) ||
      !flowie_control_column_text_equal(statement, 3, target_id) ||
      (target_detail && !flowie_control_column_text_equal(statement, 4, target_detail))) {
    rc = SALTS_EBUSY;
    goto done;
  }
  result->revision = (uint64_t)flowie_control_database_column_int64(statement, 5);
  result->replayed = 1;
  *found_out = 1;
  rc = SALTS_OK;

done:
  (void)flowie_control_database_finalize(statement);
  return rc;
}

static int flowie_control_insert_audit(flowie_control_database_t *database, const char *request_id,
                                       const char *actor, const char *operation,
                                       const char *domain_id, const char *target_id,
                                       const char *target_detail, uint64_t revision,
                                       uint64_t occurred_at) {
  flowie_control_statement_t *statement = NULL;
  int status;
  int rc;
  status = flowie_control_database_prepare(
      database,
      "INSERT INTO flowie_control_audit(request_id,actor,operation,domain_id,target_id,"
      "target_detail,result_revision,occurred_at) VALUES(?1,?2,?3,?4,?5,?6,?7,?8)",
      -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) return flowie_control_database_status(status);
  rc = flowie_control_bind_text(statement, 1, request_id);
  if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 2, actor);
  if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 3, operation);
  if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 4, domain_id);
  if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 5, target_id);
  if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 6, target_detail);
  if (rc == SALTS_OK &&
      flowie_control_database_bind_int64(statement, 7, (int64_t)revision) != FLOWIE_CONTROL_DB_OK)
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
  if (rc == SALTS_OK && flowie_control_database_bind_int64(statement, 8, (int64_t)occurred_at) !=
                            FLOWIE_CONTROL_DB_OK)
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
  if (rc == SALTS_OK) {
    status = flowie_control_database_step(statement);
    rc = status == FLOWIE_CONTROL_DB_DONE ? SALTS_OK : flowie_control_database_status(status);
  }
  (void)flowie_control_database_finalize(statement);
  return rc;
}

static int flowie_control_command_common_valid(const char *domain_id, const char *target_id,
                                               const char *actor, const char *request_id,
                                               uint64_t expected_revision, uint64_t occurred_at) {
  return flowie_control_text_valid(domain_id, FLOWIE_SECURITY_ID_MAX) &&
         flowie_control_text_valid(target_id, FLOWIE_SECURITY_ID_MAX) &&
         flowie_control_text_valid(actor, FLOWIE_CONTROL_ACTOR_MAX) &&
         flowie_control_text_valid(request_id, FLOWIE_CONTROL_REQUEST_ID_MAX) &&
         expected_revision <= (uint64_t)INT64_MAX && occurred_at > 0u &&
         occurred_at <= (uint64_t)INT64_MAX;
}

static int flowie_control_domain_exists(flowie_control_database_t *database,
                                        const char *domain_id) {
  flowie_control_statement_t *statement = NULL;
  int status;
  int rc;
  if (!database || !domain_id) return SALTS_EINVAL;
  status = flowie_control_database_prepare(
      database, "SELECT 1 FROM flowie_control_domain WHERE domain_id=?1", -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) return flowie_control_database_status(status);
  rc = flowie_control_bind_text(statement, 1, domain_id);
  if (rc != SALTS_OK) goto done;
  status = flowie_control_database_step(statement);
  if (status == FLOWIE_CONTROL_DB_DONE) rc = SALTS_ENOENT;
  else if (status != FLOWIE_CONTROL_DB_ROW ||
           flowie_control_database_step(statement) != FLOWIE_CONTROL_DB_DONE)
    rc = status == FLOWIE_CONTROL_DB_ROW ? SALTS_EPROTO : flowie_control_database_status(status);
  else rc = SALTS_OK;
done:
  (void)flowie_control_database_finalize(statement);
  return rc;
}

static int flowie_control_group_lookup(flowie_control_database_t *database, const char *domain_id,
                                       const char *group_id, uint32_t *depth_out,
                                       int *enabled_out) {
  flowie_control_statement_t *statement = NULL;
  int status;
  int rc;
  if (depth_out) *depth_out = 0u;
  if (enabled_out) *enabled_out = 0;
  if (!database || !domain_id || !group_id || !depth_out || !enabled_out) return SALTS_EINVAL;
  status = flowie_control_database_prepare(
      database, "SELECT depth,enabled FROM flowie_control_group WHERE domain_id=?1 AND group_id=?2",
      -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) return flowie_control_database_status(status);
  rc = flowie_control_bind_text(statement, 1, domain_id);
  if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 2, group_id);
  if (rc != SALTS_OK) goto done;
  status = flowie_control_database_step(statement);
  if (status == FLOWIE_CONTROL_DB_DONE) {
    rc = SALTS_ENOENT;
    goto done;
  }
  if (status != FLOWIE_CONTROL_DB_ROW ||
      flowie_control_database_column_type(statement, 0) != FLOWIE_CONTROL_DB_INTEGER ||
      flowie_control_database_column_type(statement, 1) != FLOWIE_CONTROL_DB_INTEGER ||
      flowie_control_database_column_int64(statement, 0) < 0 ||
      flowie_control_database_column_int64(statement, 0) > FLOWIE_CONTROL_GROUP_MAX_DEPTH ||
      (flowie_control_database_column_int(statement, 1) != 0 &&
       flowie_control_database_column_int(statement, 1) != 1)) {
    rc = status == FLOWIE_CONTROL_DB_ROW ? SALTS_EPROTO : flowie_control_database_status(status);
    goto done;
  }
  *depth_out = (uint32_t)flowie_control_database_column_int(statement, 0);
  *enabled_out = flowie_control_database_column_int(statement, 1);
  rc = SALTS_OK;

done:
  (void)flowie_control_database_finalize(statement);
  return rc;
}

static int flowie_control_group_references(flowie_control_database_t *database,
                                           const char *domain_id, const char *group_id,
                                           int *active_child_out, int *direct_membership_out) {
  flowie_control_statement_t *statement = NULL;
  int status;
  int rc;
  if (active_child_out) *active_child_out = 0;
  if (direct_membership_out) *direct_membership_out = 0;
  if (!database || !domain_id || !group_id || !active_child_out || !direct_membership_out)
    return SALTS_EINVAL;
  status = flowie_control_database_prepare(
      database,
      "SELECT EXISTS(SELECT 1 FROM flowie_control_group WHERE domain_id=?1 AND "
      "parent_group_id=?2),EXISTS(SELECT 1 FROM flowie_control_membership WHERE "
      "domain_id=?1 AND group_id=?2)",
      -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) return flowie_control_database_status(status);
  rc = flowie_control_bind_text(statement, 1, domain_id);
  if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 2, group_id);
  if (rc != SALTS_OK) goto done;
  status = flowie_control_database_step(statement);
  if (status != FLOWIE_CONTROL_DB_ROW ||
      flowie_control_database_column_type(statement, 0) != FLOWIE_CONTROL_DB_INTEGER ||
      flowie_control_database_column_type(statement, 1) != FLOWIE_CONTROL_DB_INTEGER ||
      (flowie_control_database_column_int(statement, 0) != 0 &&
       flowie_control_database_column_int(statement, 0) != 1) ||
      (flowie_control_database_column_int(statement, 1) != 0 &&
       flowie_control_database_column_int(statement, 1) != 1)) {
    rc = status == FLOWIE_CONTROL_DB_ROW ? SALTS_EPROTO : flowie_control_database_status(status);
    goto done;
  }
  *active_child_out = flowie_control_database_column_int(statement, 0);
  *direct_membership_out = flowie_control_database_column_int(statement, 1);
  rc = SALTS_OK;

done:
  (void)flowie_control_database_finalize(statement);
  return rc;
}

static int flowie_control_policy_subject_referenced(flowie_control_database_t *database,
                                                    const char *domain_id,
                                                    flowie_security_subject_kind_t subject_kind,
                                                    const char *subject, int *referenced_out) {
  static const char sql[] =
      "SELECT 0,rule_document FROM flowie_control_policy_draft WHERE domain_id=?1 "
      "UNION ALL SELECT 1,rule_line FROM flowie_control_published_rule WHERE namespace_name=?1";
  flowie_control_statement_t *statement = NULL;
  int status;
  int rc;
  if (referenced_out) *referenced_out = 0;
  if (!database || !domain_id || !subject || !referenced_out ||
      (subject_kind != FLOWIE_SECURITY_SUBJECT_PRINCIPAL &&
       subject_kind != FLOWIE_SECURITY_SUBJECT_ROLE &&
       subject_kind != FLOWIE_SECURITY_SUBJECT_GROUP))
    return SALTS_EINVAL;
  status = flowie_control_database_prepare(database, sql, -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) return flowie_control_database_status(status);
  rc = flowie_control_bind_text(statement, 1, domain_id);
  if (rc != SALTS_OK) goto done;
  while ((status = flowie_control_database_step(statement)) == FLOWIE_CONTROL_DB_ROW) {
    const unsigned char *line;
    int line_size;
    int published;
    if (flowie_control_database_column_type(statement, 0) != FLOWIE_CONTROL_DB_INTEGER ||
        flowie_control_database_column_type(statement, 1) != FLOWIE_CONTROL_DB_TEXT) {
      rc = SALTS_EPROTO;
      goto done;
    }
    published = flowie_control_database_column_int(statement, 0);
    line = flowie_control_database_column_text(statement, 1);
    line_size = flowie_control_database_column_bytes(statement, 1);
    if (!line || line_size <= 0) {
      rc = SALTS_EPROTO;
      goto done;
    }
    if (published) {
      flowie_security_rule_t rule = FLOWIE_SECURITY_RULE_INIT;
      if ((size_t)line_size > FLOWIE_SECURITY_RULE_LINE_MAX ||
          flowie_security_rule_parse_line((const char *)line, (size_t)line_size, &rule) !=
              SALTS_OK ||
          strcmp(rule.domain_id, domain_id) != 0) {
        rc = SALTS_EPROTO;
        goto done;
      }
      if (rule.subject_kind == subject_kind && strcmp(rule.subject, subject) == 0) {
        *referenced_out = 1;
        rc = SALTS_OK;
        goto done;
      }
    } else {
      flowie_control_acl_document_t document = FLOWIE_CONTROL_ACL_DOCUMENT_INIT;
      if (flowie_control_acl_parse((const char *)line, (size_t)line_size, &document) != SALTS_OK) {
        rc = SALTS_EPROTO;
        goto done;
      }
      if (document.subject_kind == subject_kind && strcmp(document.subject, subject) == 0) {
        *referenced_out = 1;
        rc = SALTS_OK;
        goto done;
      }
    }
  }
  rc = status == FLOWIE_CONTROL_DB_DONE ? SALTS_OK : flowie_control_database_status(status);

done:
  (void)flowie_control_database_finalize(statement);
  return rc;
}

static int flowie_control_user_enabled(flowie_control_database_t *database, const char *domain_id,
                                       const char *principal_id, int *enabled_out) {
  flowie_control_statement_t *statement = NULL;
  int status;
  int rc;
  if (enabled_out) *enabled_out = 0;
  if (!database || !domain_id || !principal_id || !enabled_out) return SALTS_EINVAL;
  status = flowie_control_database_prepare(
      database, "SELECT enabled FROM flowie_control_user WHERE domain_id=?1 AND principal_id=?2",
      -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) return flowie_control_database_status(status);
  rc = flowie_control_bind_text(statement, 1, domain_id);
  if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 2, principal_id);
  if (rc != SALTS_OK) goto done;
  status = flowie_control_database_step(statement);
  if (status == FLOWIE_CONTROL_DB_DONE) {
    rc = SALTS_ENOENT;
    goto done;
  }
  if (status != FLOWIE_CONTROL_DB_ROW ||
      flowie_control_database_column_type(statement, 0) != FLOWIE_CONTROL_DB_INTEGER ||
      (flowie_control_database_column_int(statement, 0) != 0 &&
       flowie_control_database_column_int(statement, 0) != 1)) {
    rc = status == FLOWIE_CONTROL_DB_ROW ? SALTS_EPROTO : flowie_control_database_status(status);
    goto done;
  }
  *enabled_out = flowie_control_database_column_int(statement, 0);
  rc = SALTS_OK;

done:
  (void)flowie_control_database_finalize(statement);
  return rc;
}

static int flowie_control_credential_record_read(flowie_control_database_t *database,
                                                 const char *domain_id, const char *principal_id,
                                                 flowie_control_credential_record_t *out) {
  flowie_control_credential_record_t record = {0};
  flowie_control_statement_t *statement = NULL;
  const void *salt;
  const void *verifier;
  int status;
  int rc;
  if (!database || !domain_id || !principal_id || !out) return SALTS_EINVAL;
  status = flowie_control_database_prepare(
      database,
      "SELECT u.enabled,u.revision,c.kdf_algorithm,c.memory_blocks,c.passes,c.lanes,c.salt,"
      "c.verifier,c.enabled,c.revision FROM flowie_control_user u LEFT JOIN "
      "flowie_control_credential c ON c.domain_id=u.domain_id AND "
      "c.principal_id=u.principal_id WHERE u.domain_id=?1 AND u.principal_id=?2",
      -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) return flowie_control_database_status(status);
  rc = flowie_control_bind_text(statement, 1, domain_id);
  if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 2, principal_id);
  if (rc != SALTS_OK) goto done;
  status = flowie_control_database_step(statement);
  if (status == FLOWIE_CONTROL_DB_DONE) {
    rc = SALTS_ENOENT;
    goto done;
  }
  if (status != FLOWIE_CONTROL_DB_ROW ||
      flowie_control_database_column_type(statement, 0) != FLOWIE_CONTROL_DB_INTEGER ||
      flowie_control_database_column_type(statement, 1) != FLOWIE_CONTROL_DB_INTEGER ||
      (flowie_control_database_column_int(statement, 0) != 0 &&
       flowie_control_database_column_int(statement, 0) != 1) ||
      flowie_control_database_column_int64(statement, 1) <= 0) {
    rc = status == FLOWIE_CONTROL_DB_ROW ? SALTS_EPROTO : flowie_control_database_status(status);
    goto done;
  }
  record.user_enabled = flowie_control_database_column_int(statement, 0);
  record.user_revision = (uint64_t)flowie_control_database_column_int64(statement, 1);
  if (flowie_control_database_column_type(statement, 2) == FLOWIE_CONTROL_DB_NULL) {
    for (int column = 3; column <= 9; ++column) {
      if (flowie_control_database_column_type(statement, column) != FLOWIE_CONTROL_DB_NULL) {
        rc = SALTS_EPROTO;
        goto done;
      }
    }
    *out = record;
    rc = SALTS_OK;
    goto done;
  }
  if (flowie_control_database_column_type(statement, 2) != FLOWIE_CONTROL_DB_INTEGER ||
      flowie_control_database_column_type(statement, 3) != FLOWIE_CONTROL_DB_INTEGER ||
      flowie_control_database_column_type(statement, 4) != FLOWIE_CONTROL_DB_INTEGER ||
      flowie_control_database_column_type(statement, 5) != FLOWIE_CONTROL_DB_INTEGER ||
      flowie_control_database_column_type(statement, 6) != FLOWIE_CONTROL_DB_BLOB ||
      flowie_control_database_column_bytes(statement, 6) != FLOWIE_CONTROL_CREDENTIAL_SALT_SIZE ||
      flowie_control_database_column_type(statement, 7) != FLOWIE_CONTROL_DB_BLOB ||
      flowie_control_database_column_bytes(statement, 7) !=
          FLOWIE_CONTROL_CREDENTIAL_VERIFIER_SIZE ||
      flowie_control_database_column_type(statement, 8) != FLOWIE_CONTROL_DB_INTEGER ||
      flowie_control_database_column_type(statement, 9) != FLOWIE_CONTROL_DB_INTEGER ||
      flowie_control_database_column_int64(statement, 2) < 0 ||
      flowie_control_database_column_int64(statement, 2) > UINT32_MAX ||
      flowie_control_database_column_int64(statement, 3) < 0 ||
      flowie_control_database_column_int64(statement, 3) > UINT32_MAX ||
      flowie_control_database_column_int64(statement, 4) < 0 ||
      flowie_control_database_column_int64(statement, 4) > UINT32_MAX ||
      flowie_control_database_column_int64(statement, 5) < 0 ||
      flowie_control_database_column_int64(statement, 5) > UINT32_MAX ||
      (flowie_control_database_column_int(statement, 8) != 0 &&
       flowie_control_database_column_int(statement, 8) != 1) ||
      flowie_control_database_column_int64(statement, 9) <= 0) {
    rc = SALTS_EPROTO;
    goto done;
  }
  record.params.algorithm = (uint32_t)flowie_control_database_column_int64(statement, 2);
  record.params.memory_blocks = (uint32_t)flowie_control_database_column_int64(statement, 3);
  record.params.passes = (uint32_t)flowie_control_database_column_int64(statement, 4);
  record.params.lanes = (uint32_t)flowie_control_database_column_int64(statement, 5);
  if (!flowie_control_credential_params_valid(&record.params)) {
    rc = SALTS_EPROTO;
    goto done;
  }
  salt = flowie_control_database_column_blob(statement, 6);
  verifier = flowie_control_database_column_blob(statement, 7);
  if (!salt || !verifier) {
    rc = SALTS_EPROTO;
    goto done;
  }
  memcpy(record.salt, salt, sizeof(record.salt));
  memcpy(record.verifier, verifier, sizeof(record.verifier));
  record.credential_enabled = flowie_control_database_column_int(statement, 8);
  record.credential_revision = (uint64_t)flowie_control_database_column_int64(statement, 9);
  record.credential_exists = 1;
  *out = record;
  rc = SALTS_OK;

done:
  (void)flowie_control_database_finalize(statement);
  if (rc != SALTS_OK) flowie_control_credential_wipe(&record, sizeof(record));
  return rc;
}

static int flowie_control_role_enabled(flowie_control_database_t *database, const char *domain_id,
                                       const char *role_id, int *enabled_out) {
  flowie_control_statement_t *statement = NULL;
  int status;
  int rc;
  if (enabled_out) *enabled_out = 0;
  if (!database || !domain_id || !role_id || !enabled_out) return SALTS_EINVAL;
  status = flowie_control_database_prepare(
      database, "SELECT enabled FROM flowie_control_role WHERE domain_id=?1 AND role_id=?2", -1,
      &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) return flowie_control_database_status(status);
  rc = flowie_control_bind_text(statement, 1, domain_id);
  if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 2, role_id);
  if (rc != SALTS_OK) goto done;
  status = flowie_control_database_step(statement);
  if (status == FLOWIE_CONTROL_DB_DONE) {
    rc = SALTS_ENOENT;
    goto done;
  }
  if (status != FLOWIE_CONTROL_DB_ROW ||
      flowie_control_database_column_type(statement, 0) != FLOWIE_CONTROL_DB_INTEGER ||
      (flowie_control_database_column_int(statement, 0) != 0 &&
       flowie_control_database_column_int(statement, 0) != 1)) {
    rc = status == FLOWIE_CONTROL_DB_ROW ? SALTS_EPROTO : flowie_control_database_status(status);
    goto done;
  }
  *enabled_out = flowie_control_database_column_int(statement, 0);
  rc = SALTS_OK;

done:
  (void)flowie_control_database_finalize(statement);
  return rc;
}

static int flowie_control_effective_roles_database(flowie_control_database_t *database,
                                                   const char *domain_id, const char *principal_id,
                                                   flowie_control_effective_roles_view_t *out) {
  static const char sql[] =
      "SELECT r.role_id FROM flowie_control_user_role ur "
      "JOIN flowie_control_role r ON r.domain_id=ur.domain_id AND r.role_id=ur.role_id "
      "WHERE ur.domain_id=?1 AND ur.principal_id=?2 AND r.enabled=1 ORDER BY r.role_id";
  flowie_control_effective_roles_view_t view = FLOWIE_CONTROL_EFFECTIVE_ROLES_VIEW_INIT;
  flowie_control_statement_t *statement = NULL;
  int status;
  int rc;
  if (!database || !domain_id || !principal_id || !out || out->size < sizeof(*out))
    return SALTS_EINVAL;
  status = flowie_control_database_prepare(database, sql, -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) return flowie_control_database_status(status);
  rc = flowie_control_bind_text(statement, 1, domain_id);
  if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 2, principal_id);
  if (rc != SALTS_OK) goto done;
  while ((status = flowie_control_database_step(statement)) == FLOWIE_CONTROL_DB_ROW) {
    if (view.role_count >= FLOWIE_SECURITY_MAX_ROLES) {
      rc = SALTS_ENOSPC;
      goto done;
    }
    rc = flowie_control_copy_column(statement, 0, view.roles[view.role_count],
                                    sizeof(view.roles[view.role_count]));
    if (rc != SALTS_OK) goto done;
    ++view.role_count;
  }
  if (status != FLOWIE_CONTROL_DB_DONE) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  *out = view;
  rc = SALTS_OK;

done:
  (void)flowie_control_database_finalize(statement);
  return rc;
}

static int flowie_control_effective_groups_database(flowie_control_database_t *database,
                                                    const char *domain_id, const char *principal_id,
                                                    flowie_control_effective_groups_view_t *out) {
  static const char sql[] =
      "WITH RECURSIVE effective(group_id,parent_group_id,depth) AS ("
      "SELECT g.group_id,g.parent_group_id,g.depth FROM flowie_control_membership m "
      "JOIN flowie_control_group g ON g.domain_id=m.domain_id AND g.group_id=m.group_id "
      "WHERE m.domain_id=?1 AND m.principal_id=?2 AND g.enabled=1 "
      "UNION SELECT p.group_id,p.parent_group_id,p.depth FROM effective e "
      "JOIN flowie_control_group p ON p.domain_id=?1 AND p.group_id=e.parent_group_id "
      "WHERE p.enabled=1) SELECT group_id FROM effective GROUP BY group_id "
      "ORDER BY MIN(depth),group_id";
  flowie_control_effective_groups_view_t view = FLOWIE_CONTROL_EFFECTIVE_GROUPS_VIEW_INIT;
  flowie_control_statement_t *statement = NULL;
  int status;
  int rc;
  if (!database || !domain_id || !principal_id || !out || out->size < sizeof(*out))
    return SALTS_EINVAL;
  status = flowie_control_database_prepare(database, sql, -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) return flowie_control_database_status(status);
  rc = flowie_control_bind_text(statement, 1, domain_id);
  if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 2, principal_id);
  if (rc != SALTS_OK) goto done;
  while ((status = flowie_control_database_step(statement)) == FLOWIE_CONTROL_DB_ROW) {
    if (view.group_count >= FLOWIE_SECURITY_MAX_GROUPS) {
      rc = SALTS_ENOSPC;
      goto done;
    }
    rc = flowie_control_copy_column(statement, 0, view.groups[view.group_count],
                                    sizeof(view.groups[view.group_count]));
    if (rc != SALTS_OK) goto done;
    ++view.group_count;
  }
  if (status != FLOWIE_CONTROL_DB_DONE) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  *out = view;
  rc = SALTS_OK;

done:
  (void)flowie_control_database_finalize(statement);
  return rc;
}

static const char *
flowie_control_policy_subject_kind_text(flowie_security_subject_kind_t subject_kind) {
  switch (subject_kind) {
  case FLOWIE_SECURITY_SUBJECT_PRINCIPAL:
    return "user";
  case FLOWIE_SECURITY_SUBJECT_ROLE:
    return "role";
  case FLOWIE_SECURITY_SUBJECT_GROUP:
    return "group";
  default:
    return NULL;
  }
}

static int flowie_control_policy_publish_detail(uint64_t expires_at, char output[64]) {
  int written = snprintf(output, 64u, "expires_at=%llu", (unsigned long long)expires_at);
  return written > 0 && written < 64 ? SALTS_OK : SALTS_EINVAL;
}

static int flowie_control_policy_subject_enabled(flowie_control_database_t *database,
                                                 const char *domain_id,
                                                 flowie_security_subject_kind_t subject_kind,
                                                 const char *subject, int *enabled_out) {
  uint32_t group_depth = 0u;
  if (enabled_out) *enabled_out = 0;
  if (!database || !domain_id || !subject || !enabled_out) return SALTS_EINVAL;
  switch (subject_kind) {
  case FLOWIE_SECURITY_SUBJECT_PRINCIPAL:
    return flowie_control_user_enabled(database, domain_id, subject, enabled_out);
  case FLOWIE_SECURITY_SUBJECT_ROLE:
    return flowie_control_role_enabled(database, domain_id, subject, enabled_out);
  case FLOWIE_SECURITY_SUBJECT_GROUP:
    return flowie_control_group_lookup(database, domain_id, subject, &group_depth, enabled_out);
  default:
    return SALTS_EINVAL;
  }
}

static int flowie_control_policy_document_validate(flowie_control_database_t *database,
                                                   const char *domain_id, const char *document_text,
                                                   size_t document_size,
                                                   flowie_control_acl_document_t *document_out,
                                                   size_t *rule_count_out,
                                                   size_t *deny_rule_count_out) {
  flowie_control_acl_document_t *document = NULL;
  size_t rule_count = 1u;
  size_t deny_count = 0u;
  int enabled = 0;
  int rc;
  if (rule_count_out) *rule_count_out = 0u;
  if (deny_rule_count_out) *deny_rule_count_out = 0u;
  if (!database || !rule_count_out || !deny_rule_count_out) return SALTS_EINVAL;
  document = (flowie_control_acl_document_t *)malloc(sizeof(*document));
  if (!document) return SALTS_ENOMEM;
  flowie_control_acl_document_init(document);
  rc = flowie_control_acl_document_syntax_validate(domain_id, document_text, document_size,
                                                   document);
  if (rc != SALTS_OK) goto done;
  rc = flowie_control_domain_exists(database, domain_id);
  if (rc != SALTS_OK) goto done;
  rc = flowie_control_policy_subject_enabled(database, domain_id, document->subject_kind,
                                             document->subject, &enabled);
  if (rc != SALTS_OK) goto done;
  if (!enabled) {
    rc = SALTS_EPERM;
    goto done;
  }
  if (document->connection_effect == FLOWIE_SECURITY_DENY) deny_count = 1u;
  for (size_t index = 0u; index < document->entry_count; ++index) {
    const flowie_control_acl_entry_t *entry = &document->entries[index];
    if (entry->alternative_count == 0u ||
        rule_count > FLOWIE_SECURITY_MAX_RULES - entry->alternative_count) {
      rc = SALTS_ENOSPC;
      goto done;
    }
    rule_count += entry->alternative_count;
    if (entry->effect == FLOWIE_SECURITY_DENY) deny_count += entry->alternative_count;
  }
  if (document_out) *document_out = *document;
  *rule_count_out = rule_count;
  *deny_rule_count_out = deny_count;
  rc = SALTS_OK;

done:
  free(document);
  return rc;
}

static int flowie_control_policy_validate_database(flowie_control_database_t *database,
                                                   const char *domain_id,
                                                   flowie_control_policy_validation_t *out) {
  flowie_control_policy_validation_t validation = FLOWIE_CONTROL_POLICY_VALIDATION_INIT;
  flowie_control_statement_t *statement = NULL;
  char *subjects = NULL;
  flowie_security_subject_kind_t *subject_kinds = NULL;
  size_t document_count = 0u;
  int status;
  int rc;
  if (!database || !domain_id || !out || out->size < sizeof(*out)) return SALTS_EINVAL;
  rc = flowie_control_read_revision(database, &validation.store_revision);
  if (rc != SALTS_OK) return rc;
  subjects = (char *)calloc(FLOWIE_SECURITY_MAX_RULES, FLOWIE_SECURITY_ID_MAX + 1u);
  if (!subjects) return SALTS_ENOMEM;
  subject_kinds =
      (flowie_security_subject_kind_t *)calloc(FLOWIE_SECURITY_MAX_RULES, sizeof(*subject_kinds));
  if (!subject_kinds) {
    free(subjects);
    return SALTS_ENOMEM;
  }
  status = flowie_control_database_prepare(
      database,
      "SELECT subject_kind,subject_id,rule_document FROM flowie_control_policy_draft "
      "WHERE domain_id=?1 ORDER BY ordinal",
      -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  rc = flowie_control_bind_text(statement, 1, domain_id);
  if (rc != SALTS_OK) goto done;
  while ((status = flowie_control_database_step(statement)) == FLOWIE_CONTROL_DB_ROW) {
    const unsigned char *line;
    int line_size;
    flowie_control_acl_document_t document = FLOWIE_CONTROL_ACL_DOCUMENT_INIT;
    size_t expanded = 0u;
    size_t denied = 0u;
    if (document_count >= FLOWIE_SECURITY_MAX_RULES ||
        flowie_control_database_column_type(statement, 0) != FLOWIE_CONTROL_DB_INTEGER ||
        flowie_control_database_column_type(statement, 1) != FLOWIE_CONTROL_DB_TEXT ||
        flowie_control_database_column_type(statement, 2) != FLOWIE_CONTROL_DB_TEXT) {
      rc = document_count >= FLOWIE_SECURITY_MAX_RULES ? SALTS_ENOSPC : SALTS_EPROTO;
      goto done;
    }
    line = flowie_control_database_column_text(statement, 2);
    line_size = flowie_control_database_column_bytes(statement, 2);
    if (!line || line_size <= 0) {
      rc = SALTS_EPROTO;
      goto done;
    }
    rc = flowie_control_policy_document_validate(database, domain_id, (const char *)line,
                                                 (size_t)line_size, &document, &expanded, &denied);
    if (rc != SALTS_OK) goto done;
    if (flowie_control_database_column_int(statement, 0) != (int)document.subject_kind ||
        !flowie_control_column_text_equal(statement, 1, document.subject)) {
      rc = SALTS_EPROTO;
      goto done;
    }
    for (size_t prior = 0u; prior < document_count; ++prior) {
      if (subject_kinds[prior] == document.subject_kind &&
          strcmp(subjects + prior * (FLOWIE_SECURITY_ID_MAX + 1u), document.subject) == 0) {
        rc = SALTS_EALREADY;
        goto done;
      }
    }
    memcpy(subjects + document_count * (FLOWIE_SECURITY_ID_MAX + 1u), document.subject,
           strlen(document.subject) + 1u);
    subject_kinds[document_count] = document.subject_kind;
    ++document_count;
    if (expanded > FLOWIE_SECURITY_MAX_RULES - validation.rule_count) {
      rc = SALTS_ENOSPC;
      goto done;
    }
    validation.rule_count += expanded;
    validation.deny_rule_count += denied;
  }
  if (status != FLOWIE_CONTROL_DB_DONE) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  if (validation.rule_count == 0u) {
    rc = SALTS_ENOENT;
    goto done;
  }
  *out = validation;
  rc = SALTS_OK;

done:
  (void)flowie_control_database_finalize(statement);
  free(subject_kinds);
  free(subjects);
  return rc;
}

static int flowie_control_policy_changes_target_row(
    const flowie_control_policy_dry_run_change_t *changes, size_t change_count,
    flowie_security_subject_kind_t subject_kind, const char *subject_id, uint8_t *matched_changes) {
  int matched = 0;
  if (!changes || !subject_id) return 0;
  for (size_t index = 0u; index < change_count; ++index) {
    if (changes[index].subject_kind == subject_kind && changes[index].subject_id &&
        strcmp(changes[index].subject_id, subject_id) == 0) {
      if (matched_changes) matched_changes[index] = 1u;
      matched = 1;
    }
  }
  return matched;
}

static int flowie_control_policy_diagnostic_add(
    flowie_control_policy_dry_run_result_t *result, flowie_control_policy_diagnostic_code_t code,
    size_t change_index, const flowie_control_policy_dry_run_change_t *change,
    flowie_control_policy_diagnostic_field_t field) {
  flowie_control_policy_diagnostic_t diagnostic = FLOWIE_CONTROL_POLICY_DIAGNOSTIC_INIT;
  size_t subject_size = 0u;
  if (!result || code == FLOWIE_CONTROL_POLICY_DIAGNOSTIC_NONE ||
      result->diagnostic_count >= result->diagnostic_capacity || !result->diagnostics)
    return SALTS_ENOSPC;
  diagnostic.code = code;
  diagnostic.change_index = change_index;
  diagnostic.has_change_index = change != NULL;
  diagnostic.field = field;
  if (change) {
    diagnostic.subject_kind = change->subject_kind;
    if (change->subject_id) {
      subject_size = strnlen(change->subject_id, FLOWIE_SECURITY_ID_MAX + 1u);
      if (subject_size > FLOWIE_SECURITY_ID_MAX) return SALTS_EINVAL;
      memcpy(diagnostic.subject_id, change->subject_id, subject_size + 1u);
    }
  }
  result->diagnostics[result->diagnostic_count++] = diagnostic;
  return SALTS_OK;
}

static int
flowie_control_policy_candidate_error(flowie_control_policy_dry_run_result_t *result,
                                      int candidate_rc, size_t change_index,
                                      const flowie_control_policy_dry_run_change_t *change) {
  switch (candidate_rc) {
  case SALTS_ENOENT:
    return flowie_control_policy_diagnostic_add(
        result, FLOWIE_CONTROL_POLICY_DIAGNOSTIC_SUBJECT_NOT_FOUND, change_index, change,
        FLOWIE_CONTROL_POLICY_DIAGNOSTIC_FIELD_SUBJECT_ID);
  case SALTS_EPERM:
    return flowie_control_policy_diagnostic_add(
        result, FLOWIE_CONTROL_POLICY_DIAGNOSTIC_SUBJECT_DISABLED, change_index, change,
        FLOWIE_CONTROL_POLICY_DIAGNOSTIC_FIELD_SUBJECT_ID);
  case SALTS_ENOSPC:
    return flowie_control_policy_diagnostic_add(result, FLOWIE_CONTROL_POLICY_DIAGNOSTIC_RULE_LIMIT,
                                                change_index, change,
                                                FLOWIE_CONTROL_POLICY_DIAGNOSTIC_FIELD_ENTRIES);
  case SALTS_EINVAL:
  case SALTS_EPROTO:
    return flowie_control_policy_diagnostic_add(
        result, FLOWIE_CONTROL_POLICY_DIAGNOSTIC_INVALID_DOCUMENT, change_index, change,
        FLOWIE_CONTROL_POLICY_DIAGNOSTIC_FIELD_ENTRIES);
  default:
    return candidate_rc;
  }
}

static int
flowie_control_policy_dry_run_database(flowie_control_database_t *database, const char *domain_id,
                                       const flowie_control_policy_dry_run_change_t *changes,
                                       size_t change_count,
                                       flowie_control_policy_dry_run_result_t *out) {
  flowie_control_policy_dry_run_result_t dry_run = FLOWIE_CONTROL_POLICY_DRY_RUN_RESULT_INIT;
  flowie_control_statement_t *statement = NULL;
  uint8_t *used_ordinals = NULL;
  uint8_t *matched_changes = NULL;
  uint8_t *invalid_changes = NULL;
  int status;
  int rc;
  if (!database || !domain_id || !changes || change_count == 0u ||
      change_count > FLOWIE_CONTROL_POLICY_DRY_RUN_MAX_CHANGES || !out || out->size < sizeof(*out))
    return SALTS_EINVAL;
  dry_run.diagnostics = out->diagnostics;
  dry_run.diagnostic_capacity = out->diagnostic_capacity;
  rc = flowie_control_read_revision(database, &dry_run.store_revision);
  if (rc != SALTS_OK) return rc;
  used_ordinals = (uint8_t *)calloc(FLOWIE_SECURITY_MAX_RULES, sizeof(*used_ordinals));
  if (!used_ordinals) return SALTS_ENOMEM;
  matched_changes = (uint8_t *)calloc(change_count, sizeof(*matched_changes));
  if (!matched_changes) {
    free(used_ordinals);
    return SALTS_ENOMEM;
  }
  invalid_changes = (uint8_t *)calloc(change_count, sizeof(*invalid_changes));
  if (!invalid_changes) {
    free(matched_changes);
    free(used_ordinals);
    return SALTS_ENOMEM;
  }
  for (size_t index = 0u; index < change_count; ++index) {
    const flowie_control_policy_dry_run_change_t *change = &changes[index];
    for (size_t prior = 0u; prior < index; ++prior) {
      if (changes[prior].subject_kind == change->subject_kind && changes[prior].subject_id &&
          change->subject_id && strcmp(changes[prior].subject_id, change->subject_id) == 0) {
        rc = flowie_control_policy_diagnostic_add(
            &dry_run, FLOWIE_CONTROL_POLICY_DIAGNOSTIC_DUPLICATE_CHANGE, index, change,
            FLOWIE_CONTROL_POLICY_DIAGNOSTIC_FIELD_CHANGES);
        if (rc != SALTS_OK) goto done;
        invalid_changes[index] = 1u;
        break;
      }
    }
  }
  status = flowie_control_database_prepare(
      database,
      "SELECT subject_kind,subject_id,ordinal,rule_document FROM flowie_control_policy_draft "
      "WHERE domain_id=?1 ORDER BY ordinal",
      -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  rc = flowie_control_bind_text(statement, 1, domain_id);
  if (rc != SALTS_OK) goto done;
  while ((status = flowie_control_database_step(statement)) == FLOWIE_CONTROL_DB_ROW) {
    const unsigned char *subject_id;
    const unsigned char *document_text;
    flowie_security_subject_kind_t subject_kind;
    int ordinal;
    int document_size;
    size_t expanded = 0u;
    size_t denied = 0u;
    if (flowie_control_database_column_type(statement, 0) != FLOWIE_CONTROL_DB_INTEGER ||
        flowie_control_database_column_type(statement, 1) != FLOWIE_CONTROL_DB_TEXT ||
        flowie_control_database_column_type(statement, 2) != FLOWIE_CONTROL_DB_INTEGER ||
        flowie_control_database_column_type(statement, 3) != FLOWIE_CONTROL_DB_TEXT) {
      rc = SALTS_EPROTO;
      goto done;
    }
    subject_kind = (flowie_security_subject_kind_t)flowie_control_database_column_int(statement, 0);
    subject_id = flowie_control_database_column_text(statement, 1);
    ordinal = flowie_control_database_column_int(statement, 2);
    document_text = flowie_control_database_column_text(statement, 3);
    document_size = flowie_control_database_column_bytes(statement, 3);
    if (!subject_id || !document_text || document_size <= 0 || ordinal < 0 ||
        ordinal >= (int)FLOWIE_SECURITY_MAX_RULES) {
      rc = SALTS_EPROTO;
      goto done;
    }
    if (flowie_control_policy_changes_target_row(changes, change_count, subject_kind,
                                                 (const char *)subject_id, matched_changes))
      continue;
    rc = flowie_control_policy_document_validate(database, domain_id, (const char *)document_text,
                                                 (size_t)document_size, NULL, &expanded, &denied);
    if (rc != SALTS_OK) goto done;
    if (used_ordinals[(size_t)ordinal]) {
      rc = SALTS_EPROTO;
      goto done;
    }
    used_ordinals[(size_t)ordinal] = 1u;
    if (expanded > FLOWIE_SECURITY_MAX_RULES - dry_run.rule_count) {
      rc = SALTS_ENOSPC;
      goto done;
    }
    dry_run.rule_count += expanded;
    dry_run.deny_rule_count += denied;
  }
  if (status != FLOWIE_CONTROL_DB_DONE) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  for (size_t index = 0u; index < change_count; ++index) {
    const flowie_control_policy_dry_run_change_t *change = &changes[index];
    flowie_control_acl_document_t document = FLOWIE_CONTROL_ACL_DOCUMENT_INIT;
    size_t expanded = 0u;
    size_t denied = 0u;
    if (invalid_changes[index]) continue;
    if (change->size < sizeof(*change) ||
        (change->operation != FLOWIE_CONTROL_POLICY_DRY_RUN_PUT &&
         change->operation != FLOWIE_CONTROL_POLICY_DRY_RUN_DELETE) ||
        !flowie_control_text_valid(change->subject_id, FLOWIE_SECURITY_ID_MAX) ||
        (change->subject_kind != FLOWIE_SECURITY_SUBJECT_PRINCIPAL &&
         change->subject_kind != FLOWIE_SECURITY_SUBJECT_ROLE &&
         change->subject_kind != FLOWIE_SECURITY_SUBJECT_GROUP) ||
        (change->operation == FLOWIE_CONTROL_POLICY_DRY_RUN_PUT &&
         (change->ordinal >= FLOWIE_SECURITY_MAX_RULES || !change->document ||
          change->document_size == 0u ||
          change->document_size > FLOWIE_CONTROL_ACL_DOCUMENT_MAX)) ||
        (change->operation == FLOWIE_CONTROL_POLICY_DRY_RUN_DELETE &&
         (change->document || change->document_size != 0u))) {
      rc = SALTS_EINVAL;
      goto done;
    }
    if (change->operation == FLOWIE_CONTROL_POLICY_DRY_RUN_DELETE) {
      if (!matched_changes[index]) {
        rc = flowie_control_policy_diagnostic_add(
            &dry_run, FLOWIE_CONTROL_POLICY_DIAGNOSTIC_DELETE_TARGET_NOT_FOUND, index, change,
            FLOWIE_CONTROL_POLICY_DIAGNOSTIC_FIELD_SUBJECT_ID);
        if (rc != SALTS_OK) goto done;
      }
      continue;
    }
    rc = flowie_control_policy_document_validate(database, domain_id, change->document,
                                                 change->document_size, &document, &expanded,
                                                 &denied);
    if (rc != SALTS_OK) {
      rc = flowie_control_policy_candidate_error(&dry_run, rc, index, change);
      if (rc != SALTS_OK) goto done;
      continue;
    }
    if (document.subject_kind != change->subject_kind ||
        strcmp(document.subject, change->subject_id) != 0) {
      rc = flowie_control_policy_diagnostic_add(
          &dry_run, FLOWIE_CONTROL_POLICY_DIAGNOSTIC_INVALID_DOCUMENT, index, change,
          FLOWIE_CONTROL_POLICY_DIAGNOSTIC_FIELD_SUBJECT_ID);
      if (rc != SALTS_OK) goto done;
      continue;
    }
    if (used_ordinals[change->ordinal]) {
      rc = flowie_control_policy_diagnostic_add(
          &dry_run, FLOWIE_CONTROL_POLICY_DIAGNOSTIC_ORDINAL_CONFLICT, index, change,
          FLOWIE_CONTROL_POLICY_DIAGNOSTIC_FIELD_ORDINAL);
      if (rc != SALTS_OK) goto done;
      continue;
    }
    used_ordinals[change->ordinal] = 1u;
    if (expanded > FLOWIE_SECURITY_MAX_RULES - dry_run.rule_count) {
      rc = flowie_control_policy_diagnostic_add(
          &dry_run, FLOWIE_CONTROL_POLICY_DIAGNOSTIC_RULE_LIMIT, index, change,
          FLOWIE_CONTROL_POLICY_DIAGNOSTIC_FIELD_ENTRIES);
      if (rc != SALTS_OK) goto done;
      continue;
    }
    dry_run.rule_count += expanded;
    dry_run.deny_rule_count += denied;
  }
  if (dry_run.diagnostic_count != 0u) {
    dry_run.rule_count = 0u;
    dry_run.deny_rule_count = 0u;
    *out = dry_run;
    rc = SALTS_OK;
    goto done;
  }
  if (dry_run.rule_count == 0u) {
    rc = flowie_control_policy_diagnostic_add(&dry_run,
                                              FLOWIE_CONTROL_POLICY_DIAGNOSTIC_EMPTY_POLICY, 0u,
                                              NULL, FLOWIE_CONTROL_POLICY_DIAGNOSTIC_FIELD_CHANGES);
    if (rc != SALTS_OK) goto done;
    *out = dry_run;
    rc = SALTS_OK;
    goto done;
  }
  dry_run.valid = 1;
  *out = dry_run;
  rc = SALTS_OK;

done:
  if (statement) (void)flowie_control_database_finalize(statement);
  free(used_ordinals);
  free(matched_changes);
  free(invalid_changes);
  return rc;
}

int flowie_control_store_open(const flowie_control_store_config_t *config,
                              flowie_control_store_t **out) {
  flowie_control_store_t *store;
  flowie_control_database_t *database = NULL;
  int initialized;
  int rc;
  if (out) *out = NULL;
  if (!config || config->size < sizeof(*config) || !out || !config->database) return SALTS_EINVAL;
  store = (flowie_control_store_t *)calloc(1u, sizeof(*store));
  if (!store) return SALTS_ENOMEM;
  rc = flowie_control_store_database_config_copy(store, config->database);
  if (rc != SALTS_OK) goto fail;
  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) goto fail;
  rc = flowie_control_schema_preflight(database, &initialized);
  if (rc != SALTS_OK) goto fail;
  if (!initialized) {
    int initialize_rc = flowie_control_schema_initialize(database, store->database_driver);
    if (initialize_rc != SALTS_OK) {
      /* A concurrent initializer may have committed while this connection waited. */
      rc = flowie_control_schema_preflight(database, &initialized);
      if (rc != SALTS_OK) goto fail;
      if (!initialized) {
        rc = initialize_rc;
        goto fail;
      }
    }
  }
  rc = flowie_control_schema_validate(database);
  if (rc != SALTS_OK) goto fail;
  (void)flowie_control_database_close(database);
  database = NULL;
  store->repository = (flowie_control_repository_t)FLOWIE_CONTROL_REPOSITORY_INIT;
  rc = flowie_control_repository_bind_turbodb(store, &store->repository);
  if (rc != SALTS_OK) goto fail;
  *out = store;
  return SALTS_OK;

fail:
  if (database) (void)flowie_control_database_close(database);
  flowie_control_store_destroy(store);
  return rc;
}

void flowie_control_store_destroy(flowie_control_store_t *store) {
  if (!store) return;
  flowie_control_store_database_config_destroy(store);
  store->repository = (flowie_control_repository_t)FLOWIE_CONTROL_REPOSITORY_INIT;
  free(store);
}

const flowie_control_repository_t *flowie_control_store_repository(flowie_control_store_t *store) {
  if (!store || flowie_control_repository_validate(&store->repository) != SALTS_OK) return NULL;
  return &store->repository;
}

static int flowie_control_management_session_record_valid(
    const flowie_control_management_session_record_t *record, uint64_t now) {
  return record && record->size >= sizeof(*record) && now > 0u && now <= (uint64_t)INT64_MAX &&
         record->expires_at > now && record->expires_at <= (uint64_t)INT64_MAX &&
         flowie_control_text_valid(record->domain_id, FLOWIE_SECURITY_ID_MAX) &&
         flowie_control_text_valid(record->principal_id, FLOWIE_SECURITY_ID_MAX) &&
         flowie_control_text_valid(record->csrf, FLOWIE_CONTROL_MANAGEMENT_SESSION_CSRF_SIZE) &&
         strlen(record->csrf) == FLOWIE_CONTROL_MANAGEMENT_SESSION_CSRF_SIZE;
}

static int flowie_control_management_session_next_sequence(flowie_control_database_t *database,
                                                           uint64_t *sequence_out) {
  static const char update[] =
      "UPDATE flowie_control_management_session_sequence SET value=value+1 "
      "WHERE singleton=1 AND value<9223372036854775807";
  flowie_control_statement_t *statement = NULL;
  int64_t sequence;
  int status;
  int rc;
  if (sequence_out) *sequence_out = 0u;
  if (!database || !sequence_out) return SALTS_EINVAL;
  status = flowie_control_database_exec(database, update, NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) return flowie_control_database_status(status);
  if (flowie_control_database_changes(database) != 1) return SALTS_ERANGE;
  status = flowie_control_database_prepare(
      database, "SELECT value FROM flowie_control_management_session_sequence WHERE singleton=1",
      -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) return flowie_control_database_status(status);
  status = flowie_control_database_step(statement);
  sequence =
      status == FLOWIE_CONTROL_DB_ROW ? flowie_control_database_column_int64(statement, 0) : 0;
  rc = status == FLOWIE_CONTROL_DB_ROW && sequence > 0 ? SALTS_OK : SALTS_EIO;
  (void)flowie_control_database_finalize(statement);
  if (rc == SALTS_OK) *sequence_out = (uint64_t)sequence;
  return rc;
}

static int flowie_control_management_session_delete_expired(flowie_control_database_t *database,
                                                            uint64_t now) {
  flowie_control_statement_t *statement = NULL;
  int status;
  int rc;
  status = flowie_control_database_prepare(
      database, "DELETE FROM flowie_control_management_session WHERE expires_at<=?1", -1,
      &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) return flowie_control_database_status(status);
  status = flowie_control_database_bind_int64(statement, 1, (int64_t)now);
  rc = status == FLOWIE_CONTROL_DB_OK ? SALTS_OK : flowie_control_database_status(status);
  if (rc == SALTS_OK) {
    status = flowie_control_database_step(statement);
    rc = status == FLOWIE_CONTROL_DB_DONE ? SALTS_OK : flowie_control_database_status(status);
  }
  (void)flowie_control_database_finalize(statement);
  return rc;
}

static int flowie_control_management_session_count(flowie_control_database_t *database,
                                                   const char *domain_id, const char *principal_id,
                                                   size_t *count_out) {
  flowie_control_statement_t *statement = NULL;
  int64_t count;
  int status;
  int rc;
  if (count_out) *count_out = 0u;
  if (!database || !count_out || (domain_id && !principal_id) || (!domain_id && principal_id))
    return SALTS_EINVAL;
  status = flowie_control_database_prepare(
      database,
      domain_id ? "SELECT COUNT(*) FROM flowie_control_management_session WHERE domain_id=?1 "
                  "AND principal_id=?2"
                : "SELECT COUNT(*) FROM flowie_control_management_session",
      -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) return flowie_control_database_status(status);
  rc = SALTS_OK;
  if (domain_id) {
    rc = flowie_control_bind_text(statement, 1, domain_id);
    if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 2, principal_id);
  }
  if (rc == SALTS_OK) {
    status = flowie_control_database_step(statement);
    count =
        status == FLOWIE_CONTROL_DB_ROW ? flowie_control_database_column_int64(statement, 0) : -1;
    rc = status == FLOWIE_CONTROL_DB_ROW && count >= 0 && (uint64_t)count <= (uint64_t)SIZE_MAX
             ? SALTS_OK
             : SALTS_EIO;
    if (rc == SALTS_OK) *count_out = (size_t)count;
  }
  (void)flowie_control_database_finalize(statement);
  return rc;
}

static int flowie_control_management_session_evict(flowie_control_database_t *database,
                                                   const char *domain_id, const char *principal_id,
                                                   size_t remove_count) {
  static const char principal_sql[] =
      "DELETE FROM flowie_control_management_session WHERE token_digest IN(SELECT token_digest "
      "FROM flowie_control_management_session WHERE domain_id=?1 AND principal_id=?2 "
      "ORDER BY issued_sequence,token_digest LIMIT ?3)";
  static const char global_sql[] =
      "DELETE FROM flowie_control_management_session WHERE token_digest IN(SELECT token_digest "
      "FROM flowie_control_management_session ORDER BY last_used,token_digest LIMIT ?1)";
  flowie_control_statement_t *statement = NULL;
  int status;
  int rc;
  if (!database || remove_count == 0u || remove_count > (size_t)INT64_MAX ||
      (domain_id && !principal_id) || (!domain_id && principal_id))
    return SALTS_EINVAL;
  status = flowie_control_database_prepare(database, domain_id ? principal_sql : global_sql, -1,
                                           &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) return flowie_control_database_status(status);
  rc = SALTS_OK;
  if (domain_id) {
    rc = flowie_control_bind_text(statement, 1, domain_id);
    if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 2, principal_id);
  }
  if (rc == SALTS_OK) {
    status =
        flowie_control_database_bind_int64(statement, domain_id ? 3 : 1, (int64_t)remove_count);
    rc = status == FLOWIE_CONTROL_DB_OK ? SALTS_OK : flowie_control_database_status(status);
  }
  if (rc == SALTS_OK) {
    status = flowie_control_database_step(statement);
    rc = status == FLOWIE_CONTROL_DB_DONE &&
                 flowie_control_database_changes(database) == (int)remove_count
             ? SALTS_OK
             : SALTS_EIO;
  }
  (void)flowie_control_database_finalize(statement);
  return rc;
}

int flowie_control_store_management_session_issue(
    flowie_control_store_t *store, const flowie_control_management_session_record_t *record,
    size_t capacity, size_t max_sessions_per_principal, uint64_t now) {
  flowie_control_database_t *database = NULL;
  flowie_control_statement_t *statement = NULL;
  size_t count = 0u;
  uint64_t sequence = 0u;
  int transaction_started = 0;
  int status;
  int rc;
  if (!store || !flowie_control_management_session_record_valid(record, now) || capacity == 0u ||
      capacity > FLOWIE_CONTROL_MANAGEMENT_SESSION_MAX_CAPACITY ||
      max_sessions_per_principal == 0u ||
      max_sessions_per_principal > FLOWIE_CONTROL_MANAGEMENT_SESSION_MAX_PER_PRINCIPAL)
    return SALTS_EINVAL;
  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) return rc;
  status = flowie_control_database_exec(database, "BEGIN", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 1;
  rc = flowie_control_management_session_delete_expired(database, now);
  if (rc != SALTS_OK) goto done;
  rc = flowie_control_management_session_count(database, record->domain_id, record->principal_id,
                                               &count);
  if (rc != SALTS_OK) goto done;
  if (count >= max_sessions_per_principal) {
    rc = flowie_control_management_session_evict(database, record->domain_id, record->principal_id,
                                                 count - max_sessions_per_principal + 1u);
    if (rc != SALTS_OK) goto done;
  }
  rc = flowie_control_management_session_count(database, NULL, NULL, &count);
  if (rc != SALTS_OK) goto done;
  if (count >= capacity) {
    rc = flowie_control_management_session_evict(database, NULL, NULL, count - capacity + 1u);
    if (rc != SALTS_OK) goto done;
  }
  rc = flowie_control_management_session_next_sequence(database, &sequence);
  if (rc != SALTS_OK) goto done;
  status = flowie_control_database_prepare(
      database,
      "INSERT INTO flowie_control_management_session(token_digest,domain_id,principal_id,csrf,"
      "expires_at,issued_sequence,last_used) VALUES(?1,?2,?3,?4,?5,?6,?6)",
      -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  rc = flowie_control_bind_blob(statement, 1, record->token_digest, sizeof(record->token_digest));
  if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 2, record->domain_id);
  if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 3, record->principal_id);
  if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 4, record->csrf);
  if (rc == SALTS_OK && flowie_control_database_bind_int64(
                            statement, 5, (int64_t)record->expires_at) != FLOWIE_CONTROL_DB_OK)
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
  if (rc == SALTS_OK &&
      flowie_control_database_bind_int64(statement, 6, (int64_t)sequence) != FLOWIE_CONTROL_DB_OK)
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
  if (rc == SALTS_OK) {
    status = flowie_control_database_step(statement);
    rc = status == FLOWIE_CONTROL_DB_DONE && flowie_control_database_changes(database) == 1
             ? SALTS_OK
             : flowie_control_database_status(status);
  }
  if (rc != SALTS_OK) goto done;
  (void)flowie_control_database_finalize(statement);
  statement = NULL;
  status = flowie_control_database_exec(database, "COMMIT", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 0;
  rc = SALTS_OK;

done:
  (void)flowie_control_database_finalize(statement);
  if (transaction_started)
    (void)flowie_control_database_exec(database, "ROLLBACK", NULL, NULL, NULL);
  (void)flowie_control_database_close(database);
  return rc;
}

int flowie_control_store_management_session_resolve(
    flowie_control_store_t *store,
    const uint8_t token_digest[FLOWIE_CONTROL_MANAGEMENT_SESSION_DIGEST_SIZE], uint64_t now,
    flowie_control_management_session_record_t *out) {
  flowie_control_management_session_record_t record = FLOWIE_CONTROL_MANAGEMENT_SESSION_RECORD_INIT;
  flowie_control_database_t *database = NULL;
  flowie_control_statement_t *statement = NULL;
  uint64_t sequence = 0u;
  int transaction_started = 0;
  int status;
  int rc;
  if (out && out->size >= sizeof(*out))
    *out =
        (flowie_control_management_session_record_t)FLOWIE_CONTROL_MANAGEMENT_SESSION_RECORD_INIT;
  if (!store || !token_digest || now == 0u || now > (uint64_t)INT64_MAX || !out ||
      out->size < sizeof(*out))
    return SALTS_EINVAL;
  memcpy(record.token_digest, token_digest, sizeof(record.token_digest));
  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) return rc;
  status = flowie_control_database_exec(database, "BEGIN", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 1;
  status = flowie_control_database_prepare(
      database,
      "SELECT domain_id,principal_id,csrf,expires_at,issued_sequence,last_used FROM "
      "flowie_control_management_session WHERE token_digest=?1",
      -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  rc = flowie_control_bind_blob(statement, 1, token_digest, sizeof(record.token_digest));
  if (rc != SALTS_OK) goto done;
  status = flowie_control_database_step(statement);
  if (status == FLOWIE_CONTROL_DB_DONE) {
    rc = SALTS_ENOENT;
    goto done;
  }
  if (status != FLOWIE_CONTROL_DB_ROW ||
      flowie_control_copy_column(statement, 0, record.domain_id, sizeof(record.domain_id)) !=
          SALTS_OK ||
      flowie_control_copy_column(statement, 1, record.principal_id, sizeof(record.principal_id)) !=
          SALTS_OK ||
      flowie_control_copy_column(statement, 2, record.csrf, sizeof(record.csrf)) != SALTS_OK) {
    rc = SALTS_EIO;
    goto done;
  }
  record.expires_at = (uint64_t)flowie_control_database_column_int64(statement, 3);
  record.issued_sequence = (uint64_t)flowie_control_database_column_int64(statement, 4);
  record.last_used = (uint64_t)flowie_control_database_column_int64(statement, 5);
  (void)flowie_control_database_finalize(statement);
  statement = NULL;
  if (!flowie_control_text_valid(record.domain_id, FLOWIE_SECURITY_ID_MAX) ||
      !flowie_control_text_valid(record.principal_id, FLOWIE_SECURITY_ID_MAX) ||
      !flowie_control_text_valid(record.csrf, FLOWIE_CONTROL_MANAGEMENT_SESSION_CSRF_SIZE) ||
      strlen(record.csrf) != FLOWIE_CONTROL_MANAGEMENT_SESSION_CSRF_SIZE ||
      record.expires_at == 0u || record.expires_at > (uint64_t)INT64_MAX ||
      record.issued_sequence == 0u || record.last_used == 0u) {
    rc = SALTS_EIO;
    goto done;
  }
  if (now >= record.expires_at) {
    status = flowie_control_database_prepare(
        database, "DELETE FROM flowie_control_management_session WHERE token_digest=?1", -1,
        &statement, NULL);
    if (status != FLOWIE_CONTROL_DB_OK) {
      rc = flowie_control_database_status(status);
      goto done;
    }
    rc = flowie_control_bind_blob(statement, 1, token_digest, sizeof(record.token_digest));
    if (rc == SALTS_OK) {
      status = flowie_control_database_step(statement);
      rc = status == FLOWIE_CONTROL_DB_DONE ? SALTS_ENOENT : flowie_control_database_status(status);
    }
  } else {
    rc = flowie_control_management_session_next_sequence(database, &sequence);
    if (rc != SALTS_OK) goto done;
    status = flowie_control_database_prepare(
        database, "UPDATE flowie_control_management_session SET last_used=?1 WHERE token_digest=?2",
        -1, &statement, NULL);
    if (status != FLOWIE_CONTROL_DB_OK) {
      rc = flowie_control_database_status(status);
      goto done;
    }
    status = flowie_control_database_bind_int64(statement, 1, (int64_t)sequence);
    rc = status == FLOWIE_CONTROL_DB_OK ? SALTS_OK : flowie_control_database_status(status);
    if (rc == SALTS_OK)
      rc = flowie_control_bind_blob(statement, 2, token_digest, sizeof(record.token_digest));
    if (rc == SALTS_OK) {
      status = flowie_control_database_step(statement);
      rc = status == FLOWIE_CONTROL_DB_DONE && flowie_control_database_changes(database) == 1
               ? SALTS_OK
               : SALTS_EIO;
    }
    record.last_used = sequence;
  }
  if (rc != SALTS_OK && rc != SALTS_ENOENT) goto done;
  (void)flowie_control_database_finalize(statement);
  statement = NULL;
  status = flowie_control_database_exec(database, "COMMIT", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 0;
  if (rc == SALTS_OK) *out = record;

done:
  (void)flowie_control_database_finalize(statement);
  if (transaction_started)
    (void)flowie_control_database_exec(database, "ROLLBACK", NULL, NULL, NULL);
  (void)flowie_control_database_close(database);
  return rc;
}

int flowie_control_store_management_session_revoke(
    flowie_control_store_t *store,
    const uint8_t token_digest[FLOWIE_CONTROL_MANAGEMENT_SESSION_DIGEST_SIZE]) {
  flowie_control_database_t *database = NULL;
  flowie_control_statement_t *statement = NULL;
  int status;
  int rc;
  if (!store || !token_digest) return SALTS_EINVAL;
  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) return rc;
  status = flowie_control_database_prepare(
      database, "DELETE FROM flowie_control_management_session WHERE token_digest=?1", -1,
      &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  rc = flowie_control_bind_blob(statement, 1, token_digest,
                                FLOWIE_CONTROL_MANAGEMENT_SESSION_DIGEST_SIZE);
  if (rc == SALTS_OK) {
    status = flowie_control_database_step(statement);
    if (status != FLOWIE_CONTROL_DB_DONE) rc = flowie_control_database_status(status);
    else rc = flowie_control_database_changes(database) == 1 ? SALTS_OK : SALTS_ENOENT;
  }

done:
  (void)flowie_control_database_finalize(statement);
  (void)flowie_control_database_close(database);
  return rc;
}

int flowie_control_store_domain_create(flowie_control_store_t *store,
                                       const flowie_control_domain_create_command_t *command,
                                       flowie_control_command_result_t *result) {
  flowie_control_database_t *database = NULL;
  flowie_control_statement_t *statement = NULL;
  uint64_t current = 0u;
  uint64_t next = 0u;
  int transaction_started = 0;
  int found = 0;
  int status;
  int rc;
  if (result && result->size >= sizeof(*result))
    *result = (flowie_control_command_result_t)FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  if (!store || !command || command->size < sizeof(*command) || !result ||
      result->size < sizeof(*result) ||
      !flowie_control_command_common_valid(command->domain_id, command->domain_id, command->actor,
                                           command->request_id, command->expected_revision,
                                           command->occurred_at))
    return SALTS_EINVAL;
  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) return rc;
  status = flowie_control_database_exec(database, "BEGIN", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 1;
  rc = flowie_control_replay(database, command->request_id, command->actor,
                             FLOWIE_CONTROL_OPERATION_DOMAIN_CREATE, command->domain_id,
                             command->domain_id, FLOWIE_CONTROL_TARGET_DOMAIN, result, &found);
  if (rc != SALTS_OK) goto done;
  if (found) goto commit;
  rc = flowie_control_read_revision(database, &current);
  if (rc != SALTS_OK) goto done;
  if (command->expected_revision != 0u && current != command->expected_revision) {
    rc = SALTS_EBUSY;
    goto done;
  }
  if (current >= (uint64_t)INT64_MAX) {
    rc = SALTS_ERANGE;
    goto done;
  }
  next = current + 1u;
  status = flowie_control_database_prepare(
      database, "INSERT INTO flowie_control_domain(domain_id) VALUES(?1)", -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  rc = flowie_control_bind_text(statement, 1, command->domain_id);
  if (rc == SALTS_OK) {
    status = flowie_control_database_step(statement);
    rc = status == FLOWIE_CONTROL_DB_DONE ? SALTS_OK
                                          : ((status & 0xff) == FLOWIE_CONTROL_DB_CONSTRAINT
                                                 ? SALTS_EALREADY
                                                 : flowie_control_database_status(status));
  }
  (void)flowie_control_database_finalize(statement);
  statement = NULL;
  if (rc != SALTS_OK) goto done;
  rc = flowie_control_advance_revision(database, current, &next);
  if (rc != SALTS_OK) goto done;
  rc = flowie_control_insert_audit(database, command->request_id, command->actor,
                                   FLOWIE_CONTROL_OPERATION_DOMAIN_CREATE, command->domain_id,
                                   command->domain_id, FLOWIE_CONTROL_TARGET_DOMAIN, next,
                                   command->occurred_at);
  if (rc != SALTS_OK) goto done;
  result->revision = next;
  result->replayed = 0;

commit:
  status = flowie_control_database_exec(database, "COMMIT", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 0;
  rc = SALTS_OK;

done:
  if (statement) (void)flowie_control_database_finalize(statement);
  if (transaction_started)
    (void)flowie_control_database_exec(database, "ROLLBACK", NULL, NULL, NULL);
  (void)flowie_control_database_close(database);
  if (rc != SALTS_OK) *result = (flowie_control_command_result_t)FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  return rc;
}

int flowie_control_store_group_create(flowie_control_store_t *store,
                                      const flowie_control_group_create_command_t *command,
                                      flowie_control_command_result_t *result) {
  flowie_control_database_t *database = NULL;
  flowie_control_statement_t *statement = NULL;
  uint64_t current = 0u;
  uint64_t next = 0u;
  uint32_t parent_depth = 0u;
  int parent_enabled = 0;
  int transaction_started = 0;
  int found = 0;
  int status;
  int rc;
  if (result && result->size >= sizeof(*result))
    *result = (flowie_control_command_result_t)FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  if (!store || !command || command->size < sizeof(*command) || !result ||
      result->size < sizeof(*result) ||
      !flowie_control_command_common_valid(command->domain_id, command->group_id, command->actor,
                                           command->request_id, command->expected_revision,
                                           command->occurred_at) ||
      (command->parent_group_id &&
       !flowie_control_text_valid(command->parent_group_id, FLOWIE_SECURITY_ID_MAX)) ||
      strcmp(command->group_id, command->domain_id) == 0 ||
      (command->parent_group_id && strcmp(command->group_id, command->parent_group_id) == 0))
    return SALTS_EINVAL;
  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) return rc;
  status = flowie_control_database_exec(database, "BEGIN", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 1;
  rc = flowie_control_replay(
      database, command->request_id, command->actor, FLOWIE_CONTROL_OPERATION_GROUP_CREATE,
      command->domain_id, command->group_id,
      command->parent_group_id ? command->parent_group_id : FLOWIE_CONTROL_TARGET_DOMAIN, result,
      &found);
  if (rc != SALTS_OK) goto done;
  if (found) goto commit;
  rc = flowie_control_read_revision(database, &current);
  if (rc != SALTS_OK) goto done;
  if (command->expected_revision != 0u && current != command->expected_revision) {
    rc = SALTS_EBUSY;
    goto done;
  }
  if (command->parent_group_id) {
    rc = flowie_control_group_lookup(database, command->domain_id, command->parent_group_id,
                                     &parent_depth, &parent_enabled);
    if (rc != SALTS_OK) goto done;
    if (!parent_enabled) {
      rc = SALTS_EPERM;
      goto done;
    }
    if (parent_depth >= FLOWIE_CONTROL_GROUP_MAX_DEPTH) {
      rc = SALTS_ENOSPC;
      goto done;
    }
  } else {
    rc = flowie_control_domain_exists(database, command->domain_id);
    if (rc != SALTS_OK) goto done;
  }
  if (current >= (uint64_t)INT64_MAX) {
    rc = SALTS_ERANGE;
    goto done;
  }
  next = current + 1u;
  status = flowie_control_database_prepare(
      database,
      "INSERT INTO flowie_control_group(domain_id,group_id,parent_group_id,depth,enabled,"
      "revision,created_at,updated_at) VALUES(?1,?2,?3,?4,1,?5,?6,?6)",
      -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  rc = flowie_control_bind_text(statement, 1, command->domain_id);
  if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 2, command->group_id);
  if (rc == SALTS_OK && command->parent_group_id)
    rc = flowie_control_bind_text(statement, 3, command->parent_group_id);
  else if (rc == SALTS_OK &&
           flowie_control_database_bind_null(statement, 3) != FLOWIE_CONTROL_DB_OK)
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
  if (rc == SALTS_OK &&
      flowie_control_database_bind_int(statement, 4,
                                       command->parent_group_id ? (int)(parent_depth + 1u) : 0) !=
          FLOWIE_CONTROL_DB_OK)
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
  if (rc == SALTS_OK &&
      flowie_control_database_bind_int64(statement, 5, (int64_t)next) != FLOWIE_CONTROL_DB_OK)
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
  if (rc == SALTS_OK && flowie_control_database_bind_int64(
                            statement, 6, (int64_t)command->occurred_at) != FLOWIE_CONTROL_DB_OK)
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
  if (rc == SALTS_OK) {
    status = flowie_control_database_step(statement);
    rc = status == FLOWIE_CONTROL_DB_DONE ? SALTS_OK
                                          : ((status & 0xff) == FLOWIE_CONTROL_DB_CONSTRAINT
                                                 ? SALTS_EALREADY
                                                 : flowie_control_database_status(status));
  }
  (void)flowie_control_database_finalize(statement);
  statement = NULL;
  if (rc != SALTS_OK) goto done;
  rc = flowie_control_advance_revision(database, current, &next);
  if (rc != SALTS_OK) goto done;
  rc = flowie_control_insert_audit(
      database, command->request_id, command->actor, FLOWIE_CONTROL_OPERATION_GROUP_CREATE,
      command->domain_id, command->group_id,
      command->parent_group_id ? command->parent_group_id : FLOWIE_CONTROL_TARGET_DOMAIN, next,
      command->occurred_at);
  if (rc != SALTS_OK) goto done;
  result->revision = next;
  result->replayed = 0;

commit:
  status = flowie_control_database_exec(database, "COMMIT", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 0;
  rc = SALTS_OK;

done:
  if (statement) (void)flowie_control_database_finalize(statement);
  if (transaction_started)
    (void)flowie_control_database_exec(database, "ROLLBACK", NULL, NULL, NULL);
  (void)flowie_control_database_close(database);
  if (rc != SALTS_OK) *result = (flowie_control_command_result_t)FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  return rc;
}

int flowie_control_store_group_delete(flowie_control_store_t *store,
                                      const flowie_control_group_delete_command_t *command,
                                      flowie_control_command_result_t *result) {
  flowie_control_database_t *database = NULL;
  flowie_control_statement_t *statement = NULL;
  uint64_t current = 0u;
  uint64_t next = 0u;
  uint32_t group_depth = 0u;
  int group_enabled = 0;
  int child = 0;
  int direct_membership = 0;
  int policy_reference = 0;
  int transaction_started = 0;
  int found = 0;
  int status;
  int rc;
  if (result && result->size >= sizeof(*result))
    *result = (flowie_control_command_result_t)FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  if (!store || !command || command->size < sizeof(*command) || !result ||
      result->size < sizeof(*result) ||
      !flowie_control_command_common_valid(command->domain_id, command->group_id, command->actor,
                                           command->request_id, command->expected_revision,
                                           command->occurred_at) ||
      strcmp(command->group_id, command->domain_id) == 0)
    return SALTS_EINVAL;
  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) return rc;
  status = flowie_control_database_exec(database, "BEGIN", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 1;
  rc = flowie_control_replay(database, command->request_id, command->actor,
                             FLOWIE_CONTROL_OPERATION_GROUP_DELETE, command->domain_id,
                             command->group_id, FLOWIE_CONTROL_TARGET_GROUP, result, &found);
  if (rc != SALTS_OK) goto done;
  if (found) goto commit;
  rc = flowie_control_read_revision(database, &current);
  if (rc != SALTS_OK) goto done;
  if (command->expected_revision != 0u && current != command->expected_revision) {
    rc = SALTS_EBUSY;
    goto done;
  }
  rc = flowie_control_group_lookup(database, command->domain_id, command->group_id, &group_depth,
                                   &group_enabled);
  if (rc != SALTS_OK) goto done;
  rc = flowie_control_group_references(database, command->domain_id, command->group_id, &child,
                                       &direct_membership);
  if (rc != SALTS_OK) goto done;
  if (child || direct_membership) {
    rc = SALTS_EBUSY;
    goto done;
  }
  rc = flowie_control_policy_subject_referenced(database, command->domain_id,
                                                FLOWIE_SECURITY_SUBJECT_GROUP, command->group_id,
                                                &policy_reference);
  if (rc != SALTS_OK) goto done;
  if (policy_reference) {
    rc = SALTS_EBUSY;
    goto done;
  }
  rc = flowie_control_advance_revision(database, current, &next);
  if (rc != SALTS_OK) goto done;
  status = flowie_control_database_prepare(
      database, "DELETE FROM flowie_control_group WHERE domain_id=?1 AND group_id=?2", -1,
      &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  rc = flowie_control_bind_text(statement, 1, command->domain_id);
  if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 2, command->group_id);
  if (rc == SALTS_OK) {
    status = flowie_control_database_step(statement);
    rc = status == FLOWIE_CONTROL_DB_DONE && flowie_control_database_changes(database) == 1
             ? SALTS_OK
             : (status == FLOWIE_CONTROL_DB_DONE ? SALTS_EBUSY
                                                 : flowie_control_database_status(status));
  }
  (void)flowie_control_database_finalize(statement);
  statement = NULL;
  if (rc != SALTS_OK) goto done;
  rc = flowie_control_insert_audit(database, command->request_id, command->actor,
                                   FLOWIE_CONTROL_OPERATION_GROUP_DELETE, command->domain_id,
                                   command->group_id, FLOWIE_CONTROL_TARGET_GROUP, next,
                                   command->occurred_at);
  if (rc != SALTS_OK) goto done;
  result->revision = next;
  result->replayed = 0;

commit:
  status = flowie_control_database_exec(database, "COMMIT", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 0;
  rc = SALTS_OK;

done:
  if (statement) (void)flowie_control_database_finalize(statement);
  if (transaction_started)
    (void)flowie_control_database_exec(database, "ROLLBACK", NULL, NULL, NULL);
  (void)flowie_control_database_close(database);
  if (rc != SALTS_OK) *result = (flowie_control_command_result_t)FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  return rc;
}

int flowie_control_store_user_create(flowie_control_store_t *store,
                                     const flowie_control_user_create_command_t *command,
                                     flowie_control_command_result_t *result) {
  flowie_control_database_t *database = NULL;
  flowie_control_statement_t *statement = NULL;
  uint64_t current = 0u;
  uint64_t next = 0u;
  int transaction_started = 0;
  int found = 0;
  int status;
  int rc;
  if (result && result->size >= sizeof(*result))
    *result = (flowie_control_command_result_t)FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  if (!store || !command || command->size < sizeof(*command) || !result ||
      result->size < sizeof(*result) ||
      !flowie_control_command_common_valid(command->domain_id, command->principal_id,
                                           command->actor, command->request_id,
                                           command->expected_revision, command->occurred_at) ||
      !flowie_control_text_valid(command->principal_type, FLOWIE_SECURITY_TYPE_MAX))
    return SALTS_EINVAL;
  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) return rc;
  status = flowie_control_database_exec(database, "BEGIN", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 1;
  rc = flowie_control_replay(database, command->request_id, command->actor,
                             FLOWIE_CONTROL_OPERATION_USER_CREATE, command->domain_id,
                             command->principal_id, command->principal_type, result, &found);
  if (rc != SALTS_OK) goto done;
  if (found) goto commit;
  rc = flowie_control_read_revision(database, &current);
  if (rc != SALTS_OK) goto done;
  if (command->expected_revision != 0u && current != command->expected_revision) {
    rc = SALTS_EBUSY;
    goto done;
  }
  rc = flowie_control_domain_exists(database, command->domain_id);
  if (rc != SALTS_OK) goto done;
  if (current >= (uint64_t)INT64_MAX) {
    rc = SALTS_ERANGE;
    goto done;
  }
  next = current + 1u;
  status = flowie_control_database_prepare(
      database,
      "INSERT INTO flowie_control_user(domain_id,principal_id,principal_type,enabled,revision,"
      "created_at,updated_at) VALUES(?1,?2,?3,1,?4,?5,?5)",
      -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  rc = flowie_control_bind_text(statement, 1, command->domain_id);
  if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 2, command->principal_id);
  if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 3, command->principal_type);
  if (rc == SALTS_OK &&
      flowie_control_database_bind_int64(statement, 4, (int64_t)next) != FLOWIE_CONTROL_DB_OK)
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
  if (rc == SALTS_OK && flowie_control_database_bind_int64(
                            statement, 5, (int64_t)command->occurred_at) != FLOWIE_CONTROL_DB_OK)
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
  if (rc == SALTS_OK) {
    status = flowie_control_database_step(statement);
    rc = status == FLOWIE_CONTROL_DB_DONE ? SALTS_OK
                                          : ((status & 0xff) == FLOWIE_CONTROL_DB_CONSTRAINT
                                                 ? SALTS_EALREADY
                                                 : flowie_control_database_status(status));
  }
  (void)flowie_control_database_finalize(statement);
  statement = NULL;
  if (rc != SALTS_OK) goto done;
  rc = flowie_control_advance_revision(database, current, &next);
  if (rc != SALTS_OK) goto done;
  rc = flowie_control_insert_audit(database, command->request_id, command->actor,
                                   FLOWIE_CONTROL_OPERATION_USER_CREATE, command->domain_id,
                                   command->principal_id, command->principal_type, next,
                                   command->occurred_at);
  if (rc != SALTS_OK) goto done;
  result->revision = next;
  result->replayed = 0;

commit:
  status = flowie_control_database_exec(database, "COMMIT", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 0;
  rc = SALTS_OK;

done:
  if (statement) (void)flowie_control_database_finalize(statement);
  if (transaction_started)
    (void)flowie_control_database_exec(database, "ROLLBACK", NULL, NULL, NULL);
  (void)flowie_control_database_close(database);
  if (rc != SALTS_OK) *result = (flowie_control_command_result_t)FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  return rc;
}

int flowie_control_store_user_disable(flowie_control_store_t *store,
                                      const flowie_control_user_disable_command_t *command,
                                      flowie_control_command_result_t *result) {
  flowie_control_database_t *database = NULL;
  flowie_control_statement_t *statement = NULL;
  char principal_type[FLOWIE_SECURITY_TYPE_MAX + 1u] = {0};
  uint64_t current = 0u;
  uint64_t next = 0u;
  int policy_reference = 0;
  int transaction_started = 0;
  int found = 0;
  int status;
  int rc;
  if (result && result->size >= sizeof(*result))
    *result = (flowie_control_command_result_t)FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  if (!store || !command || command->size < sizeof(*command) || !result ||
      result->size < sizeof(*result) ||
      !flowie_control_command_common_valid(command->domain_id, command->principal_id,
                                           command->actor, command->request_id,
                                           command->expected_revision, command->occurred_at))
    return SALTS_EINVAL;
  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) return rc;
  status = flowie_control_database_exec(database, "BEGIN", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 1;
  rc = flowie_control_replay(database, command->request_id, command->actor,
                             FLOWIE_CONTROL_OPERATION_USER_DISABLE, command->domain_id,
                             command->principal_id, NULL, result, &found);
  if (rc != SALTS_OK) goto done;
  if (found) goto commit;
  rc = flowie_control_read_revision(database, &current);
  if (rc != SALTS_OK) goto done;
  if (command->expected_revision != 0u && current != command->expected_revision) {
    rc = SALTS_EBUSY;
    goto done;
  }
  status = flowie_control_database_prepare(
      database,
      "SELECT principal_type,enabled FROM flowie_control_user WHERE domain_id=?1 AND "
      "principal_id=?2",
      -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  rc = flowie_control_bind_text(statement, 1, command->domain_id);
  if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 2, command->principal_id);
  if (rc != SALTS_OK) goto done;
  status = flowie_control_database_step(statement);
  if (status == FLOWIE_CONTROL_DB_DONE) {
    rc = SALTS_ENOENT;
    goto done;
  }
  if (status != FLOWIE_CONTROL_DB_ROW ||
      flowie_control_database_column_type(statement, 1) != FLOWIE_CONTROL_DB_INTEGER) {
    rc = status == FLOWIE_CONTROL_DB_ROW ? SALTS_EPROTO : flowie_control_database_status(status);
    goto done;
  }
  rc = flowie_control_copy_column(statement, 0, principal_type, sizeof(principal_type));
  if (rc != SALTS_OK) goto done;
  if (flowie_control_database_column_int(statement, 1) != 1) {
    rc = SALTS_EALREADY;
    goto done;
  }
  (void)flowie_control_database_finalize(statement);
  statement = NULL;
  rc = flowie_control_policy_subject_referenced(database, command->domain_id,
                                                FLOWIE_SECURITY_SUBJECT_PRINCIPAL,
                                                command->principal_id, &policy_reference);
  if (rc != SALTS_OK) goto done;
  if (policy_reference) {
    rc = SALTS_EBUSY;
    goto done;
  }
  rc = flowie_control_advance_revision(database, current, &next);
  if (rc != SALTS_OK) goto done;
  status = flowie_control_database_prepare(
      database,
      "UPDATE flowie_control_user SET enabled=0,revision=?1,updated_at=?2 WHERE domain_id=?3 "
      "AND principal_id=?4 AND enabled=1",
      -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  if (flowie_control_database_bind_int64(statement, 1, (int64_t)next) != FLOWIE_CONTROL_DB_OK ||
      flowie_control_database_bind_int64(statement, 2, (int64_t)command->occurred_at) !=
          FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
    goto done;
  }
  rc = flowie_control_bind_text(statement, 3, command->domain_id);
  if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 4, command->principal_id);
  if (rc == SALTS_OK) {
    status = flowie_control_database_step(statement);
    rc = status == FLOWIE_CONTROL_DB_DONE && flowie_control_database_changes(database) == 1
             ? SALTS_OK
             : (status == FLOWIE_CONTROL_DB_DONE ? SALTS_EBUSY
                                                 : flowie_control_database_status(status));
  }
  (void)flowie_control_database_finalize(statement);
  statement = NULL;
  if (rc != SALTS_OK) goto done;
  rc = flowie_control_insert_audit(
      database, command->request_id, command->actor, FLOWIE_CONTROL_OPERATION_USER_DISABLE,
      command->domain_id, command->principal_id, principal_type, next, command->occurred_at);
  if (rc != SALTS_OK) goto done;
  result->revision = next;
  result->replayed = 0;

commit:
  status = flowie_control_database_exec(database, "COMMIT", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 0;
  rc = SALTS_OK;

done:
  if (statement) (void)flowie_control_database_finalize(statement);
  if (transaction_started)
    (void)flowie_control_database_exec(database, "ROLLBACK", NULL, NULL, NULL);
  (void)flowie_control_database_close(database);
  if (rc != SALTS_OK) *result = (flowie_control_command_result_t)FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  return rc;
}

int flowie_control_store_user_get(flowie_control_store_t *store, const char *domain_id,
                                  const char *principal_id, flowie_control_user_view_t *out) {
  flowie_control_database_t *database = NULL;
  flowie_control_statement_t *statement = NULL;
  flowie_control_user_view_t view = FLOWIE_CONTROL_USER_VIEW_INIT;
  int status;
  int rc;
  if (out && out->size >= sizeof(*out)) *out = view;
  if (!store || !flowie_control_text_valid(domain_id, FLOWIE_SECURITY_ID_MAX) ||
      !flowie_control_text_valid(principal_id, FLOWIE_SECURITY_ID_MAX) || !out ||
      out->size < sizeof(*out))
    return SALTS_EINVAL;
  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) return rc;
  status = flowie_control_database_prepare(
      database,
      "SELECT domain_id,principal_id,principal_type,enabled,revision,created_at,updated_at "
      "FROM flowie_control_user WHERE domain_id=?1 AND principal_id=?2",
      -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  rc = flowie_control_bind_text(statement, 1, domain_id);
  if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 2, principal_id);
  if (rc != SALTS_OK) goto done;
  status = flowie_control_database_step(statement);
  if (status == FLOWIE_CONTROL_DB_DONE) {
    rc = SALTS_ENOENT;
    goto done;
  }
  if (status != FLOWIE_CONTROL_DB_ROW ||
      flowie_control_database_column_type(statement, 3) != FLOWIE_CONTROL_DB_INTEGER ||
      flowie_control_database_column_type(statement, 4) != FLOWIE_CONTROL_DB_INTEGER ||
      flowie_control_database_column_type(statement, 5) != FLOWIE_CONTROL_DB_INTEGER ||
      flowie_control_database_column_type(statement, 6) != FLOWIE_CONTROL_DB_INTEGER ||
      flowie_control_database_column_int64(statement, 4) <= 0 ||
      flowie_control_database_column_int64(statement, 5) <= 0 ||
      flowie_control_database_column_int64(statement, 6) <= 0) {
    rc = status == FLOWIE_CONTROL_DB_ROW ? SALTS_EPROTO : flowie_control_database_status(status);
    goto done;
  }
  rc = flowie_control_copy_column(statement, 0, view.domain_id, sizeof(view.domain_id));
  if (rc == SALTS_OK)
    rc = flowie_control_copy_column(statement, 1, view.principal_id, sizeof(view.principal_id));
  if (rc == SALTS_OK)
    rc = flowie_control_copy_column(statement, 2, view.principal_type, sizeof(view.principal_type));
  if (rc != SALTS_OK) goto done;
  view.enabled = flowie_control_database_column_int(statement, 3);
  if (view.enabled != 0 && view.enabled != 1) {
    rc = SALTS_EPROTO;
    goto done;
  }
  view.revision = (uint64_t)flowie_control_database_column_int64(statement, 4);
  view.created_at = (uint64_t)flowie_control_database_column_int64(statement, 5);
  view.updated_at = (uint64_t)flowie_control_database_column_int64(statement, 6);
  *out = view;
  rc = SALTS_OK;

done:
  if (statement) (void)flowie_control_database_finalize(statement);
  (void)flowie_control_database_close(database);
  return rc;
}

void flowie_control_generated_credential_wipe(flowie_control_generated_credential_t *credential) {
  if (!credential || credential->size < sizeof(*credential)) return;
  flowie_control_credential_wipe(credential->token, sizeof(credential->token));
  credential->token_size = 0u;
}

static int flowie_control_store_credential_issue(
    flowie_control_store_t *store, const flowie_control_credential_issue_command_t *command,
    flowie_control_generated_credential_t *result, const char *operation, int require_existing) {
  flowie_control_database_t *database = NULL;
  flowie_control_statement_t *statement = NULL;
  flowie_control_credential_record_t record = {0};
  flowie_control_credential_kdf_params_t params;
  flowie_control_command_result_t replay = FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  char token[FLOWIE_CONTROL_CREDENTIAL_TOKEN_CAPACITY] = {0};
  uint8_t salt[FLOWIE_CONTROL_CREDENTIAL_SALT_SIZE] = {0};
  uint8_t verifier[FLOWIE_CONTROL_CREDENTIAL_VERIFIER_SIZE] = {0};
  uint64_t current = 0u;
  uint64_t next = 0u;
  int transaction_started = 0;
  int found = 0;
  int status;
  int rc;
  if (result && result->size >= sizeof(*result)) {
    flowie_control_generated_credential_wipe(result);
    *result = (flowie_control_generated_credential_t)FLOWIE_CONTROL_GENERATED_CREDENTIAL_INIT;
  }
  if (!store || !command || command->size < sizeof(*command) || !result ||
      result->size < sizeof(*result) || !operation ||
      ((!command->initial_secret && command->initial_secret_size != 0u) ||
       (command->initial_secret &&
        (command->initial_secret_size == 0u ||
         command->initial_secret_size > FLOWIE_CONTROL_CREDENTIAL_SECRET_MAX))) ||
      !flowie_control_command_common_valid(command->domain_id, command->principal_id,
                                           command->actor, command->request_id,
                                           command->expected_revision, command->occurred_at))
    return SALTS_EINVAL;
  flowie_control_credential_default_params(&params);
  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) goto done;
  rc = flowie_control_replay(database, command->request_id, command->actor, operation,
                             command->domain_id, command->principal_id,
                             FLOWIE_CONTROL_DETAIL_ARGON2ID, &replay, &found);
  if (rc != SALTS_OK) goto done;
  if (found) {
    rc = SALTS_EALREADY;
    goto done;
  }
  rc = flowie_control_read_revision(database, &current);
  if (rc != SALTS_OK) goto done;
  if (command->expected_revision != 0u && current != command->expected_revision) {
    rc = SALTS_EBUSY;
    goto done;
  }
  rc = flowie_control_credential_record_read(database, command->domain_id, command->principal_id,
                                             &record);
  if (rc != SALTS_OK) goto done;
  if (!record.user_enabled) {
    rc = SALTS_EPERM;
    goto done;
  }
  if (require_existing && !record.credential_exists) {
    rc = SALTS_ENOENT;
    goto done;
  }
  if (!require_existing && record.credential_exists) {
    rc = SALTS_EALREADY;
    goto done;
  }
  flowie_control_credential_wipe(&record, sizeof(record));
  (void)flowie_control_database_close(database);
  database = NULL;

  if (command->initial_secret)
    rc = flowie_control_credential_hash(command->initial_secret, command->initial_secret_size, salt,
                                        verifier, &params);
  else rc = flowie_control_credential_generate(token, salt, verifier, &params);
  if (rc != SALTS_OK) goto done;
  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) goto done;
  status = flowie_control_database_exec(database, "BEGIN", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 1;
  replay = (flowie_control_command_result_t)FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  found = 0;
  rc = flowie_control_replay(database, command->request_id, command->actor, operation,
                             command->domain_id, command->principal_id,
                             FLOWIE_CONTROL_DETAIL_ARGON2ID, &replay, &found);
  if (rc != SALTS_OK) goto done;
  if (found) {
    rc = SALTS_EALREADY;
    goto done;
  }
  rc = flowie_control_read_revision(database, &current);
  if (rc != SALTS_OK) goto done;
  if (command->expected_revision != 0u && current != command->expected_revision) {
    rc = SALTS_EBUSY;
    goto done;
  }
  rc = flowie_control_credential_record_read(database, command->domain_id, command->principal_id,
                                             &record);
  if (rc != SALTS_OK) goto done;
  if (!record.user_enabled) {
    rc = SALTS_EPERM;
    goto done;
  }
  if (require_existing && !record.credential_exists) {
    rc = SALTS_ENOENT;
    goto done;
  }
  if (!require_existing && record.credential_exists) {
    rc = SALTS_EALREADY;
    goto done;
  }
  rc = flowie_control_advance_revision(database, current, &next);
  if (rc != SALTS_OK) goto done;
  status = flowie_control_database_prepare(
      database,
      require_existing
          ? "UPDATE flowie_control_credential SET kdf_algorithm=?3,memory_blocks=?4,passes=?5,"
            "lanes=?6,salt=?7,verifier=?8,enabled=1,revision=?9,updated_at=?10 WHERE "
            "domain_id=?1 AND principal_id=?2"
          : "INSERT INTO flowie_control_credential(domain_id,principal_id,kdf_algorithm,"
            "memory_blocks,passes,lanes,salt,verifier,enabled,revision,created_at,updated_at) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,1,?9,?10,?10)",
      -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  rc = flowie_control_bind_text(statement, 1, command->domain_id);
  if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 2, command->principal_id);
  if (rc == SALTS_OK &&
      flowie_control_database_bind_int64(statement, 3, params.algorithm) != FLOWIE_CONTROL_DB_OK)
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
  if (rc == SALTS_OK && flowie_control_database_bind_int64(statement, 4, params.memory_blocks) !=
                            FLOWIE_CONTROL_DB_OK)
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
  if (rc == SALTS_OK &&
      flowie_control_database_bind_int64(statement, 5, params.passes) != FLOWIE_CONTROL_DB_OK)
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
  if (rc == SALTS_OK &&
      flowie_control_database_bind_int64(statement, 6, params.lanes) != FLOWIE_CONTROL_DB_OK)
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
  if (rc == SALTS_OK) rc = flowie_control_bind_blob(statement, 7, salt, sizeof(salt));
  if (rc == SALTS_OK) rc = flowie_control_bind_blob(statement, 8, verifier, sizeof(verifier));
  if (rc == SALTS_OK &&
      flowie_control_database_bind_int64(statement, 9, (int64_t)next) != FLOWIE_CONTROL_DB_OK)
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
  if (rc == SALTS_OK && flowie_control_database_bind_int64(
                            statement, 10, (int64_t)command->occurred_at) != FLOWIE_CONTROL_DB_OK)
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
  if (rc == SALTS_OK) {
    status = flowie_control_database_step(statement);
    rc = status == FLOWIE_CONTROL_DB_DONE && flowie_control_database_changes(database) == 1
             ? SALTS_OK
             : (status == FLOWIE_CONTROL_DB_DONE ? SALTS_EBUSY
                                                 : flowie_control_database_status(status));
  }
  (void)flowie_control_database_finalize(statement);
  statement = NULL;
  if (rc != SALTS_OK) goto done;
  rc = flowie_control_insert_audit(database, command->request_id, command->actor, operation,
                                   command->domain_id, command->principal_id,
                                   FLOWIE_CONTROL_DETAIL_ARGON2ID, next, command->occurred_at);
  if (rc != SALTS_OK) goto done;
  status = flowie_control_database_exec(database, "COMMIT", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 0;
  result->revision = next;
  if (!command->initial_secret) {
    memcpy(result->token, token, sizeof(result->token));
    result->token_size = FLOWIE_CONTROL_CREDENTIAL_TOKEN_SIZE;
  }
  rc = SALTS_OK;

done:
  if (statement) (void)flowie_control_database_finalize(statement);
  if (transaction_started && database)
    (void)flowie_control_database_exec(database, "ROLLBACK", NULL, NULL, NULL);
  if (database) (void)flowie_control_database_close(database);
  flowie_control_credential_wipe(&record, sizeof(record));
  flowie_control_credential_wipe(token, sizeof(token));
  flowie_control_credential_wipe(salt, sizeof(salt));
  flowie_control_credential_wipe(verifier, sizeof(verifier));
  if (rc != SALTS_OK && result && result->size >= sizeof(*result))
    *result = (flowie_control_generated_credential_t)FLOWIE_CONTROL_GENERATED_CREDENTIAL_INIT;
  return rc;
}

int flowie_control_store_credential_generate(
    flowie_control_store_t *store, const flowie_control_credential_issue_command_t *command,
    flowie_control_generated_credential_t *result) {
  return flowie_control_store_credential_issue(store, command, result,
                                               FLOWIE_CONTROL_OPERATION_CREDENTIAL_GENERATE, 0);
}

int flowie_control_store_credential_rotate(flowie_control_store_t *store,
                                           const flowie_control_credential_issue_command_t *command,
                                           flowie_control_generated_credential_t *result) {
  return flowie_control_store_credential_issue(store, command, result,
                                               FLOWIE_CONTROL_OPERATION_CREDENTIAL_ROTATE, 1);
}

int flowie_control_store_credential_revoke(
    flowie_control_store_t *store, const flowie_control_credential_revoke_command_t *command,
    flowie_control_command_result_t *result) {
  flowie_control_database_t *database = NULL;
  flowie_control_statement_t *statement = NULL;
  flowie_control_credential_record_t record = {0};
  uint64_t current = 0u;
  uint64_t next = 0u;
  int transaction_started = 0;
  int found = 0;
  int status;
  int rc;
  if (result && result->size >= sizeof(*result))
    *result = (flowie_control_command_result_t)FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  if (!store || !command || command->size < sizeof(*command) || !result ||
      result->size < sizeof(*result) ||
      !flowie_control_command_common_valid(command->domain_id, command->principal_id,
                                           command->actor, command->request_id,
                                           command->expected_revision, command->occurred_at))
    return SALTS_EINVAL;
  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) return rc;
  status = flowie_control_database_exec(database, "BEGIN", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 1;
  rc = flowie_control_replay(
      database, command->request_id, command->actor, FLOWIE_CONTROL_OPERATION_CREDENTIAL_REVOKE,
      command->domain_id, command->principal_id, FLOWIE_CONTROL_TARGET_CREDENTIAL, result, &found);
  if (rc != SALTS_OK) goto done;
  if (found) goto commit;
  rc = flowie_control_read_revision(database, &current);
  if (rc != SALTS_OK) goto done;
  if (command->expected_revision != 0u && current != command->expected_revision) {
    rc = SALTS_EBUSY;
    goto done;
  }
  rc = flowie_control_credential_record_read(database, command->domain_id, command->principal_id,
                                             &record);
  if (rc != SALTS_OK) goto done;
  if (!record.credential_exists) {
    rc = SALTS_ENOENT;
    goto done;
  }
  if (!record.credential_enabled) {
    rc = SALTS_EALREADY;
    goto done;
  }
  rc = flowie_control_advance_revision(database, current, &next);
  if (rc != SALTS_OK) goto done;
  status = flowie_control_database_prepare(
      database,
      "UPDATE flowie_control_credential SET enabled=0,revision=?1,updated_at=?2 WHERE "
      "domain_id=?3 AND principal_id=?4 AND enabled=1",
      -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  if (flowie_control_database_bind_int64(statement, 1, (int64_t)next) != FLOWIE_CONTROL_DB_OK ||
      flowie_control_database_bind_int64(statement, 2, (int64_t)command->occurred_at) !=
          FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
    goto done;
  }
  rc = flowie_control_bind_text(statement, 3, command->domain_id);
  if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 4, command->principal_id);
  if (rc == SALTS_OK) {
    status = flowie_control_database_step(statement);
    rc = status == FLOWIE_CONTROL_DB_DONE && flowie_control_database_changes(database) == 1
             ? SALTS_OK
             : (status == FLOWIE_CONTROL_DB_DONE ? SALTS_EBUSY
                                                 : flowie_control_database_status(status));
  }
  (void)flowie_control_database_finalize(statement);
  statement = NULL;
  if (rc != SALTS_OK) goto done;
  rc = flowie_control_insert_audit(database, command->request_id, command->actor,
                                   FLOWIE_CONTROL_OPERATION_CREDENTIAL_REVOKE, command->domain_id,
                                   command->principal_id, FLOWIE_CONTROL_TARGET_CREDENTIAL, next,
                                   command->occurred_at);
  if (rc != SALTS_OK) goto done;
  result->revision = next;
  result->replayed = 0;

commit:
  status = flowie_control_database_exec(database, "COMMIT", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 0;
  rc = SALTS_OK;

done:
  if (statement) (void)flowie_control_database_finalize(statement);
  if (transaction_started)
    (void)flowie_control_database_exec(database, "ROLLBACK", NULL, NULL, NULL);
  (void)flowie_control_database_close(database);
  flowie_control_credential_wipe(&record, sizeof(record));
  if (rc != SALTS_OK) *result = (flowie_control_command_result_t)FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  return rc;
}

int flowie_control_store_credential_verify(flowie_control_store_t *store, const char *domain_id,
                                           const char *principal_id, const void *secret,
                                           size_t secret_size,
                                           flowie_control_credential_verify_result_t *result) {
  flowie_control_credential_record_t record = {0};
  flowie_control_credential_record_t fresh = {0};
  flowie_control_credential_kdf_params_t dummy_params;
  uint8_t dummy_salt[FLOWIE_CONTROL_CREDENTIAL_SALT_SIZE] = {0};
  uint8_t dummy_verifier[FLOWIE_CONTROL_CREDENTIAL_VERIFIER_SIZE] = {0};
  flowie_control_database_t *database = NULL;
  int user_exists = 1;
  int rc;
  if (result && result->size >= sizeof(*result))
    *result =
        (flowie_control_credential_verify_result_t)FLOWIE_CONTROL_CREDENTIAL_VERIFY_RESULT_INIT;
  if (!store || !flowie_control_text_valid(domain_id, FLOWIE_SECURITY_ID_MAX) ||
      !flowie_control_text_valid(principal_id, FLOWIE_SECURITY_ID_MAX) || !secret ||
      secret_size == 0u || secret_size > FLOWIE_CONTROL_CREDENTIAL_SECRET_MAX || !result ||
      result->size < sizeof(*result))
    return SALTS_EINVAL;
  flowie_control_credential_default_params(&dummy_params);
  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) goto done;
  rc = flowie_control_credential_record_read(database, domain_id, principal_id, &record);
  (void)flowie_control_database_close(database);
  database = NULL;
  if (rc == SALTS_ENOENT) {
    user_exists = 0;
    rc = SALTS_OK;
  }
  if (rc != SALTS_OK) goto done;
  rc = flowie_control_credential_verify(secret, secret_size,
                                        record.credential_exists ? record.salt : dummy_salt,
                                        record.credential_exists ? record.verifier : dummy_verifier,
                                        record.credential_exists ? &record.params : &dummy_params);
  if (rc != SALTS_OK) goto done;
  if (!user_exists || !record.user_enabled || !record.credential_exists ||
      !record.credential_enabled) {
    rc = SALTS_EPERM;
    goto done;
  }
  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) goto done;
  rc = flowie_control_credential_record_read(database, domain_id, principal_id, &fresh);
  if (rc != SALTS_OK) goto done;
  if (!fresh.user_enabled || !fresh.credential_exists || !fresh.credential_enabled ||
      fresh.user_revision != record.user_revision ||
      fresh.credential_revision != record.credential_revision) {
    rc = SALTS_EBUSY;
    goto done;
  }
  result->user_revision = fresh.user_revision;
  result->credential_revision = fresh.credential_revision;
  rc = SALTS_OK;

done:
  if (database) (void)flowie_control_database_close(database);
  flowie_control_credential_wipe(&record, sizeof(record));
  flowie_control_credential_wipe(&fresh, sizeof(fresh));
  flowie_control_credential_wipe(dummy_salt, sizeof(dummy_salt));
  flowie_control_credential_wipe(dummy_verifier, sizeof(dummy_verifier));
  if (rc != SALTS_OK && result && result->size >= sizeof(*result))
    *result =
        (flowie_control_credential_verify_result_t)FLOWIE_CONTROL_CREDENTIAL_VERIFY_RESULT_INIT;
  return rc;
}

int flowie_control_store_credential_resolve(flowie_control_store_t *store, const char *principal_id,
                                            const void *secret, size_t secret_size,
                                            flowie_control_credential_resolution_t *result) {
  static const char sql[] = "SELECT u.domain_id FROM flowie_control_user u "
                            "JOIN flowie_control_credential c ON c.domain_id=u.domain_id "
                            "AND c.principal_id=u.principal_id "
                            "WHERE u.principal_id=?1 AND u.enabled=1 AND c.enabled=1 "
                            "ORDER BY u.domain_id LIMIT 2";
  flowie_control_credential_resolution_t resolved = FLOWIE_CONTROL_CREDENTIAL_RESOLUTION_INIT;
  flowie_control_database_t *database = NULL;
  flowie_control_statement_t *statement = NULL;
  size_t match_count = 0u;
  int status;
  int rc;
  if (result && result->size >= sizeof(*result)) *result = resolved;
  if (!store || !flowie_control_text_valid(principal_id, FLOWIE_SECURITY_ID_MAX) || !secret ||
      secret_size == 0u || secret_size > FLOWIE_CONTROL_CREDENTIAL_SECRET_MAX || !result ||
      result->size < sizeof(*result))
    return SALTS_EINVAL;
  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) return rc;
  status = flowie_control_database_prepare(database, sql, -1, &statement, NULL);
  if (status == FLOWIE_CONTROL_DB_OK)
    status = flowie_control_database_bind_text(statement, 1, principal_id, -1,
                                               FLOWIE_CONTROL_DB_TRANSIENT);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  while ((status = flowie_control_database_step(statement)) == FLOWIE_CONTROL_DB_ROW) {
    const unsigned char *domain;
    int domain_size;
    if (flowie_control_database_column_type(statement, 0) != FLOWIE_CONTROL_DB_TEXT ||
        !(domain = flowie_control_database_column_text(statement, 0)) ||
        (domain_size = flowie_control_database_column_bytes(statement, 0)) <= 0 ||
        (size_t)domain_size > FLOWIE_SECURITY_ID_MAX) {
      rc = SALTS_EPROTO;
      goto done;
    }
    if (match_count == 0u) {
      memcpy(resolved.domain_id, domain, (size_t)domain_size);
      resolved.domain_id[domain_size] = '\0';
    }
    ++match_count;
  }
  if (status != FLOWIE_CONTROL_DB_DONE) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  (void)flowie_control_database_finalize(statement);
  statement = NULL;
  (void)flowie_control_database_close(database);
  database = NULL;
  if (match_count != 1u) {
    flowie_control_credential_verify_result_t dummy = FLOWIE_CONTROL_CREDENTIAL_VERIFY_RESULT_INIT;
    (void)flowie_control_store_credential_verify(store, "__unresolved__", principal_id, secret,
                                                 secret_size, &dummy);
    rc = SALTS_EPERM;
    goto done;
  }
  rc = flowie_control_store_credential_verify(store, resolved.domain_id, principal_id, secret,
                                              secret_size, &resolved.verified);
  if (rc == SALTS_OK) *result = resolved;

done:
  if (statement) (void)flowie_control_database_finalize(statement);
  if (database) (void)flowie_control_database_close(database);
  if (rc != SALTS_OK)
    *result = (flowie_control_credential_resolution_t)FLOWIE_CONTROL_CREDENTIAL_RESOLUTION_INIT;
  return rc;
}

int flowie_control_store_credential_state(flowie_control_store_t *store, const char *domain_id,
                                          const char *principal_id,
                                          flowie_control_credential_verify_result_t *result) {
  flowie_control_credential_record_t record = {0};
  flowie_control_database_t *database = NULL;
  int rc;
  if (result && result->size >= sizeof(*result))
    *result =
        (flowie_control_credential_verify_result_t)FLOWIE_CONTROL_CREDENTIAL_VERIFY_RESULT_INIT;
  if (!store || !flowie_control_text_valid(domain_id, FLOWIE_SECURITY_ID_MAX) ||
      !flowie_control_text_valid(principal_id, FLOWIE_SECURITY_ID_MAX) || !result ||
      result->size < sizeof(*result))
    return SALTS_EINVAL;
  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) goto done;
  rc = flowie_control_credential_record_read(database, domain_id, principal_id, &record);
  if (rc == SALTS_ENOENT) rc = SALTS_EPERM;
  if (rc != SALTS_OK) goto done;
  if (!record.user_enabled || !record.credential_exists || !record.credential_enabled) {
    rc = SALTS_EPERM;
    goto done;
  }
  result->user_revision = record.user_revision;
  result->credential_revision = record.credential_revision;
  rc = SALTS_OK;

done:
  if (database) (void)flowie_control_database_close(database);
  flowie_control_credential_wipe(&record, sizeof(record));
  if (rc != SALTS_OK && result && result->size >= sizeof(*result))
    *result =
        (flowie_control_credential_verify_result_t)FLOWIE_CONTROL_CREDENTIAL_VERIFY_RESULT_INIT;
  return rc;
}

int flowie_control_store_current_revision(flowie_control_store_t *store, uint64_t *revision_out) {
  flowie_control_database_t *database = NULL;
  int rc;
  if (revision_out) *revision_out = 0u;
  if (!store || !revision_out) return SALTS_EINVAL;
  rc = flowie_control_open_database(store, &database);
  if (rc == SALTS_OK) rc = flowie_control_read_revision(database, revision_out);
  if (database) (void)flowie_control_database_close(database);
  if (rc != SALTS_OK) *revision_out = 0u;
  return rc;
}

int flowie_control_store_principal_snapshot(
    flowie_control_store_t *store, const char *domain_id, const char *principal_id,
    const flowie_control_credential_verify_result_t *expected,
    flowie_control_principal_snapshot_t *out) {
  static const char sql[] =
      "SELECT u.domain_id,u.principal_id,u.principal_type,u.enabled,u.revision,"
      "c.enabled,c.revision FROM flowie_control_user u LEFT JOIN flowie_control_credential c "
      "ON c.domain_id=u.domain_id AND c.principal_id=u.principal_id "
      "WHERE u.domain_id=?1 AND u.principal_id=?2";
  flowie_control_principal_snapshot_t snapshot = FLOWIE_CONTROL_PRINCIPAL_SNAPSHOT_INIT;
  flowie_control_database_t *database = NULL;
  flowie_control_statement_t *statement = NULL;
  int64_t user_revision;
  int64_t credential_revision;
  int transaction_started = 0;
  int status;
  int rc;

  if (out && out->size >= sizeof(*out)) *out = snapshot;
  if (!store || !flowie_control_text_valid(domain_id, FLOWIE_SECURITY_ID_MAX) ||
      !flowie_control_text_valid(principal_id, FLOWIE_SECURITY_ID_MAX) || !expected ||
      expected->size < sizeof(*expected) || expected->user_revision == 0u ||
      expected->credential_revision == 0u || !out || out->size < sizeof(*out))
    return SALTS_EINVAL;

  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) goto done;
  status = flowie_control_database_exec(database, "BEGIN", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 1;
  status = flowie_control_database_prepare(database, sql, -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  rc = flowie_control_bind_text(statement, 1, domain_id);
  if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 2, principal_id);
  if (rc != SALTS_OK) goto done;
  status = flowie_control_database_step(statement);
  if (status == FLOWIE_CONTROL_DB_DONE) {
    rc = SALTS_EPERM;
    goto done;
  }
  if (status != FLOWIE_CONTROL_DB_ROW ||
      flowie_control_database_column_type(statement, 3) != FLOWIE_CONTROL_DB_INTEGER ||
      flowie_control_database_column_type(statement, 4) != FLOWIE_CONTROL_DB_INTEGER ||
      flowie_control_database_column_type(statement, 5) != FLOWIE_CONTROL_DB_INTEGER ||
      flowie_control_database_column_type(statement, 6) != FLOWIE_CONTROL_DB_INTEGER ||
      (flowie_control_database_column_int(statement, 3) != 0 &&
       flowie_control_database_column_int(statement, 3) != 1) ||
      (flowie_control_database_column_int(statement, 5) != 0 &&
       flowie_control_database_column_int(statement, 5) != 1)) {
    rc = status == FLOWIE_CONTROL_DB_ROW ? SALTS_EPROTO : flowie_control_database_status(status);
    goto done;
  }
  user_revision = flowie_control_database_column_int64(statement, 4);
  credential_revision = flowie_control_database_column_int64(statement, 6);
  if (!flowie_control_database_column_int(statement, 3) ||
      !flowie_control_database_column_int(statement, 5) || user_revision <= 0 ||
      credential_revision <= 0 || (uint64_t)user_revision != expected->user_revision ||
      (uint64_t)credential_revision != expected->credential_revision) {
    rc = SALTS_EPERM;
    goto done;
  }
  rc = flowie_control_copy_column(statement, 0, snapshot.domain_id, sizeof(snapshot.domain_id));
  if (rc == SALTS_OK)
    rc = flowie_control_copy_column(statement, 1, snapshot.principal_id,
                                    sizeof(snapshot.principal_id));
  if (rc == SALTS_OK)
    rc = flowie_control_copy_column(statement, 2, snapshot.principal_type,
                                    sizeof(snapshot.principal_type));
  if (rc != SALTS_OK) goto done;
  snapshot.user_revision = (uint64_t)user_revision;
  snapshot.credential_revision = (uint64_t)credential_revision;
  (void)flowie_control_database_finalize(statement);
  statement = NULL;

  rc = flowie_control_effective_groups_database(database, domain_id, principal_id,
                                                &snapshot.effective_groups);
  if (rc == SALTS_OK)
    rc = flowie_control_effective_roles_database(database, domain_id, principal_id,
                                                 &snapshot.effective_roles);
  if (rc != SALTS_OK) goto done;
  status = flowie_control_database_exec(database, "COMMIT", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 0;
  *out = snapshot;
  rc = SALTS_OK;

done:
  if (statement) (void)flowie_control_database_finalize(statement);
  if (transaction_started && database)
    (void)flowie_control_database_exec(database, "ROLLBACK", NULL, NULL, NULL);
  if (database) (void)flowie_control_database_close(database);
  if (rc != SALTS_OK && out && out->size >= sizeof(*out))
    *out = (flowie_control_principal_snapshot_t)FLOWIE_CONTROL_PRINCIPAL_SNAPSHOT_INIT;
  return rc;
}

int flowie_control_store_external_principal_snapshot(flowie_control_store_t *store,
                                                     const char *domain_id,
                                                     const char *principal_id,
                                                     uint64_t assertion_revision,
                                                     flowie_control_principal_snapshot_t *out) {
  static const char sql[] = "SELECT domain_id,principal_id,principal_type,enabled,revision "
                            "FROM flowie_control_user WHERE domain_id=?1 AND principal_id=?2";
  flowie_control_principal_snapshot_t snapshot = FLOWIE_CONTROL_PRINCIPAL_SNAPSHOT_INIT;
  flowie_control_database_t *database = NULL;
  flowie_control_statement_t *statement = NULL;
  int64_t user_revision;
  int transaction_started = 0;
  int status;
  int rc;

  if (out && out->size >= sizeof(*out)) *out = snapshot;
  if (!store || !flowie_control_text_valid(domain_id, FLOWIE_SECURITY_ID_MAX) ||
      !flowie_control_text_valid(principal_id, FLOWIE_SECURITY_ID_MAX) ||
      assertion_revision == 0u || !out || out->size < sizeof(*out))
    return SALTS_EINVAL;

  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) goto done;
  status = flowie_control_database_exec(database, "BEGIN", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 1;
  status = flowie_control_database_prepare(database, sql, -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  rc = flowie_control_bind_text(statement, 1, domain_id);
  if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 2, principal_id);
  if (rc != SALTS_OK) goto done;
  status = flowie_control_database_step(statement);
  if (status == FLOWIE_CONTROL_DB_DONE) {
    rc = SALTS_EPERM;
    goto done;
  }
  if (status != FLOWIE_CONTROL_DB_ROW ||
      flowie_control_database_column_type(statement, 3) != FLOWIE_CONTROL_DB_INTEGER ||
      flowie_control_database_column_type(statement, 4) != FLOWIE_CONTROL_DB_INTEGER ||
      (flowie_control_database_column_int(statement, 3) != 0 &&
       flowie_control_database_column_int(statement, 3) != 1)) {
    rc = status == FLOWIE_CONTROL_DB_ROW ? SALTS_EPROTO : flowie_control_database_status(status);
    goto done;
  }
  user_revision = flowie_control_database_column_int64(statement, 4);
  if (!flowie_control_database_column_int(statement, 3) || user_revision <= 0) {
    rc = SALTS_EPERM;
    goto done;
  }
  rc = flowie_control_copy_column(statement, 0, snapshot.domain_id, sizeof(snapshot.domain_id));
  if (rc == SALTS_OK)
    rc = flowie_control_copy_column(statement, 1, snapshot.principal_id,
                                    sizeof(snapshot.principal_id));
  if (rc == SALTS_OK)
    rc = flowie_control_copy_column(statement, 2, snapshot.principal_type,
                                    sizeof(snapshot.principal_type));
  if (rc != SALTS_OK) goto done;
  snapshot.user_revision = (uint64_t)user_revision;
  snapshot.credential_revision = assertion_revision;
  (void)flowie_control_database_finalize(statement);
  statement = NULL;

  rc = flowie_control_effective_groups_database(database, domain_id, principal_id,
                                                &snapshot.effective_groups);
  if (rc == SALTS_OK)
    rc = flowie_control_effective_roles_database(database, domain_id, principal_id,
                                                 &snapshot.effective_roles);
  if (rc != SALTS_OK) goto done;
  status = flowie_control_database_exec(database, "COMMIT", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 0;
  *out = snapshot;
  rc = SALTS_OK;

done:
  if (statement) (void)flowie_control_database_finalize(statement);
  if (transaction_started && database)
    (void)flowie_control_database_exec(database, "ROLLBACK", NULL, NULL, NULL);
  if (database) (void)flowie_control_database_close(database);
  if (rc != SALTS_OK && out && out->size >= sizeof(*out))
    *out = (flowie_control_principal_snapshot_t)FLOWIE_CONTROL_PRINCIPAL_SNAPSHOT_INIT;
  return rc;
}

int flowie_control_store_membership_add(flowie_control_store_t *store,
                                        const flowie_control_membership_add_command_t *command,
                                        flowie_control_command_result_t *result) {
  flowie_control_database_t *database = NULL;
  flowie_control_statement_t *statement = NULL;
  flowie_control_effective_groups_view_t effective = FLOWIE_CONTROL_EFFECTIVE_GROUPS_VIEW_INIT;
  uint64_t current = 0u;
  uint64_t next = 0u;
  uint32_t group_depth = 0u;
  int group_enabled = 0;
  int user_enabled = 0;
  int transaction_started = 0;
  int found = 0;
  int status;
  int rc;
  if (result && result->size >= sizeof(*result))
    *result = (flowie_control_command_result_t)FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  if (!store || !command || command->size < sizeof(*command) || !result ||
      result->size < sizeof(*result) ||
      !flowie_control_command_common_valid(command->domain_id, command->principal_id,
                                           command->actor, command->request_id,
                                           command->expected_revision, command->occurred_at) ||
      !flowie_control_text_valid(command->group_id, FLOWIE_SECURITY_ID_MAX) ||
      strcmp(command->group_id, command->domain_id) == 0)
    return SALTS_EINVAL;
  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) return rc;
  status = flowie_control_database_exec(database, "BEGIN", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 1;
  rc = flowie_control_replay(database, command->request_id, command->actor,
                             FLOWIE_CONTROL_OPERATION_MEMBERSHIP_ADD, command->domain_id,
                             command->principal_id, command->group_id, result, &found);
  if (rc != SALTS_OK) goto done;
  if (found) goto commit;
  rc = flowie_control_read_revision(database, &current);
  if (rc != SALTS_OK) goto done;
  if (command->expected_revision != 0u && current != command->expected_revision) {
    rc = SALTS_EBUSY;
    goto done;
  }
  rc = flowie_control_user_enabled(database, command->domain_id, command->principal_id,
                                   &user_enabled);
  if (rc != SALTS_OK) goto done;
  if (!user_enabled) {
    rc = SALTS_EPERM;
    goto done;
  }
  rc = flowie_control_group_lookup(database, command->domain_id, command->group_id, &group_depth,
                                   &group_enabled);
  if (rc != SALTS_OK) goto done;
  if (!group_enabled) {
    rc = SALTS_EPERM;
    goto done;
  }
  if (current >= (uint64_t)INT64_MAX) {
    rc = SALTS_ERANGE;
    goto done;
  }
  next = current + 1u;
  status = flowie_control_database_prepare(
      database,
      "INSERT INTO flowie_control_membership(domain_id,principal_id,group_id,revision,"
      "created_at) VALUES(?1,?2,?3,?4,?5)",
      -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  rc = flowie_control_bind_text(statement, 1, command->domain_id);
  if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 2, command->principal_id);
  if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 3, command->group_id);
  if (rc == SALTS_OK &&
      flowie_control_database_bind_int64(statement, 4, (int64_t)next) != FLOWIE_CONTROL_DB_OK)
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
  if (rc == SALTS_OK && flowie_control_database_bind_int64(
                            statement, 5, (int64_t)command->occurred_at) != FLOWIE_CONTROL_DB_OK)
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
  if (rc == SALTS_OK) {
    status = flowie_control_database_step(statement);
    rc = status == FLOWIE_CONTROL_DB_DONE ? SALTS_OK
                                          : ((status & 0xff) == FLOWIE_CONTROL_DB_CONSTRAINT
                                                 ? SALTS_EALREADY
                                                 : flowie_control_database_status(status));
  }
  (void)flowie_control_database_finalize(statement);
  statement = NULL;
  if (rc != SALTS_OK) goto done;
  rc = flowie_control_effective_groups_database(database, command->domain_id, command->principal_id,
                                                &effective);
  if (rc != SALTS_OK) goto done;
  rc = flowie_control_advance_revision(database, current, &next);
  if (rc != SALTS_OK) goto done;
  rc = flowie_control_insert_audit(
      database, command->request_id, command->actor, FLOWIE_CONTROL_OPERATION_MEMBERSHIP_ADD,
      command->domain_id, command->principal_id, command->group_id, next, command->occurred_at);
  if (rc != SALTS_OK) goto done;
  result->revision = next;
  result->replayed = 0;

commit:
  status = flowie_control_database_exec(database, "COMMIT", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 0;
  rc = SALTS_OK;

done:
  if (statement) (void)flowie_control_database_finalize(statement);
  if (transaction_started)
    (void)flowie_control_database_exec(database, "ROLLBACK", NULL, NULL, NULL);
  (void)flowie_control_database_close(database);
  if (rc != SALTS_OK) *result = (flowie_control_command_result_t)FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  return rc;
}

int flowie_control_store_membership_remove(
    flowie_control_store_t *store, const flowie_control_membership_remove_command_t *command,
    flowie_control_command_result_t *result) {
  flowie_control_database_t *database = NULL;
  flowie_control_statement_t *statement = NULL;
  uint64_t current = 0u;
  uint64_t next = 0u;
  int transaction_started = 0;
  int found = 0;
  int status;
  int rc;
  if (result && result->size >= sizeof(*result))
    *result = (flowie_control_command_result_t)FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  if (!store || !command || command->size < sizeof(*command) || !result ||
      result->size < sizeof(*result) ||
      !flowie_control_command_common_valid(command->domain_id, command->principal_id,
                                           command->actor, command->request_id,
                                           command->expected_revision, command->occurred_at) ||
      !flowie_control_text_valid(command->group_id, FLOWIE_SECURITY_ID_MAX) ||
      strcmp(command->group_id, command->domain_id) == 0)
    return SALTS_EINVAL;
  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) return rc;
  status = flowie_control_database_exec(database, "BEGIN", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 1;
  rc = flowie_control_replay(database, command->request_id, command->actor,
                             FLOWIE_CONTROL_OPERATION_MEMBERSHIP_REMOVE, command->domain_id,
                             command->principal_id, command->group_id, result, &found);
  if (rc != SALTS_OK) goto done;
  if (found) goto commit;
  rc = flowie_control_read_revision(database, &current);
  if (rc != SALTS_OK) goto done;
  if (command->expected_revision != 0u && current != command->expected_revision) {
    rc = SALTS_EBUSY;
    goto done;
  }
  status = flowie_control_database_prepare(
      database,
      "DELETE FROM flowie_control_membership WHERE domain_id=?1 AND principal_id=?2 AND "
      "group_id=?3",
      -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  rc = flowie_control_bind_text(statement, 1, command->domain_id);
  if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 2, command->principal_id);
  if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 3, command->group_id);
  if (rc == SALTS_OK) {
    status = flowie_control_database_step(statement);
    rc = status == FLOWIE_CONTROL_DB_DONE && flowie_control_database_changes(database) == 1
             ? SALTS_OK
             : (status == FLOWIE_CONTROL_DB_DONE ? SALTS_ENOENT
                                                 : flowie_control_database_status(status));
  }
  (void)flowie_control_database_finalize(statement);
  statement = NULL;
  if (rc != SALTS_OK) goto done;
  rc = flowie_control_advance_revision(database, current, &next);
  if (rc != SALTS_OK) goto done;
  rc = flowie_control_insert_audit(
      database, command->request_id, command->actor, FLOWIE_CONTROL_OPERATION_MEMBERSHIP_REMOVE,
      command->domain_id, command->principal_id, command->group_id, next, command->occurred_at);
  if (rc != SALTS_OK) goto done;
  result->revision = next;
  result->replayed = 0;

commit:
  status = flowie_control_database_exec(database, "COMMIT", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 0;
  rc = SALTS_OK;

done:
  if (statement) (void)flowie_control_database_finalize(statement);
  if (transaction_started)
    (void)flowie_control_database_exec(database, "ROLLBACK", NULL, NULL, NULL);
  (void)flowie_control_database_close(database);
  if (rc != SALTS_OK) *result = (flowie_control_command_result_t)FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  return rc;
}

int flowie_control_store_effective_groups(flowie_control_store_t *store, const char *domain_id,
                                          const char *principal_id,
                                          flowie_control_effective_groups_view_t *out) {
  flowie_control_database_t *database = NULL;
  flowie_control_effective_groups_view_t view = FLOWIE_CONTROL_EFFECTIVE_GROUPS_VIEW_INIT;
  int enabled = 0;
  int rc;
  if (out && out->size >= sizeof(*out)) *out = view;
  if (!store || !flowie_control_text_valid(domain_id, FLOWIE_SECURITY_ID_MAX) ||
      !flowie_control_text_valid(principal_id, FLOWIE_SECURITY_ID_MAX) || !out ||
      out->size < sizeof(*out))
    return SALTS_EINVAL;
  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) return rc;
  rc = flowie_control_user_enabled(database, domain_id, principal_id, &enabled);
  if (rc == SALTS_OK && !enabled) rc = SALTS_EPERM;
  if (rc == SALTS_OK)
    rc = flowie_control_effective_groups_database(database, domain_id, principal_id, &view);
  (void)flowie_control_database_close(database);
  if (rc == SALTS_OK) *out = view;
  return rc;
}

int flowie_control_store_role_create(flowie_control_store_t *store,
                                     const flowie_control_role_create_command_t *command,
                                     flowie_control_command_result_t *result) {
  flowie_control_database_t *database = NULL;
  flowie_control_statement_t *statement = NULL;
  uint64_t current = 0u;
  uint64_t next = 0u;
  int transaction_started = 0;
  int found = 0;
  int status;
  int rc;
  if (result && result->size >= sizeof(*result))
    *result = (flowie_control_command_result_t)FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  if (!store || !command || command->size < sizeof(*command) || !result ||
      result->size < sizeof(*result) ||
      !flowie_control_command_common_valid(command->domain_id, command->role_id, command->actor,
                                           command->request_id, command->expected_revision,
                                           command->occurred_at) ||
      !flowie_control_text_valid(command->role_id, FLOWIE_SECURITY_TYPE_MAX))
    return SALTS_EINVAL;
  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) return rc;
  status = flowie_control_database_exec(database, "BEGIN", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 1;
  rc = flowie_control_replay(database, command->request_id, command->actor,
                             FLOWIE_CONTROL_OPERATION_ROLE_CREATE, command->domain_id,
                             command->role_id, FLOWIE_CONTROL_TARGET_ROLE, result, &found);
  if (rc != SALTS_OK) goto done;
  if (found) goto commit;
  rc = flowie_control_read_revision(database, &current);
  if (rc != SALTS_OK) goto done;
  if (command->expected_revision != 0u && current != command->expected_revision) {
    rc = SALTS_EBUSY;
    goto done;
  }
  rc = flowie_control_domain_exists(database, command->domain_id);
  if (rc != SALTS_OK) goto done;
  if (current >= (uint64_t)INT64_MAX) {
    rc = SALTS_ERANGE;
    goto done;
  }
  next = current + 1u;
  status = flowie_control_database_prepare(
      database,
      "INSERT INTO flowie_control_role(domain_id,role_id,enabled,revision,created_at,"
      "updated_at) VALUES(?1,?2,1,?3,?4,?4)",
      -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  rc = flowie_control_bind_text(statement, 1, command->domain_id);
  if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 2, command->role_id);
  if (rc == SALTS_OK &&
      flowie_control_database_bind_int64(statement, 3, (int64_t)next) != FLOWIE_CONTROL_DB_OK)
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
  if (rc == SALTS_OK && flowie_control_database_bind_int64(
                            statement, 4, (int64_t)command->occurred_at) != FLOWIE_CONTROL_DB_OK)
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
  if (rc == SALTS_OK) {
    status = flowie_control_database_step(statement);
    rc = status == FLOWIE_CONTROL_DB_DONE ? SALTS_OK
                                          : ((status & 0xff) == FLOWIE_CONTROL_DB_CONSTRAINT
                                                 ? SALTS_EALREADY
                                                 : flowie_control_database_status(status));
  }
  (void)flowie_control_database_finalize(statement);
  statement = NULL;
  if (rc != SALTS_OK) goto done;
  rc = flowie_control_advance_revision(database, current, &next);
  if (rc != SALTS_OK) goto done;
  rc = flowie_control_insert_audit(
      database, command->request_id, command->actor, FLOWIE_CONTROL_OPERATION_ROLE_CREATE,
      command->domain_id, command->role_id, FLOWIE_CONTROL_TARGET_ROLE, next, command->occurred_at);
  if (rc != SALTS_OK) goto done;
  result->revision = next;
  result->replayed = 0;

commit:
  status = flowie_control_database_exec(database, "COMMIT", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 0;
  rc = SALTS_OK;

done:
  if (statement) (void)flowie_control_database_finalize(statement);
  if (transaction_started)
    (void)flowie_control_database_exec(database, "ROLLBACK", NULL, NULL, NULL);
  (void)flowie_control_database_close(database);
  if (rc != SALTS_OK) *result = (flowie_control_command_result_t)FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  return rc;
}

int flowie_control_store_role_disable(flowie_control_store_t *store,
                                      const flowie_control_role_disable_command_t *command,
                                      flowie_control_command_result_t *result) {
  flowie_control_database_t *database = NULL;
  flowie_control_statement_t *statement = NULL;
  uint64_t current = 0u;
  uint64_t next = 0u;
  int role_enabled = 0;
  int policy_reference = 0;
  int transaction_started = 0;
  int found = 0;
  int status;
  int rc;
  if (result && result->size >= sizeof(*result))
    *result = (flowie_control_command_result_t)FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  if (!store || !command || command->size < sizeof(*command) || !result ||
      result->size < sizeof(*result) ||
      !flowie_control_command_common_valid(command->domain_id, command->role_id, command->actor,
                                           command->request_id, command->expected_revision,
                                           command->occurred_at) ||
      !flowie_control_text_valid(command->role_id, FLOWIE_SECURITY_TYPE_MAX))
    return SALTS_EINVAL;
  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) return rc;
  status = flowie_control_database_exec(database, "BEGIN", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 1;
  rc = flowie_control_replay(database, command->request_id, command->actor,
                             FLOWIE_CONTROL_OPERATION_ROLE_DISABLE, command->domain_id,
                             command->role_id, FLOWIE_CONTROL_TARGET_ROLE, result, &found);
  if (rc != SALTS_OK) goto done;
  if (found) goto commit;
  rc = flowie_control_read_revision(database, &current);
  if (rc != SALTS_OK) goto done;
  if (command->expected_revision != 0u && current != command->expected_revision) {
    rc = SALTS_EBUSY;
    goto done;
  }
  rc = flowie_control_role_enabled(database, command->domain_id, command->role_id, &role_enabled);
  if (rc != SALTS_OK) goto done;
  if (!role_enabled) {
    rc = SALTS_EALREADY;
    goto done;
  }
  rc = flowie_control_policy_subject_referenced(database, command->domain_id,
                                                FLOWIE_SECURITY_SUBJECT_ROLE, command->role_id,
                                                &policy_reference);
  if (rc != SALTS_OK) goto done;
  if (policy_reference) {
    rc = SALTS_EBUSY;
    goto done;
  }
  rc = flowie_control_advance_revision(database, current, &next);
  if (rc != SALTS_OK) goto done;
  status = flowie_control_database_prepare(
      database,
      "UPDATE flowie_control_role SET enabled=0,revision=?1,updated_at=?2 WHERE domain_id=?3 "
      "AND role_id=?4 AND enabled=1",
      -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  if (flowie_control_database_bind_int64(statement, 1, (int64_t)next) != FLOWIE_CONTROL_DB_OK ||
      flowie_control_database_bind_int64(statement, 2, (int64_t)command->occurred_at) !=
          FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
    goto done;
  }
  rc = flowie_control_bind_text(statement, 3, command->domain_id);
  if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 4, command->role_id);
  if (rc == SALTS_OK) {
    status = flowie_control_database_step(statement);
    rc = status == FLOWIE_CONTROL_DB_DONE && flowie_control_database_changes(database) == 1
             ? SALTS_OK
             : (status == FLOWIE_CONTROL_DB_DONE ? SALTS_EBUSY
                                                 : flowie_control_database_status(status));
  }
  (void)flowie_control_database_finalize(statement);
  statement = NULL;
  if (rc != SALTS_OK) goto done;
  rc = flowie_control_insert_audit(
      database, command->request_id, command->actor, FLOWIE_CONTROL_OPERATION_ROLE_DISABLE,
      command->domain_id, command->role_id, FLOWIE_CONTROL_TARGET_ROLE, next, command->occurred_at);
  if (rc != SALTS_OK) goto done;
  result->revision = next;
  result->replayed = 0;

commit:
  status = flowie_control_database_exec(database, "COMMIT", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 0;
  rc = SALTS_OK;

done:
  if (statement) (void)flowie_control_database_finalize(statement);
  if (transaction_started)
    (void)flowie_control_database_exec(database, "ROLLBACK", NULL, NULL, NULL);
  (void)flowie_control_database_close(database);
  if (rc != SALTS_OK) *result = (flowie_control_command_result_t)FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  return rc;
}

int flowie_control_store_user_role_add(flowie_control_store_t *store,
                                       const flowie_control_user_role_add_command_t *command,
                                       flowie_control_command_result_t *result) {
  flowie_control_database_t *database = NULL;
  flowie_control_statement_t *statement = NULL;
  flowie_control_effective_roles_view_t effective = FLOWIE_CONTROL_EFFECTIVE_ROLES_VIEW_INIT;
  uint64_t current = 0u;
  uint64_t next = 0u;
  int user_enabled = 0;
  int role_enabled = 0;
  int transaction_started = 0;
  int found = 0;
  int status;
  int rc;
  if (result && result->size >= sizeof(*result))
    *result = (flowie_control_command_result_t)FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  if (!store || !command || command->size < sizeof(*command) || !result ||
      result->size < sizeof(*result) ||
      !flowie_control_command_common_valid(command->domain_id, command->principal_id,
                                           command->actor, command->request_id,
                                           command->expected_revision, command->occurred_at) ||
      !flowie_control_text_valid(command->role_id, FLOWIE_SECURITY_TYPE_MAX))
    return SALTS_EINVAL;
  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) return rc;
  status = flowie_control_database_exec(database, "BEGIN", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 1;
  rc = flowie_control_replay(database, command->request_id, command->actor,
                             FLOWIE_CONTROL_OPERATION_USER_ROLE_ADD, command->domain_id,
                             command->principal_id, command->role_id, result, &found);
  if (rc != SALTS_OK) goto done;
  if (found) goto commit;
  rc = flowie_control_read_revision(database, &current);
  if (rc != SALTS_OK) goto done;
  if (command->expected_revision != 0u && current != command->expected_revision) {
    rc = SALTS_EBUSY;
    goto done;
  }
  rc = flowie_control_user_enabled(database, command->domain_id, command->principal_id,
                                   &user_enabled);
  if (rc != SALTS_OK) goto done;
  if (!user_enabled) {
    rc = SALTS_EPERM;
    goto done;
  }
  rc = flowie_control_role_enabled(database, command->domain_id, command->role_id, &role_enabled);
  if (rc != SALTS_OK) goto done;
  if (!role_enabled) {
    rc = SALTS_EPERM;
    goto done;
  }
  if (current >= (uint64_t)INT64_MAX) {
    rc = SALTS_ERANGE;
    goto done;
  }
  next = current + 1u;
  status = flowie_control_database_prepare(
      database,
      "INSERT INTO "
      "flowie_control_user_role(domain_id,principal_id,role_id,revision,created_at) "
      "VALUES(?1,?2,?3,?4,?5)",
      -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  rc = flowie_control_bind_text(statement, 1, command->domain_id);
  if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 2, command->principal_id);
  if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 3, command->role_id);
  if (rc == SALTS_OK &&
      flowie_control_database_bind_int64(statement, 4, (int64_t)next) != FLOWIE_CONTROL_DB_OK)
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
  if (rc == SALTS_OK && flowie_control_database_bind_int64(
                            statement, 5, (int64_t)command->occurred_at) != FLOWIE_CONTROL_DB_OK)
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
  if (rc == SALTS_OK) {
    status = flowie_control_database_step(statement);
    rc = status == FLOWIE_CONTROL_DB_DONE ? SALTS_OK
                                          : ((status & 0xff) == FLOWIE_CONTROL_DB_CONSTRAINT
                                                 ? SALTS_EALREADY
                                                 : flowie_control_database_status(status));
  }
  (void)flowie_control_database_finalize(statement);
  statement = NULL;
  if (rc != SALTS_OK) goto done;
  rc = flowie_control_effective_roles_database(database, command->domain_id, command->principal_id,
                                               &effective);
  if (rc != SALTS_OK) goto done;
  rc = flowie_control_advance_revision(database, current, &next);
  if (rc != SALTS_OK) goto done;
  rc = flowie_control_insert_audit(
      database, command->request_id, command->actor, FLOWIE_CONTROL_OPERATION_USER_ROLE_ADD,
      command->domain_id, command->principal_id, command->role_id, next, command->occurred_at);
  if (rc != SALTS_OK) goto done;
  result->revision = next;
  result->replayed = 0;

commit:
  status = flowie_control_database_exec(database, "COMMIT", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 0;
  rc = SALTS_OK;

done:
  if (statement) (void)flowie_control_database_finalize(statement);
  if (transaction_started)
    (void)flowie_control_database_exec(database, "ROLLBACK", NULL, NULL, NULL);
  (void)flowie_control_database_close(database);
  if (rc != SALTS_OK) *result = (flowie_control_command_result_t)FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  return rc;
}

int flowie_control_store_user_role_remove(flowie_control_store_t *store,
                                          const flowie_control_user_role_remove_command_t *command,
                                          flowie_control_command_result_t *result) {
  flowie_control_database_t *database = NULL;
  flowie_control_statement_t *statement = NULL;
  uint64_t current = 0u;
  uint64_t next = 0u;
  int transaction_started = 0;
  int found = 0;
  int status;
  int rc;
  if (result && result->size >= sizeof(*result))
    *result = (flowie_control_command_result_t)FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  if (!store || !command || command->size < sizeof(*command) || !result ||
      result->size < sizeof(*result) ||
      !flowie_control_command_common_valid(command->domain_id, command->principal_id,
                                           command->actor, command->request_id,
                                           command->expected_revision, command->occurred_at) ||
      !flowie_control_text_valid(command->role_id, FLOWIE_SECURITY_TYPE_MAX))
    return SALTS_EINVAL;
  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) return rc;
  status = flowie_control_database_exec(database, "BEGIN", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 1;
  rc = flowie_control_replay(database, command->request_id, command->actor,
                             FLOWIE_CONTROL_OPERATION_USER_ROLE_REMOVE, command->domain_id,
                             command->principal_id, command->role_id, result, &found);
  if (rc != SALTS_OK) goto done;
  if (found) goto commit;
  rc = flowie_control_read_revision(database, &current);
  if (rc != SALTS_OK) goto done;
  if (command->expected_revision != 0u && current != command->expected_revision) {
    rc = SALTS_EBUSY;
    goto done;
  }
  status = flowie_control_database_prepare(
      database,
      "DELETE FROM flowie_control_user_role WHERE domain_id=?1 AND principal_id=?2 AND "
      "role_id=?3",
      -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  rc = flowie_control_bind_text(statement, 1, command->domain_id);
  if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 2, command->principal_id);
  if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 3, command->role_id);
  if (rc == SALTS_OK) {
    status = flowie_control_database_step(statement);
    rc = status == FLOWIE_CONTROL_DB_DONE && flowie_control_database_changes(database) == 1
             ? SALTS_OK
             : (status == FLOWIE_CONTROL_DB_DONE ? SALTS_ENOENT
                                                 : flowie_control_database_status(status));
  }
  (void)flowie_control_database_finalize(statement);
  statement = NULL;
  if (rc != SALTS_OK) goto done;
  rc = flowie_control_advance_revision(database, current, &next);
  if (rc != SALTS_OK) goto done;
  rc = flowie_control_insert_audit(
      database, command->request_id, command->actor, FLOWIE_CONTROL_OPERATION_USER_ROLE_REMOVE,
      command->domain_id, command->principal_id, command->role_id, next, command->occurred_at);
  if (rc != SALTS_OK) goto done;
  result->revision = next;
  result->replayed = 0;

commit:
  status = flowie_control_database_exec(database, "COMMIT", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 0;
  rc = SALTS_OK;

done:
  if (statement) (void)flowie_control_database_finalize(statement);
  if (transaction_started)
    (void)flowie_control_database_exec(database, "ROLLBACK", NULL, NULL, NULL);
  (void)flowie_control_database_close(database);
  if (rc != SALTS_OK) *result = (flowie_control_command_result_t)FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  return rc;
}

int flowie_control_store_effective_roles(flowie_control_store_t *store, const char *domain_id,
                                         const char *principal_id,
                                         flowie_control_effective_roles_view_t *out) {
  flowie_control_database_t *database = NULL;
  flowie_control_effective_roles_view_t view = FLOWIE_CONTROL_EFFECTIVE_ROLES_VIEW_INIT;
  int enabled = 0;
  int rc;
  if (out && out->size >= sizeof(*out)) *out = view;
  if (!store || !flowie_control_text_valid(domain_id, FLOWIE_SECURITY_ID_MAX) ||
      !flowie_control_text_valid(principal_id, FLOWIE_SECURITY_ID_MAX) || !out ||
      out->size < sizeof(*out))
    return SALTS_EINVAL;
  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) return rc;
  rc = flowie_control_user_enabled(database, domain_id, principal_id, &enabled);
  if (rc == SALTS_OK && !enabled) rc = SALTS_EPERM;
  if (rc == SALTS_OK)
    rc = flowie_control_effective_roles_database(database, domain_id, principal_id, &view);
  (void)flowie_control_database_close(database);
  if (rc == SALTS_OK) *out = view;
  return rc;
}

int flowie_control_store_policy_subject_rule_put(
    flowie_control_store_t *store, const flowie_control_policy_subject_rule_put_command_t *command,
    flowie_control_command_result_t *result) {
  flowie_control_database_t *database = NULL;
  flowie_control_statement_t *statement = NULL;
  const flowie_control_acl_document_t *document;
  size_t expanded_rule_count = 0u;
  size_t deny_rule_count = 0u;
  char *canonical = NULL;
  size_t canonical_size = 0u;
  uint64_t current = 0u;
  uint64_t next = 0u;
  int transaction_started = 0;
  int found = 0;
  int status;
  int rc;
  if (result && result->size >= sizeof(*result))
    *result = (flowie_control_command_result_t)FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  if (!store || !command || command->size < sizeof(*command) || !result ||
      result->size < sizeof(*result) || command->ordinal >= FLOWIE_SECURITY_MAX_RULES ||
      !command->document)
    return SALTS_EINVAL;
  canonical = (char *)malloc(FLOWIE_CONTROL_ACL_DOCUMENT_MAX + 1u);
  if (!canonical) return SALTS_ENOMEM;
  rc = flowie_control_acl_format(command->document, canonical,
                                 FLOWIE_CONTROL_ACL_DOCUMENT_MAX + 1u, &canonical_size);
  if (rc != SALTS_OK) goto done;
  canonical[canonical_size] = '\0';
  document = command->document;
  if (!flowie_control_command_common_valid(command->domain_id, document->subject, command->actor,
                                           command->request_id, command->expected_revision,
                                           command->occurred_at)) {
    rc = SALTS_EINVAL;
    goto done;
  }
  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) goto done;
  status = flowie_control_database_exec(database, "BEGIN", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 1;
  rc = flowie_control_replay(database, command->request_id, command->actor,
                             FLOWIE_CONTROL_OPERATION_POLICY_SUBJECT_RULE_PUT, command->domain_id,
                             document->subject, canonical, result, &found);
  if (rc != SALTS_OK) goto done;
  if (found) goto commit;
  rc = flowie_control_read_revision(database, &current);
  if (rc != SALTS_OK) goto done;
  if (command->expected_revision != 0u && current != command->expected_revision) {
    rc = SALTS_EBUSY;
    goto done;
  }
  rc = flowie_control_policy_document_validate(database, command->domain_id, canonical,
                                               canonical_size, NULL, &expanded_rule_count,
                                               &deny_rule_count);
  if (rc != SALTS_OK) goto done;
  if (current >= (uint64_t)INT64_MAX) {
    rc = SALTS_ERANGE;
    goto done;
  }
  next = current + 1u;
  status = flowie_control_database_prepare(
      database,
      "INSERT INTO flowie_control_policy_draft(domain_id,subject_kind,subject_id,ordinal,"
      "rule_document,revision,updated_at) VALUES(?1,?2,?3,?4,?5,?6,?7) "
      "ON CONFLICT(domain_id,subject_kind,subject_id) DO UPDATE SET ordinal=excluded.ordinal,"
      "rule_document=excluded.rule_document,revision=excluded.revision,"
      "updated_at=excluded.updated_at",
      -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  rc = flowie_control_bind_text(statement, 1, command->domain_id);
  if (rc == SALTS_OK && flowie_control_database_bind_int(
                            statement, 2, (int)document->subject_kind) != FLOWIE_CONTROL_DB_OK)
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
  if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 3, document->subject);
  if (rc == SALTS_OK &&
      flowie_control_database_bind_int64(statement, 4, command->ordinal) != FLOWIE_CONTROL_DB_OK)
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
  if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 5, canonical);
  if (rc == SALTS_OK &&
      flowie_control_database_bind_int64(statement, 6, (int64_t)next) != FLOWIE_CONTROL_DB_OK)
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
  if (rc == SALTS_OK && flowie_control_database_bind_int64(
                            statement, 7, (int64_t)command->occurred_at) != FLOWIE_CONTROL_DB_OK)
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
  if (rc == SALTS_OK) {
    status = flowie_control_database_step(statement);
    rc = status == FLOWIE_CONTROL_DB_DONE ? SALTS_OK : flowie_control_database_status(status);
  }
  (void)flowie_control_database_finalize(statement);
  statement = NULL;
  if (rc != SALTS_OK) goto done;
  rc = flowie_control_advance_revision(database, current, &next);
  if (rc != SALTS_OK) goto done;
  rc = flowie_control_insert_audit(database, command->request_id, command->actor,
                                   FLOWIE_CONTROL_OPERATION_POLICY_SUBJECT_RULE_PUT,
                                   command->domain_id, document->subject, canonical, next,
                                   command->occurred_at);
  if (rc != SALTS_OK) goto done;
  result->revision = next;
  result->replayed = 0;

commit:
  status = flowie_control_database_exec(database, "COMMIT", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 0;
  rc = SALTS_OK;

done:
  if (statement) (void)flowie_control_database_finalize(statement);
  if (transaction_started)
    (void)flowie_control_database_exec(database, "ROLLBACK", NULL, NULL, NULL);
  if (database) (void)flowie_control_database_close(database);
  free(canonical);
  if (rc != SALTS_OK) *result = (flowie_control_command_result_t)FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  return rc;
}

int flowie_control_store_policy_validate(flowie_control_store_t *store, const char *domain_id,
                                         flowie_control_policy_validation_t *out) {
  flowie_control_database_t *database = NULL;
  flowie_control_policy_validation_t validation = FLOWIE_CONTROL_POLICY_VALIDATION_INIT;
  int transaction_started = 0;
  int status;
  int rc;
  if (out && out->size >= sizeof(*out))
    *out = (flowie_control_policy_validation_t)FLOWIE_CONTROL_POLICY_VALIDATION_INIT;
  if (!store || !flowie_control_text_valid(domain_id, FLOWIE_SECURITY_ID_MAX) || !out ||
      out->size < sizeof(*out))
    return SALTS_EINVAL;
  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) return rc;
  status = flowie_control_database_exec(database, "BEGIN", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 1;
  rc = flowie_control_policy_validate_database(database, domain_id, &validation);
  if (rc != SALTS_OK) goto done;
  status = flowie_control_database_exec(database, "COMMIT", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 0;
  *out = validation;
  rc = SALTS_OK;

done:
  if (transaction_started)
    (void)flowie_control_database_exec(database, "ROLLBACK", NULL, NULL, NULL);
  (void)flowie_control_database_close(database);
  return rc;
}

int flowie_control_store_policy_dry_run(flowie_control_store_t *store, const char *domain_id,
                                        const flowie_control_policy_dry_run_change_t *changes,
                                        size_t change_count,
                                        flowie_control_policy_dry_run_result_t *result) {
  flowie_control_database_t *database = NULL;
  flowie_control_policy_dry_run_result_t dry_run = FLOWIE_CONTROL_POLICY_DRY_RUN_RESULT_INIT;
  int transaction_started = 0;
  int status;
  int rc;
  if (!store || !flowie_control_text_valid(domain_id, FLOWIE_SECURITY_ID_MAX) || !changes ||
      change_count == 0u || change_count > FLOWIE_CONTROL_POLICY_DRY_RUN_MAX_CHANGES || !result ||
      result->size < sizeof(*result) || (result->diagnostic_capacity != 0u && !result->diagnostics))
    return SALTS_EINVAL;
  dry_run.diagnostics = result->diagnostics;
  dry_run.diagnostic_capacity = result->diagnostic_capacity;
  *result = dry_run;
  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) return rc;
  status = flowie_control_database_exec(database, "BEGIN", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 1;
  rc = flowie_control_policy_dry_run_database(database, domain_id, changes, change_count, &dry_run);
  if (rc != SALTS_OK) goto done;
  status = flowie_control_database_exec(database, "COMMIT", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 0;
  *result = dry_run;
  rc = SALTS_OK;

done:
  if (transaction_started)
    (void)flowie_control_database_exec(database, "ROLLBACK", NULL, NULL, NULL);
  (void)flowie_control_database_close(database);
  return rc;
}

int flowie_control_store_policy_subject_rule_get(flowie_control_store_t *store,
                                                 const char *domain_id,
                                                 flowie_security_subject_kind_t subject_kind,
                                                 const char *subject_id,
                                                 flowie_control_policy_subject_rule_view_t *out) {
  flowie_control_database_t *database = NULL;
  flowie_control_statement_t *statement = NULL;
  flowie_control_policy_subject_rule_view_t view = FLOWIE_CONTROL_POLICY_SUBJECT_RULE_VIEW_INIT;
  const unsigned char *text;
  int64_t ordinal;
  int64_t revision;
  int64_t updated_at;
  int text_size;
  int status;
  int rc;
  if (out && out->size >= sizeof(*out)) *out = view;
  if (!store || !flowie_control_text_valid(domain_id, FLOWIE_SECURITY_ID_MAX) ||
      !flowie_control_text_valid(subject_id, FLOWIE_SECURITY_ID_MAX) || !out ||
      out->size < sizeof(*out) ||
      (subject_kind != FLOWIE_SECURITY_SUBJECT_PRINCIPAL &&
       subject_kind != FLOWIE_SECURITY_SUBJECT_ROLE &&
       subject_kind != FLOWIE_SECURITY_SUBJECT_GROUP))
    return SALTS_EINVAL;
  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) return rc;
  status = flowie_control_database_prepare(
      database,
      "SELECT ordinal,rule_document,revision,updated_at FROM flowie_control_policy_draft "
      "WHERE domain_id=?1 AND subject_kind=?2 AND subject_id=?3",
      -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  rc = flowie_control_bind_text(statement, 1, domain_id);
  if (rc == SALTS_OK &&
      flowie_control_database_bind_int(statement, 2, (int)subject_kind) != FLOWIE_CONTROL_DB_OK)
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
  if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 3, subject_id);
  if (rc != SALTS_OK) goto done;
  status = flowie_control_database_step(statement);
  if (status == FLOWIE_CONTROL_DB_DONE) {
    rc = SALTS_ENOENT;
    goto done;
  }
  if (status != FLOWIE_CONTROL_DB_ROW ||
      flowie_control_database_column_type(statement, 0) != FLOWIE_CONTROL_DB_INTEGER ||
      flowie_control_database_column_type(statement, 1) != FLOWIE_CONTROL_DB_TEXT ||
      flowie_control_database_column_type(statement, 2) != FLOWIE_CONTROL_DB_INTEGER ||
      flowie_control_database_column_type(statement, 3) != FLOWIE_CONTROL_DB_INTEGER ||
      (ordinal = flowie_control_database_column_int64(statement, 0)) < 0 ||
      ordinal >= FLOWIE_SECURITY_MAX_RULES ||
      !(text = flowie_control_database_column_text(statement, 1)) ||
      (text_size = flowie_control_database_column_bytes(statement, 1)) <= 0 ||
      (revision = flowie_control_database_column_int64(statement, 2)) <= 0 ||
      (updated_at = flowie_control_database_column_int64(statement, 3)) <= 0) {
    rc = SALTS_EPROTO;
    goto done;
  }
  rc = flowie_control_acl_parse((const char *)text, (size_t)text_size, &view.document);
  if (rc == SALTS_OK && (view.document.subject_kind != subject_kind ||
                         strcmp(view.document.subject, subject_id) != 0))
    rc = SALTS_EPROTO;
  if (rc == SALTS_OK && flowie_control_database_step(statement) != FLOWIE_CONTROL_DB_DONE)
    rc = SALTS_EPROTO;
  if (rc == SALTS_OK) {
    view.ordinal = (uint32_t)ordinal;
    view.revision = (uint64_t)revision;
    view.updated_at = (uint64_t)updated_at;
    *out = view;
  }

done:
  (void)flowie_control_database_finalize(statement);
  (void)flowie_control_database_close(database);
  return rc;
}

int flowie_control_store_policy_subject_rule_list(flowie_control_store_t *store,
                                                  const char *domain_id,
                                                  flowie_security_subject_kind_t subject_kind,
                                                  uint32_t after_ordinal, int has_after,
                                                  flowie_control_policy_subject_rule_view_t *items,
                                                  size_t item_capacity, size_t *count_out,
                                                  int *has_more_out) {
  flowie_control_database_t *database = NULL;
  flowie_control_statement_t *statement = NULL;
  size_t count = 0u;
  int status;
  int rc;
  if (count_out) *count_out = 0u;
  if (has_more_out) *has_more_out = 0;
  if (!store || !flowie_control_text_valid(domain_id, FLOWIE_SECURITY_ID_MAX) ||
      (subject_kind != FLOWIE_SECURITY_SUBJECT_ANY &&
       subject_kind != FLOWIE_SECURITY_SUBJECT_PRINCIPAL &&
       subject_kind != FLOWIE_SECURITY_SUBJECT_ROLE &&
       subject_kind != FLOWIE_SECURITY_SUBJECT_GROUP) ||
      (has_after != 0 && has_after != 1) || !items || item_capacity == 0u ||
      item_capacity > FLOWIE_CONTROL_PAGE_MAX || !count_out || !has_more_out)
    return SALTS_EINVAL;
  for (size_t index = 0u; index < item_capacity; ++index) {
    if (items[index].size < sizeof(items[index])) return SALTS_EINVAL;
    items[index] =
        (flowie_control_policy_subject_rule_view_t)FLOWIE_CONTROL_POLICY_SUBJECT_RULE_VIEW_INIT;
  }
  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) return rc;
  status = flowie_control_database_prepare(
      database,
      "SELECT ordinal,subject_kind,subject_id,rule_document,revision,updated_at FROM "
      "flowie_control_policy_draft WHERE domain_id=?1 AND (?2=0 OR subject_kind=?2) "
      "AND (?3=0 OR ordinal>?4) ORDER BY ordinal LIMIT ?5",
      -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  rc = flowie_control_bind_text(statement, 1, domain_id);
  if (rc == SALTS_OK &&
      flowie_control_database_bind_int(statement, 2, (int)subject_kind) != FLOWIE_CONTROL_DB_OK)
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
  if (rc == SALTS_OK &&
      flowie_control_database_bind_int(statement, 3, has_after) != FLOWIE_CONTROL_DB_OK)
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
  if (rc == SALTS_OK &&
      flowie_control_database_bind_int64(statement, 4, after_ordinal) != FLOWIE_CONTROL_DB_OK)
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
  if (rc == SALTS_OK && flowie_control_database_bind_int64(
                            statement, 5, (int64_t)(item_capacity + 1u)) != FLOWIE_CONTROL_DB_OK)
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
  if (rc != SALTS_OK) goto done;
  while ((status = flowie_control_database_step(statement)) == FLOWIE_CONTROL_DB_ROW) {
    flowie_control_policy_subject_rule_view_t *view;
    const unsigned char *text;
    int64_t ordinal;
    int64_t revision;
    int64_t updated_at;
    int stored_kind;
    int text_size;
    if (count == item_capacity) {
      *has_more_out = 1;
      continue;
    }
    if (flowie_control_database_column_type(statement, 0) != FLOWIE_CONTROL_DB_INTEGER ||
        flowie_control_database_column_type(statement, 1) != FLOWIE_CONTROL_DB_INTEGER ||
        flowie_control_database_column_type(statement, 2) != FLOWIE_CONTROL_DB_TEXT ||
        flowie_control_database_column_type(statement, 3) != FLOWIE_CONTROL_DB_TEXT ||
        flowie_control_database_column_type(statement, 4) != FLOWIE_CONTROL_DB_INTEGER ||
        flowie_control_database_column_type(statement, 5) != FLOWIE_CONTROL_DB_INTEGER ||
        (ordinal = flowie_control_database_column_int64(statement, 0)) < 0 ||
        ordinal >= FLOWIE_SECURITY_MAX_RULES ||
        !(text = flowie_control_database_column_text(statement, 3)) ||
        (text_size = flowie_control_database_column_bytes(statement, 3)) <= 0 ||
        (revision = flowie_control_database_column_int64(statement, 4)) <= 0 ||
        (updated_at = flowie_control_database_column_int64(statement, 5)) <= 0) {
      rc = SALTS_EPROTO;
      goto done;
    }
    stored_kind = flowie_control_database_column_int(statement, 1);
    view = &items[count];
    rc = flowie_control_acl_parse((const char *)text, (size_t)text_size, &view->document);
    if (rc != SALTS_OK || stored_kind != (int)view->document.subject_kind ||
        !flowie_control_column_text_equal(statement, 2, view->document.subject)) {
      rc = SALTS_EPROTO;
      goto done;
    }
    view->ordinal = (uint32_t)ordinal;
    view->revision = (uint64_t)revision;
    view->updated_at = (uint64_t)updated_at;
    ++count;
  }
  if (status != FLOWIE_CONTROL_DB_DONE) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  *count_out = count;
  rc = SALTS_OK;

done:
  (void)flowie_control_database_finalize(statement);
  (void)flowie_control_database_close(database);
  if (rc != SALTS_OK) {
    *count_out = 0u;
    *has_more_out = 0;
  }
  return rc;
}

int flowie_control_store_policy_subject_rule_delete(
    flowie_control_store_t *store,
    const flowie_control_policy_subject_rule_delete_command_t *command,
    flowie_control_command_result_t *result) {
  flowie_control_database_t *database = NULL;
  flowie_control_statement_t *statement = NULL;
  const char *kind_text;
  uint64_t current = 0u;
  uint64_t next = 0u;
  int transaction_started = 0;
  int found = 0;
  int status;
  int rc;
  if (result && result->size >= sizeof(*result))
    *result = (flowie_control_command_result_t)FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  kind_text = command ? flowie_control_policy_subject_kind_text(command->subject_kind) : NULL;
  if (!store || !command || command->size < sizeof(*command) || !result ||
      result->size < sizeof(*result) || !kind_text ||
      !flowie_control_command_common_valid(command->domain_id, command->subject_id, command->actor,
                                           command->request_id, command->expected_revision,
                                           command->occurred_at))
    return SALTS_EINVAL;
  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) return rc;
  status = flowie_control_database_exec(database, "BEGIN", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 1;
  rc = flowie_control_replay(database, command->request_id, command->actor,
                             FLOWIE_CONTROL_OPERATION_POLICY_SUBJECT_RULE_DELETE,
                             command->domain_id, command->subject_id, kind_text, result, &found);
  if (rc != SALTS_OK) goto done;
  if (found) goto commit;
  rc = flowie_control_read_revision(database, &current);
  if (rc != SALTS_OK) goto done;
  if (command->expected_revision != 0u && current != command->expected_revision) {
    rc = SALTS_EBUSY;
    goto done;
  }
  status = flowie_control_database_prepare(
      database,
      "DELETE FROM flowie_control_policy_draft WHERE domain_id=?1 AND subject_kind=?2 AND "
      "subject_id=?3",
      -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  rc = flowie_control_bind_text(statement, 1, command->domain_id);
  if (rc == SALTS_OK && flowie_control_database_bind_int(
                            statement, 2, (int)command->subject_kind) != FLOWIE_CONTROL_DB_OK)
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
  if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 3, command->subject_id);
  if (rc == SALTS_OK) {
    status = flowie_control_database_step(statement);
    rc = status == FLOWIE_CONTROL_DB_DONE && flowie_control_database_changes(database) == 1
             ? SALTS_OK
             : (status == FLOWIE_CONTROL_DB_DONE ? SALTS_ENOENT
                                                 : flowie_control_database_status(status));
  }
  (void)flowie_control_database_finalize(statement);
  statement = NULL;
  if (rc != SALTS_OK) goto done;
  rc = flowie_control_advance_revision(database, current, &next);
  if (rc != SALTS_OK) goto done;
  rc = flowie_control_insert_audit(database, command->request_id, command->actor,
                                   FLOWIE_CONTROL_OPERATION_POLICY_SUBJECT_RULE_DELETE,
                                   command->domain_id, command->subject_id, kind_text, next,
                                   command->occurred_at);
  if (rc != SALTS_OK) goto done;
  result->revision = next;
  result->replayed = 0;

commit:
  status = flowie_control_database_exec(database, "COMMIT", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 0;
  rc = SALTS_OK;

done:
  if (statement) (void)flowie_control_database_finalize(statement);
  if (transaction_started)
    (void)flowie_control_database_exec(database, "ROLLBACK", NULL, NULL, NULL);
  (void)flowie_control_database_close(database);
  if (rc != SALTS_OK) *result = (flowie_control_command_result_t)FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  return rc;
}

int flowie_control_store_policy_status(flowie_control_store_t *store, const char *domain_id,
                                       flowie_control_policy_status_t *out) {
  flowie_control_policy_status_t view = FLOWIE_CONTROL_POLICY_STATUS_INIT;
  flowie_control_database_t *database = NULL;
  flowie_control_statement_t *statement = NULL;
  int64_t draft_count;
  int64_t published_count;
  int status;
  int rc;
  if (out && out->size >= sizeof(*out))
    *out = (flowie_control_policy_status_t)FLOWIE_CONTROL_POLICY_STATUS_INIT;
  if (!store || !flowie_control_text_valid(domain_id, FLOWIE_SECURITY_ID_MAX) || !out ||
      out->size < sizeof(*out))
    return SALTS_EINVAL;
  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) return rc;
  rc = flowie_control_read_revision(database, &view.store_revision);
  if (rc != SALTS_OK) goto done;
  status = flowie_control_database_prepare(
      database,
      "SELECT (SELECT COUNT(*) FROM flowie_control_policy_draft WHERE domain_id=?1),"
      "b.policy_version,b.expires_at,(SELECT COUNT(*) FROM flowie_control_published_rule "
      "WHERE namespace_name=?1) FROM flowie_control_published_bundle b WHERE b.namespace_name=?1",
      -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  rc = flowie_control_bind_text(statement, 1, domain_id);
  if (rc != SALTS_OK) goto done;
  status = flowie_control_database_step(statement);
  if (status == FLOWIE_CONTROL_DB_DONE) {
    (void)flowie_control_database_finalize(statement);
    statement = NULL;
    status = flowie_control_database_prepare(
        database, "SELECT COUNT(*) FROM flowie_control_policy_draft WHERE domain_id=?1", -1,
        &statement, NULL);
    if (status != FLOWIE_CONTROL_DB_OK) {
      rc = flowie_control_database_status(status);
      goto done;
    }
    rc = flowie_control_bind_text(statement, 1, domain_id);
    if (rc != SALTS_OK) goto done;
    status = flowie_control_database_step(statement);
    if (status != FLOWIE_CONTROL_DB_ROW ||
        flowie_control_database_column_type(statement, 0) != FLOWIE_CONTROL_DB_INTEGER ||
        (draft_count = flowie_control_database_column_int64(statement, 0)) < 0 ||
        (uint64_t)draft_count > SIZE_MAX) {
      rc = status == FLOWIE_CONTROL_DB_ROW ? SALTS_EPROTO : flowie_control_database_status(status);
      goto done;
    }
    view.draft_rule_count = (size_t)draft_count;
  } else {
    if (status != FLOWIE_CONTROL_DB_ROW ||
        flowie_control_database_column_type(statement, 0) != FLOWIE_CONTROL_DB_INTEGER ||
        flowie_control_database_column_type(statement, 1) != FLOWIE_CONTROL_DB_INTEGER ||
        flowie_control_database_column_type(statement, 2) != FLOWIE_CONTROL_DB_INTEGER ||
        flowie_control_database_column_type(statement, 3) != FLOWIE_CONTROL_DB_INTEGER ||
        (draft_count = flowie_control_database_column_int64(statement, 0)) < 0 ||
        flowie_control_database_column_int64(statement, 1) <= 0 ||
        flowie_control_database_column_int64(statement, 2) < 0 ||
        (published_count = flowie_control_database_column_int64(statement, 3)) < 0 ||
        (uint64_t)draft_count > SIZE_MAX || (uint64_t)published_count > SIZE_MAX) {
      rc = status == FLOWIE_CONTROL_DB_ROW ? SALTS_EPROTO : flowie_control_database_status(status);
      goto done;
    }
    view.draft_rule_count = (size_t)draft_count;
    view.policy_version = (uint64_t)flowie_control_database_column_int64(statement, 1);
    view.expires_at = (uint64_t)flowie_control_database_column_int64(statement, 2);
    view.published_rule_count = (size_t)published_count;
  }
  *out = view;
  rc = SALTS_OK;

done:
  if (statement) (void)flowie_control_database_finalize(statement);
  (void)flowie_control_database_close(database);
  return rc;
}

void flowie_control_store_policy_bundle_release(flowie_security_policy_bundle_t *bundle) {
  flowie_control_policy_bundle_owner_t *owner;
  if (!bundle) return;
  owner = (flowie_control_policy_bundle_owner_t *)bundle->provider_bundle;
  if (owner) {
    free(owner->rules);
    free(owner);
  }
  *bundle = (flowie_security_policy_bundle_t)FLOWIE_SECURITY_POLICY_BUNDLE_INIT;
}

int flowie_control_store_policy_bundle_load(flowie_control_store_t *store, const char *domain_id,
                                            uint64_t required_version,
                                            flowie_security_policy_bundle_t *bundle_out) {
  flowie_control_policy_bundle_owner_t *owner = NULL;
  flowie_control_database_t *database = NULL;
  flowie_control_statement_t *statement = NULL;
  int64_t rule_count_value;
  size_t expected_ordinal = 0u;
  size_t rule_count = 0u;
  uint64_t policy_version = 0u;
  uint64_t expires_at = 0u;
  int transaction_started = 0;
  int status;
  int rc;
  if (bundle_out && bundle_out->size >= sizeof(*bundle_out))
    *bundle_out = (flowie_security_policy_bundle_t)FLOWIE_SECURITY_POLICY_BUNDLE_INIT;
  if (!store || !flowie_control_text_valid(domain_id, FLOWIE_SECURITY_ID_MAX) ||
      required_version > (uint64_t)INT64_MAX || !bundle_out ||
      bundle_out->size < sizeof(*bundle_out))
    return SALTS_EINVAL;
  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) return rc;
  status = flowie_control_database_exec(database, "BEGIN", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 1;
  status = flowie_control_database_prepare(
      database,
      "SELECT b.policy_version,b.expires_at,"
      "(SELECT COUNT(*) FROM flowie_control_published_rule r WHERE "
      "r.namespace_name=b.namespace_name) "
      "FROM flowie_control_published_bundle b WHERE b.namespace_name=?1 "
      "AND (?2=0 OR b.policy_version=?2)",
      -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  rc = flowie_control_bind_text(statement, 1, domain_id);
  if (rc == SALTS_OK && flowie_control_database_bind_int64(
                            statement, 2, (int64_t)required_version) != FLOWIE_CONTROL_DB_OK)
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
  if (rc != SALTS_OK) goto done;
  status = flowie_control_database_step(statement);
  if (status == FLOWIE_CONTROL_DB_DONE) {
    rc = SALTS_ENOENT;
    goto done;
  }
  if (status != FLOWIE_CONTROL_DB_ROW ||
      flowie_control_database_column_type(statement, 0) != FLOWIE_CONTROL_DB_INTEGER ||
      flowie_control_database_column_type(statement, 1) != FLOWIE_CONTROL_DB_INTEGER ||
      flowie_control_database_column_type(statement, 2) != FLOWIE_CONTROL_DB_INTEGER ||
      flowie_control_database_column_int64(statement, 0) <= 0 ||
      flowie_control_database_column_int64(statement, 1) < 0 ||
      (rule_count_value = flowie_control_database_column_int64(statement, 2)) <= 0 ||
      rule_count_value > (int64_t)FLOWIE_SECURITY_MAX_RULES) {
    rc = status == FLOWIE_CONTROL_DB_ROW ? SALTS_EPROTO : flowie_control_database_status(status);
    goto done;
  }
  policy_version = (uint64_t)flowie_control_database_column_int64(statement, 0);
  expires_at = (uint64_t)flowie_control_database_column_int64(statement, 1);
  rule_count = (size_t)rule_count_value;
  (void)flowie_control_database_finalize(statement);
  statement = NULL;

  owner = (flowie_control_policy_bundle_owner_t *)calloc(1u, sizeof(*owner));
  if (!owner) {
    rc = SALTS_ENOMEM;
    goto done;
  }
  owner->rules = (flowie_security_rule_t *)calloc(rule_count, sizeof(*owner->rules));
  if (!owner->rules) {
    rc = SALTS_ENOMEM;
    goto done;
  }
  status =
      flowie_control_database_prepare(database,
                                      "SELECT ordinal,rule_line FROM flowie_control_published_rule "
                                      "WHERE namespace_name=?1 ORDER BY ordinal",
                                      -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  rc = flowie_control_bind_text(statement, 1, domain_id);
  if (rc != SALTS_OK) goto done;
  while ((status = flowie_control_database_step(statement)) == FLOWIE_CONTROL_DB_ROW) {
    const unsigned char *line;
    int line_size;
    int64_t ordinal;
    flowie_security_rule_t rule = FLOWIE_SECURITY_RULE_INIT;
    if (expected_ordinal >= rule_count ||
        flowie_control_database_column_type(statement, 0) != FLOWIE_CONTROL_DB_INTEGER ||
        flowie_control_database_column_type(statement, 1) != FLOWIE_CONTROL_DB_TEXT) {
      rc = SALTS_EPROTO;
      goto done;
    }
    ordinal = flowie_control_database_column_int64(statement, 0);
    line = flowie_control_database_column_text(statement, 1);
    line_size = flowie_control_database_column_bytes(statement, 1);
    if (ordinal < 0 || (uint64_t)ordinal != (uint64_t)expected_ordinal || !line || line_size <= 0 ||
        (size_t)line_size > FLOWIE_SECURITY_RULE_LINE_MAX ||
        memchr(line, '\0', (size_t)line_size) ||
        flowie_security_rule_parse_line((const char *)line, (size_t)line_size, &rule) != SALTS_OK ||
        strcmp(rule.domain_id, domain_id) != 0) {
      rc = SALTS_EPROTO;
      goto done;
    }
    owner->rules[expected_ordinal++] = rule;
  }
  if (status != FLOWIE_CONTROL_DB_DONE || expected_ordinal != rule_count) {
    rc = status == FLOWIE_CONTROL_DB_DONE ? SALTS_EPROTO : flowie_control_database_status(status);
    goto done;
  }
  (void)flowie_control_database_finalize(statement);
  statement = NULL;
  status = flowie_control_database_exec(database, "COMMIT", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 0;
  bundle_out->policy_version = policy_version;
  bundle_out->expires_at = expires_at;
  bundle_out->rules = owner->rules;
  bundle_out->rule_count = rule_count;
  bundle_out->provider_bundle = owner;
  owner = NULL;
  rc = SALTS_OK;

done:
  if (statement) (void)flowie_control_database_finalize(statement);
  if (transaction_started)
    (void)flowie_control_database_exec(database, "ROLLBACK", NULL, NULL, NULL);
  if (database) (void)flowie_control_database_close(database);
  if (owner) {
    free(owner->rules);
    free(owner);
  }
  if (rc != SALTS_OK)
    *bundle_out = (flowie_security_policy_bundle_t)FLOWIE_SECURITY_POLICY_BUNDLE_INIT;
  return rc;
}

int flowie_control_store_policy_publish(flowie_control_store_t *store,
                                        const flowie_control_policy_publish_command_t *command,
                                        flowie_control_policy_publish_result_t *result) {
  flowie_control_command_result_t replay = FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  flowie_control_policy_validation_t validation = FLOWIE_CONTROL_POLICY_VALIDATION_INIT;
  flowie_control_database_t *database = NULL;
  flowie_control_statement_t *statement = NULL;
  flowie_control_statement_t *draft = NULL;
  flowie_control_statement_t *insert_rule = NULL;
  flowie_security_rule_t *compiled_rules = NULL;
  uint64_t current = 0u;
  uint64_t next = 0u;
  uint64_t current_policy = 0u;
  uint64_t next_policy = 0u;
  size_t ordinal = 0u;
  char publish_detail[64];
  int transaction_started = 0;
  int found = 0;
  int status;
  int rc;
  if (result && result->size >= sizeof(*result))
    *result = (flowie_control_policy_publish_result_t)FLOWIE_CONTROL_POLICY_PUBLISH_RESULT_INIT;
  if (!store || !command || command->size < sizeof(*command) || !result ||
      result->size < sizeof(*result) ||
      !flowie_control_command_common_valid(command->domain_id, command->domain_id, command->actor,
                                           command->request_id, command->expected_revision,
                                           command->occurred_at) ||
      command->expires_at > (uint64_t)INT64_MAX ||
      (command->expires_at != 0u && command->expires_at <= command->occurred_at) ||
      flowie_control_policy_publish_detail(command->expires_at, publish_detail) != SALTS_OK)
    return SALTS_EINVAL;
  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) return rc;
  status = flowie_control_database_exec(database, "BEGIN", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 1;
  rc = flowie_control_replay(database, command->request_id, command->actor,
                             FLOWIE_CONTROL_OPERATION_POLICY_PUBLISH, command->domain_id,
                             command->domain_id, publish_detail, &replay, &found);
  if (rc != SALTS_OK) goto done;
  if (found) {
    status = flowie_control_database_prepare(
        database,
        "SELECT policy_version FROM flowie_control_policy_publish_result WHERE request_id=?1", -1,
        &statement, NULL);
    if (status != FLOWIE_CONTROL_DB_OK) {
      rc = flowie_control_database_status(status);
      goto done;
    }
    rc = flowie_control_bind_text(statement, 1, command->request_id);
    if (rc != SALTS_OK) goto done;
    status = flowie_control_database_step(statement);
    if (status != FLOWIE_CONTROL_DB_ROW ||
        flowie_control_database_column_type(statement, 0) != FLOWIE_CONTROL_DB_INTEGER ||
        flowie_control_database_column_int64(statement, 0) <= 0) {
      rc = status == FLOWIE_CONTROL_DB_ROW ? SALTS_EPROTO : flowie_control_database_status(status);
      goto done;
    }
    result->revision = replay.revision;
    result->policy_version = (uint64_t)flowie_control_database_column_int64(statement, 0);
    result->replayed = 1;
    (void)flowie_control_database_finalize(statement);
    statement = NULL;
    goto commit;
  }
  rc = flowie_control_read_revision(database, &current);
  if (rc != SALTS_OK) goto done;
  if (command->expected_revision != 0u && current != command->expected_revision) {
    rc = SALTS_EBUSY;
    goto done;
  }
  rc = flowie_control_policy_validate_database(database, command->domain_id, &validation);
  if (rc != SALTS_OK) goto done;
  compiled_rules = (flowie_security_rule_t *)calloc(validation.rule_count, sizeof(*compiled_rules));
  if (!compiled_rules) {
    rc = SALTS_ENOMEM;
    goto done;
  }
  status = flowie_control_database_prepare(
      database,
      "SELECT policy_version FROM flowie_control_published_bundle WHERE namespace_name=?1", -1,
      &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  rc = flowie_control_bind_text(statement, 1, command->domain_id);
  if (rc != SALTS_OK) goto done;
  status = flowie_control_database_step(statement);
  if (status == FLOWIE_CONTROL_DB_ROW) {
    if (flowie_control_database_column_type(statement, 0) != FLOWIE_CONTROL_DB_INTEGER ||
        flowie_control_database_column_int64(statement, 0) <= 0) {
      rc = SALTS_EPROTO;
      goto done;
    }
    current_policy = (uint64_t)flowie_control_database_column_int64(statement, 0);
  } else if (status != FLOWIE_CONTROL_DB_DONE) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  (void)flowie_control_database_finalize(statement);
  statement = NULL;
  if (current_policy >= (uint64_t)INT64_MAX || current >= (uint64_t)INT64_MAX) {
    rc = SALTS_ERANGE;
    goto done;
  }
  next_policy = current_policy + 1u;
  status = flowie_control_database_prepare(
      database, "DELETE FROM flowie_control_published_rule WHERE namespace_name=?1", -1, &statement,
      NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  rc = flowie_control_bind_text(statement, 1, command->domain_id);
  if (rc == SALTS_OK) {
    status = flowie_control_database_step(statement);
    rc = status == FLOWIE_CONTROL_DB_DONE ? SALTS_OK : flowie_control_database_status(status);
  }
  (void)flowie_control_database_finalize(statement);
  statement = NULL;
  if (rc != SALTS_OK) goto done;
  status = flowie_control_database_prepare(
      database,
      "INSERT INTO flowie_control_published_bundle(namespace_name,policy_version,expires_at) "
      "VALUES(?1,?2,?3) ON CONFLICT(namespace_name) DO UPDATE SET "
      "policy_version=excluded.policy_version,expires_at=excluded.expires_at",
      -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  rc = flowie_control_bind_text(statement, 1, command->domain_id);
  if (rc == SALTS_OK && flowie_control_database_bind_int64(statement, 2, (int64_t)next_policy) !=
                            FLOWIE_CONTROL_DB_OK)
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
  if (rc == SALTS_OK && flowie_control_database_bind_int64(
                            statement, 3, (int64_t)command->expires_at) != FLOWIE_CONTROL_DB_OK)
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
  if (rc == SALTS_OK) {
    status = flowie_control_database_step(statement);
    rc = status == FLOWIE_CONTROL_DB_DONE ? SALTS_OK : flowie_control_database_status(status);
  }
  (void)flowie_control_database_finalize(statement);
  statement = NULL;
  if (rc != SALTS_OK) goto done;
  status = flowie_control_database_prepare(
      database,
      "SELECT rule_document FROM flowie_control_policy_draft WHERE domain_id=?1 ORDER BY ordinal",
      -1, &draft, NULL);
  if (status == FLOWIE_CONTROL_DB_OK)
    status = flowie_control_database_prepare(
        database,
        "INSERT INTO flowie_control_published_rule(namespace_name,ordinal,rule_line) "
        "VALUES(?1,?2,?3)",
        -1, &insert_rule, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  rc = flowie_control_bind_text(draft, 1, command->domain_id);
  if (rc != SALTS_OK) goto done;
  while ((status = flowie_control_database_step(draft)) == FLOWIE_CONTROL_DB_ROW) {
    const unsigned char *line;
    int line_size;
    flowie_control_acl_document_t document = FLOWIE_CONTROL_ACL_DOCUMENT_INIT;
    size_t compiled_count = 0u;
    if (ordinal >= validation.rule_count ||
        flowie_control_database_column_type(draft, 0) != FLOWIE_CONTROL_DB_TEXT) {
      rc = SALTS_EPROTO;
      goto done;
    }
    line = flowie_control_database_column_text(draft, 0);
    line_size = flowie_control_database_column_bytes(draft, 0);
    if (!line || line_size <= 0 ||
        flowie_control_acl_document_syntax_validate(command->domain_id, (const char *)line,
                                                    (size_t)line_size, &document) != SALTS_OK) {
      rc = SALTS_EPROTO;
      goto done;
    }
    rc = flowie_control_acl_compile(&document, command->domain_id, compiled_rules + ordinal,
                                    validation.rule_count - ordinal, &compiled_count);
    if (rc != SALTS_OK || compiled_count == 0u) {
      rc = rc != SALTS_OK ? rc : SALTS_EPROTO;
      goto done;
    }
    for (size_t index = 0u; index < compiled_count; ++index) {
      char canonical[FLOWIE_SECURITY_RULE_LINE_MAX + 1u];
      size_t canonical_size = 0u;
      rc = flowie_security_rule_format_line(&compiled_rules[ordinal + index], canonical,
                                            sizeof(canonical), &canonical_size);
      if (rc != SALTS_OK) goto done;
      (void)flowie_control_database_reset(insert_rule);
      (void)flowie_control_database_clear_bindings(insert_rule);
      rc = flowie_control_bind_text(insert_rule, 1, command->domain_id);
      if (rc == SALTS_OK && flowie_control_database_bind_int64(
                                insert_rule, 2, (int64_t)(ordinal + index)) != FLOWIE_CONTROL_DB_OK)
        rc = flowie_control_database_status(flowie_control_database_errcode(database));
      if (rc == SALTS_OK &&
          flowie_control_database_bind_text(insert_rule, 3, canonical, (int)canonical_size,
                                            FLOWIE_CONTROL_DB_TRANSIENT) != FLOWIE_CONTROL_DB_OK)
        rc = flowie_control_database_status(flowie_control_database_errcode(database));
      if (rc == SALTS_OK) {
        int insert_status = flowie_control_database_step(insert_rule);
        rc = insert_status == FLOWIE_CONTROL_DB_DONE
                 ? SALTS_OK
                 : flowie_control_database_status(insert_status);
      }
      if (rc != SALTS_OK) goto done;
    }
    ordinal += compiled_count;
  }
  if (status != FLOWIE_CONTROL_DB_DONE || ordinal != validation.rule_count) {
    rc = status == FLOWIE_CONTROL_DB_DONE ? SALTS_EPROTO : flowie_control_database_status(status);
    goto done;
  }
  (void)flowie_control_database_finalize(draft);
  draft = NULL;
  (void)flowie_control_database_finalize(insert_rule);
  insert_rule = NULL;
  rc = flowie_control_advance_revision(database, current, &next);
  if (rc != SALTS_OK) goto done;
  rc = flowie_control_insert_audit(database, command->request_id, command->actor,
                                   FLOWIE_CONTROL_OPERATION_POLICY_PUBLISH, command->domain_id,
                                   command->domain_id, publish_detail, next, command->occurred_at);
  if (rc != SALTS_OK) goto done;
  status = flowie_control_database_prepare(
      database,
      "INSERT INTO flowie_control_policy_publish_result(request_id,policy_version) VALUES(?1,?2)",
      -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  rc = flowie_control_bind_text(statement, 1, command->request_id);
  if (rc == SALTS_OK && flowie_control_database_bind_int64(statement, 2, (int64_t)next_policy) !=
                            FLOWIE_CONTROL_DB_OK)
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
  if (rc == SALTS_OK) {
    status = flowie_control_database_step(statement);
    rc = status == FLOWIE_CONTROL_DB_DONE ? SALTS_OK : flowie_control_database_status(status);
  }
  (void)flowie_control_database_finalize(statement);
  statement = NULL;
  if (rc != SALTS_OK) goto done;
  result->revision = next;
  result->policy_version = next_policy;
  result->replayed = 0;

commit:
  status = flowie_control_database_exec(database, "COMMIT", NULL, NULL, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  transaction_started = 0;
  rc = SALTS_OK;

done:
  if (statement) (void)flowie_control_database_finalize(statement);
  if (draft) (void)flowie_control_database_finalize(draft);
  if (insert_rule) (void)flowie_control_database_finalize(insert_rule);
  free(compiled_rules);
  if (transaction_started)
    (void)flowie_control_database_exec(database, "ROLLBACK", NULL, NULL, NULL);
  (void)flowie_control_database_close(database);
  if (rc != SALTS_OK)
    *result = (flowie_control_policy_publish_result_t)FLOWIE_CONTROL_POLICY_PUBLISH_RESULT_INIT;
  return rc;
}

typedef int (*flowie_control_page_row_fn)(flowie_control_statement_t *statement, void *item);

int flowie_control_store_domain_get(flowie_control_store_t *store, const char *domain_id,
                                    flowie_control_domain_view_t *out) {
  flowie_control_database_t *database = NULL;
  flowie_control_statement_t *statement = NULL;
  flowie_control_domain_view_t view = FLOWIE_CONTROL_DOMAIN_VIEW_INIT;
  int status;
  int rc;
  if (out && out->size >= sizeof(*out)) *out = view;
  if (!store || !flowie_control_text_valid(domain_id, FLOWIE_SECURITY_ID_MAX) || !out ||
      out->size < sizeof(*out))
    return SALTS_EINVAL;
  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) return rc;
  status = flowie_control_database_prepare(
      database, "SELECT domain_id FROM flowie_control_domain WHERE domain_id=?1", -1, &statement,
      NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  rc = flowie_control_bind_text(statement, 1, domain_id);
  if (rc != SALTS_OK) goto done;
  status = flowie_control_database_step(statement);
  if (status == FLOWIE_CONTROL_DB_DONE) {
    rc = SALTS_ENOENT;
    goto done;
  }
  if (status != FLOWIE_CONTROL_DB_ROW) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  rc = flowie_control_copy_column(statement, 0, view.domain_id, sizeof(view.domain_id));
  if (rc == SALTS_OK && flowie_control_database_step(statement) != FLOWIE_CONTROL_DB_DONE)
    rc = SALTS_EPROTO;
  if (rc == SALTS_OK) *out = view;

done:
  if (statement) (void)flowie_control_database_finalize(statement);
  (void)flowie_control_database_close(database);
  return rc;
}

int flowie_control_store_domain_list(flowie_control_store_t *store, const char *after_domain_id,
                                     flowie_control_domain_view_t *items, size_t item_capacity,
                                     size_t *count_out, int *has_more_out) {
  flowie_control_database_t *database = NULL;
  flowie_control_statement_t *statement = NULL;
  size_t count = 0u;
  int status;
  int rc;
  if (count_out) *count_out = 0u;
  if (has_more_out) *has_more_out = 0;
  if (!store ||
      (after_domain_id && !flowie_control_text_valid(after_domain_id, FLOWIE_SECURITY_ID_MAX)) ||
      !items || item_capacity == 0u || item_capacity > FLOWIE_CONTROL_PAGE_MAX || !count_out ||
      !has_more_out)
    return SALTS_EINVAL;
  for (size_t index = 0u; index < item_capacity; ++index) {
    if (items[index].size < sizeof(items[index])) return SALTS_EINVAL;
    items[index] = (flowie_control_domain_view_t)FLOWIE_CONTROL_DOMAIN_VIEW_INIT;
  }
  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) return rc;
  status =
      flowie_control_database_prepare(database,
                                      "SELECT domain_id FROM flowie_control_domain "
                                      "WHERE (?1='' OR domain_id>?1) ORDER BY domain_id LIMIT ?2",
                                      -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  rc = flowie_control_bind_text(statement, 1, after_domain_id ? after_domain_id : "");
  if (rc == SALTS_OK && flowie_control_database_bind_int64(
                            statement, 2, (int64_t)(item_capacity + 1u)) != FLOWIE_CONTROL_DB_OK)
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
  if (rc != SALTS_OK) goto done;
  while ((status = flowie_control_database_step(statement)) == FLOWIE_CONTROL_DB_ROW) {
    if (count == item_capacity) {
      *has_more_out = 1;
      continue;
    }
    rc = flowie_control_copy_column(statement, 0, items[count].domain_id,
                                    sizeof(items[count].domain_id));
    if (rc != SALTS_OK) goto done;
    ++count;
  }
  if (status != FLOWIE_CONTROL_DB_DONE) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  *count_out = count;
  rc = SALTS_OK;

done:
  if (statement) (void)flowie_control_database_finalize(statement);
  (void)flowie_control_database_close(database);
  if (rc != SALTS_OK) {
    *count_out = 0u;
    *has_more_out = 0;
  }
  return rc;
}

static int flowie_control_page_arguments_valid(flowie_control_store_t *store, const char *domain_id,
                                               const char *after_id, const void *items,
                                               size_t item_size, size_t item_capacity,
                                               size_t *count_out, int *has_more_out) {
  const uint8_t *cursor = (const uint8_t *)items;
  if (!store || !flowie_control_text_valid(domain_id, FLOWIE_SECURITY_ID_MAX) ||
      (after_id && !flowie_control_text_valid(after_id, FLOWIE_SECURITY_ID_MAX)) || !items ||
      item_size < sizeof(size_t) || item_capacity == 0u ||
      item_capacity > FLOWIE_CONTROL_PAGE_MAX || !count_out || !has_more_out)
    return 0;
  for (size_t index = 0u; index < item_capacity; ++index) {
    if (*(const size_t *)(cursor + index * item_size) < item_size) return 0;
  }
  return 1;
}

static int flowie_control_text_page(flowie_control_store_t *store, const char *domain_id,
                                    const char *after_id, const char *sql, void *items,
                                    size_t item_size, size_t item_capacity,
                                    flowie_control_page_row_fn decode, size_t *count_out,
                                    int *has_more_out) {
  flowie_control_database_t *database = NULL;
  flowie_control_statement_t *statement = NULL;
  uint8_t *cursor = (uint8_t *)items;
  size_t count = 0u;
  int status;
  int rc;
  if (count_out) *count_out = 0u;
  if (has_more_out) *has_more_out = 0;
  if (!sql || !decode ||
      !flowie_control_page_arguments_valid(store, domain_id, after_id, items, item_size,
                                           item_capacity, count_out, has_more_out))
    return SALTS_EINVAL;
  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) return rc;
  status = flowie_control_database_prepare(database, sql, -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  rc = flowie_control_bind_text(statement, 1, domain_id);
  if (rc == SALTS_OK) rc = flowie_control_bind_text(statement, 2, after_id ? after_id : "");
  if (rc == SALTS_OK && flowie_control_database_bind_int64(
                            statement, 3, (int64_t)(item_capacity + 1u)) != FLOWIE_CONTROL_DB_OK)
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
  if (rc != SALTS_OK) goto done;
  while ((status = flowie_control_database_step(statement)) == FLOWIE_CONTROL_DB_ROW) {
    if (count == item_capacity) {
      *has_more_out = 1;
      continue;
    }
    rc = decode(statement, cursor + count * item_size);
    if (rc != SALTS_OK) goto done;
    ++count;
  }
  if (status != FLOWIE_CONTROL_DB_DONE) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  *count_out = count;
  rc = SALTS_OK;

done:
  if (statement) (void)flowie_control_database_finalize(statement);
  (void)flowie_control_database_close(database);
  if (rc != SALTS_OK) {
    *count_out = 0u;
    *has_more_out = 0;
  }
  return rc;
}

static int flowie_control_user_page_row(flowie_control_statement_t *statement, void *item) {
  flowie_control_user_view_t *view = (flowie_control_user_view_t *)item;
  int64_t revision;
  int64_t created_at;
  int64_t updated_at;
  int enabled;
  int rc;
  if (flowie_control_database_column_type(statement, 3) != FLOWIE_CONTROL_DB_INTEGER ||
      flowie_control_database_column_type(statement, 4) != FLOWIE_CONTROL_DB_INTEGER ||
      flowie_control_database_column_type(statement, 5) != FLOWIE_CONTROL_DB_INTEGER ||
      flowie_control_database_column_type(statement, 6) != FLOWIE_CONTROL_DB_INTEGER ||
      (enabled = flowie_control_database_column_int(statement, 3)) < 0 || enabled > 1 ||
      (revision = flowie_control_database_column_int64(statement, 4)) <= 0 ||
      (created_at = flowie_control_database_column_int64(statement, 5)) <= 0 ||
      (updated_at = flowie_control_database_column_int64(statement, 6)) <= 0)
    return SALTS_EPROTO;
  *view = (flowie_control_user_view_t)FLOWIE_CONTROL_USER_VIEW_INIT;
  rc = flowie_control_copy_column(statement, 0, view->domain_id, sizeof(view->domain_id));
  if (rc == SALTS_OK)
    rc = flowie_control_copy_column(statement, 1, view->principal_id, sizeof(view->principal_id));
  if (rc == SALTS_OK)
    rc = flowie_control_copy_column(statement, 2, view->principal_type,
                                    sizeof(view->principal_type));
  if (rc != SALTS_OK) return rc;
  view->enabled = enabled;
  view->revision = (uint64_t)revision;
  view->created_at = (uint64_t)created_at;
  view->updated_at = (uint64_t)updated_at;
  return SALTS_OK;
}

static int flowie_control_group_page_row(flowie_control_statement_t *statement, void *item) {
  flowie_control_group_view_t *view = (flowie_control_group_view_t *)item;
  int64_t depth;
  int64_t revision;
  int64_t created_at;
  int64_t updated_at;
  int enabled;
  int rc;
  if (flowie_control_database_column_type(statement, 3) != FLOWIE_CONTROL_DB_INTEGER ||
      flowie_control_database_column_type(statement, 4) != FLOWIE_CONTROL_DB_INTEGER ||
      flowie_control_database_column_type(statement, 5) != FLOWIE_CONTROL_DB_INTEGER ||
      flowie_control_database_column_type(statement, 6) != FLOWIE_CONTROL_DB_INTEGER ||
      flowie_control_database_column_type(statement, 7) != FLOWIE_CONTROL_DB_INTEGER ||
      (depth = flowie_control_database_column_int64(statement, 3)) < 0 ||
      depth > FLOWIE_CONTROL_GROUP_MAX_DEPTH ||
      (enabled = flowie_control_database_column_int(statement, 4)) < 0 || enabled > 1 ||
      (revision = flowie_control_database_column_int64(statement, 5)) <= 0 ||
      (created_at = flowie_control_database_column_int64(statement, 6)) <= 0 ||
      (updated_at = flowie_control_database_column_int64(statement, 7)) <= 0)
    return SALTS_EPROTO;
  *view = (flowie_control_group_view_t)FLOWIE_CONTROL_GROUP_VIEW_INIT;
  rc = flowie_control_copy_column(statement, 0, view->domain_id, sizeof(view->domain_id));
  if (rc == SALTS_OK)
    rc = flowie_control_copy_column(statement, 1, view->group_id, sizeof(view->group_id));
  if (rc == SALTS_OK && flowie_control_database_column_type(statement, 2) != FLOWIE_CONTROL_DB_NULL)
    rc = flowie_control_copy_column(statement, 2, view->parent_group_id,
                                    sizeof(view->parent_group_id));
  if (rc != SALTS_OK) return rc;
  view->depth = (uint32_t)depth;
  view->enabled = enabled;
  view->revision = (uint64_t)revision;
  view->created_at = (uint64_t)created_at;
  view->updated_at = (uint64_t)updated_at;
  return SALTS_OK;
}

static int flowie_control_role_page_row(flowie_control_statement_t *statement, void *item) {
  flowie_control_role_view_t *view = (flowie_control_role_view_t *)item;
  int64_t revision;
  int64_t created_at;
  int64_t updated_at;
  int enabled;
  int rc;
  if (flowie_control_database_column_type(statement, 2) != FLOWIE_CONTROL_DB_INTEGER ||
      flowie_control_database_column_type(statement, 3) != FLOWIE_CONTROL_DB_INTEGER ||
      flowie_control_database_column_type(statement, 4) != FLOWIE_CONTROL_DB_INTEGER ||
      flowie_control_database_column_type(statement, 5) != FLOWIE_CONTROL_DB_INTEGER ||
      (enabled = flowie_control_database_column_int(statement, 2)) < 0 || enabled > 1 ||
      (revision = flowie_control_database_column_int64(statement, 3)) <= 0 ||
      (created_at = flowie_control_database_column_int64(statement, 4)) <= 0 ||
      (updated_at = flowie_control_database_column_int64(statement, 5)) <= 0)
    return SALTS_EPROTO;
  *view = (flowie_control_role_view_t)FLOWIE_CONTROL_ROLE_VIEW_INIT;
  rc = flowie_control_copy_column(statement, 0, view->domain_id, sizeof(view->domain_id));
  if (rc == SALTS_OK)
    rc = flowie_control_copy_column(statement, 1, view->role_id, sizeof(view->role_id));
  if (rc != SALTS_OK) return rc;
  view->enabled = enabled;
  view->revision = (uint64_t)revision;
  view->created_at = (uint64_t)created_at;
  view->updated_at = (uint64_t)updated_at;
  return SALTS_OK;
}

static int flowie_control_membership_page_row(flowie_control_statement_t *statement, void *item) {
  flowie_control_membership_view_t *view = (flowie_control_membership_view_t *)item;
  int rc;
  *view = (flowie_control_membership_view_t)FLOWIE_CONTROL_MEMBERSHIP_VIEW_INIT;
  rc = flowie_control_copy_column(statement, 0, view->domain_id, sizeof(view->domain_id));
  if (rc == SALTS_OK)
    rc = flowie_control_copy_column(statement, 1, view->principal_id,
                                    sizeof(view->principal_id));
  if (rc == SALTS_OK)
    rc = flowie_control_copy_column(statement, 2, view->group_id, sizeof(view->group_id));
  return rc;
}

static int flowie_control_user_role_page_row(flowie_control_statement_t *statement, void *item) {
  flowie_control_user_role_view_t *view = (flowie_control_user_role_view_t *)item;
  int rc;
  *view = (flowie_control_user_role_view_t)FLOWIE_CONTROL_USER_ROLE_VIEW_INIT;
  rc = flowie_control_copy_column(statement, 0, view->domain_id, sizeof(view->domain_id));
  if (rc == SALTS_OK)
    rc = flowie_control_copy_column(statement, 1, view->principal_id,
                                    sizeof(view->principal_id));
  if (rc == SALTS_OK)
    rc = flowie_control_copy_column(statement, 2, view->role_id, sizeof(view->role_id));
  return rc;
}

static int flowie_control_tuple_text_page(
    flowie_control_store_t *store, const char *domain_id, const char *after_first_id,
    const char *after_second_id, const char *sql, void *items, size_t item_size,
    size_t item_capacity, flowie_control_page_row_fn decode, size_t *count_out,
    int *has_more_out) {
  flowie_control_database_t *database = NULL;
  flowie_control_statement_t *statement = NULL;
  uint8_t *cursor = (uint8_t *)items;
  size_t count = 0u;
  int status;
  int rc;
  if (count_out) *count_out = 0u;
  if (has_more_out) *has_more_out = 0;
  if (!sql || !decode || (!!after_first_id != !!after_second_id) ||
      !flowie_control_page_arguments_valid(store, domain_id, after_first_id, items, item_size,
                                           item_capacity, count_out, has_more_out) ||
      (after_second_id &&
       !flowie_control_text_valid(after_second_id, FLOWIE_SECURITY_ID_MAX)))
    return SALTS_EINVAL;
  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) return rc;
  status = flowie_control_database_prepare(database, sql, -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  rc = flowie_control_bind_text(statement, 1, domain_id);
  if (rc == SALTS_OK)
    rc = flowie_control_bind_text(statement, 2, after_first_id ? after_first_id : "");
  if (rc == SALTS_OK)
    rc = flowie_control_bind_text(statement, 3, after_second_id ? after_second_id : "");
  if (rc == SALTS_OK && flowie_control_database_bind_int64(
                            statement, 4, (int64_t)(item_capacity + 1u)) != FLOWIE_CONTROL_DB_OK)
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
  if (rc != SALTS_OK) goto done;
  while ((status = flowie_control_database_step(statement)) == FLOWIE_CONTROL_DB_ROW) {
    if (count == item_capacity) {
      *has_more_out = 1;
      continue;
    }
    rc = decode(statement, cursor + count * item_size);
    if (rc != SALTS_OK) goto done;
    ++count;
  }
  if (status != FLOWIE_CONTROL_DB_DONE) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  *count_out = count;
  rc = SALTS_OK;

done:
  if (statement) (void)flowie_control_database_finalize(statement);
  (void)flowie_control_database_close(database);
  if (rc != SALTS_OK) {
    *count_out = 0u;
    *has_more_out = 0;
  }
  return rc;
}

int flowie_control_store_user_list(flowie_control_store_t *store, const char *domain_id,
                                   const char *after_principal_id,
                                   flowie_control_user_view_t *items, size_t item_capacity,
                                   size_t *count_out, int *has_more_out) {
  static const char sql[] =
      "SELECT domain_id,principal_id,principal_type,enabled,revision,created_at,updated_at "
      "FROM flowie_control_user WHERE domain_id=?1 AND (?2='' OR principal_id>?2) "
      "ORDER BY principal_id LIMIT ?3";
  return flowie_control_text_page(store, domain_id, after_principal_id, sql, items, sizeof(*items),
                                  item_capacity, flowie_control_user_page_row, count_out,
                                  has_more_out);
}

int flowie_control_store_group_list(flowie_control_store_t *store, const char *domain_id,
                                    const char *after_group_id, flowie_control_group_view_t *items,
                                    size_t item_capacity, size_t *count_out, int *has_more_out) {
  static const char sql[] =
      "SELECT domain_id,group_id,parent_group_id,depth,enabled,revision,created_at,updated_at "
      "FROM flowie_control_group WHERE domain_id=?1 AND (?2='' OR group_id>?2) "
      "ORDER BY group_id LIMIT ?3";
  return flowie_control_text_page(store, domain_id, after_group_id, sql, items, sizeof(*items),
                                  item_capacity, flowie_control_group_page_row, count_out,
                                  has_more_out);
}

int flowie_control_store_membership_list(
    flowie_control_store_t *store, const char *domain_id, const char *after_principal_id,
    const char *after_group_id, flowie_control_membership_view_t *items, size_t item_capacity,
    size_t *count_out, int *has_more_out) {
  static const char sql[] =
      "SELECT domain_id,principal_id,group_id FROM flowie_control_membership "
      "WHERE domain_id=?1 AND (?2='' OR principal_id>?2 OR "
      "(principal_id=?2 AND group_id>?3)) ORDER BY principal_id,group_id LIMIT ?4";
  return flowie_control_tuple_text_page(
      store, domain_id, after_principal_id, after_group_id, sql, items, sizeof(*items),
      item_capacity, flowie_control_membership_page_row, count_out, has_more_out);
}

int flowie_control_store_role_list(flowie_control_store_t *store, const char *domain_id,
                                   const char *after_role_id, flowie_control_role_view_t *items,
                                   size_t item_capacity, size_t *count_out, int *has_more_out) {
  static const char sql[] =
      "SELECT domain_id,role_id,enabled,revision,created_at,updated_at FROM "
      "flowie_control_role "
      "WHERE domain_id=?1 AND (?2='' OR role_id>?2) ORDER BY role_id LIMIT ?3";
  return flowie_control_text_page(store, domain_id, after_role_id, sql, items, sizeof(*items),
                                  item_capacity, flowie_control_role_page_row, count_out,
                                  has_more_out);
}

int flowie_control_store_user_role_list(
    flowie_control_store_t *store, const char *domain_id, const char *after_principal_id,
    const char *after_role_id, flowie_control_user_role_view_t *items, size_t item_capacity,
    size_t *count_out, int *has_more_out) {
  static const char sql[] =
      "SELECT domain_id,principal_id,role_id FROM flowie_control_user_role "
      "WHERE domain_id=?1 AND (?2='' OR principal_id>?2 OR "
      "(principal_id=?2 AND role_id>?3)) ORDER BY principal_id,role_id LIMIT ?4";
  return flowie_control_tuple_text_page(
      store, domain_id, after_principal_id, after_role_id, sql, items, sizeof(*items), item_capacity,
      flowie_control_user_role_page_row, count_out, has_more_out);
}

int flowie_control_store_audit_list(flowie_control_store_t *store, const char *domain_id,
                                    uint64_t after_revision, flowie_control_audit_view_t *items,
                                    size_t item_capacity, size_t *count_out, int *has_more_out) {
  flowie_control_database_t *database = NULL;
  flowie_control_statement_t *statement = NULL;
  size_t count = 0u;
  int status;
  int rc;
  if (count_out) *count_out = 0u;
  if (has_more_out) *has_more_out = 0;
  if (!store || !flowie_control_text_valid(domain_id, FLOWIE_SECURITY_ID_MAX) || !items ||
      item_capacity == 0u || item_capacity > FLOWIE_CONTROL_PAGE_MAX || !count_out ||
      !has_more_out || after_revision > (uint64_t)INT64_MAX)
    return SALTS_EINVAL;
  for (size_t index = 0u; index < item_capacity; ++index) {
    if (items[index].size < sizeof(items[index])) return SALTS_EINVAL;
    items[index] = (flowie_control_audit_view_t)FLOWIE_CONTROL_AUDIT_VIEW_INIT;
  }
  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) return rc;
  status = flowie_control_database_prepare(
      database,
      "SELECT request_id,actor,operation,domain_id,target_id,target_detail,result_revision,"
      "occurred_at FROM flowie_control_audit WHERE domain_id=?1 AND result_revision>?2 "
      "ORDER BY result_revision LIMIT ?3",
      -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  rc = flowie_control_bind_text(statement, 1, domain_id);
  if (rc == SALTS_OK && flowie_control_database_bind_int64(statement, 2, (int64_t)after_revision) !=
                            FLOWIE_CONTROL_DB_OK)
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
  if (rc == SALTS_OK && flowie_control_database_bind_int64(
                            statement, 3, (int64_t)(item_capacity + 1u)) != FLOWIE_CONTROL_DB_OK)
    rc = flowie_control_database_status(flowie_control_database_errcode(database));
  if (rc != SALTS_OK) goto done;
  while ((status = flowie_control_database_step(statement)) == FLOWIE_CONTROL_DB_ROW) {
    flowie_control_audit_view_t *view;
    int64_t revision;
    int64_t occurred_at;
    if (count == item_capacity) {
      *has_more_out = 1;
      continue;
    }
    if (flowie_control_database_column_type(statement, 6) != FLOWIE_CONTROL_DB_INTEGER ||
        flowie_control_database_column_type(statement, 7) != FLOWIE_CONTROL_DB_INTEGER ||
        (revision = flowie_control_database_column_int64(statement, 6)) <= 0 ||
        (occurred_at = flowie_control_database_column_int64(statement, 7)) <= 0) {
      rc = SALTS_EPROTO;
      goto done;
    }
    view = &items[count];
    rc = flowie_control_copy_column(statement, 0, view->request_id, sizeof(view->request_id));
    if (rc == SALTS_OK)
      rc = flowie_control_copy_column(statement, 1, view->actor, sizeof(view->actor));
    if (rc == SALTS_OK)
      rc = flowie_control_copy_column(statement, 2, view->operation, sizeof(view->operation));
    if (rc == SALTS_OK)
      rc = flowie_control_copy_column(statement, 3, view->domain_id, sizeof(view->domain_id));
    if (rc == SALTS_OK)
      rc = flowie_control_copy_column(statement, 4, view->target_id, sizeof(view->target_id));
    if (rc == SALTS_OK)
      rc = flowie_control_copy_column(statement, 5, view->target_detail,
                                      sizeof(view->target_detail));
    if (rc != SALTS_OK) goto done;
    view->revision = (uint64_t)revision;
    view->occurred_at = (uint64_t)occurred_at;
    ++count;
  }
  if (status != FLOWIE_CONTROL_DB_DONE) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  *count_out = count;
  rc = SALTS_OK;

done:
  if (statement) (void)flowie_control_database_finalize(statement);
  (void)flowie_control_database_close(database);
  if (rc != SALTS_OK) {
    *count_out = 0u;
    *has_more_out = 0;
  }
  return rc;
}

int flowie_control_store_revision(flowie_control_store_t *store, uint64_t *revision_out) {
  flowie_control_database_t *database = NULL;
  int rc;
  if (revision_out) *revision_out = 0u;
  if (!store || !revision_out) return SALTS_EINVAL;
  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) return rc;
  rc = flowie_control_read_revision(database, revision_out);
  (void)flowie_control_database_close(database);
  return rc;
}

int flowie_control_store_audit_count(flowie_control_store_t *store, size_t *count_out) {
  flowie_control_database_t *database = NULL;
  flowie_control_statement_t *statement = NULL;
  int64_t count;
  int status;
  int rc;
  if (count_out) *count_out = 0u;
  if (!store || !count_out) return SALTS_EINVAL;
  rc = flowie_control_open_database(store, &database);
  if (rc != SALTS_OK) return rc;
  status = flowie_control_database_prepare(database, "SELECT COUNT(*) FROM flowie_control_audit",
                                           -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) {
    rc = flowie_control_database_status(status);
    goto done;
  }
  status = flowie_control_database_step(statement);
  if (status != FLOWIE_CONTROL_DB_ROW ||
      flowie_control_database_column_type(statement, 0) != FLOWIE_CONTROL_DB_INTEGER ||
      (count = flowie_control_database_column_int64(statement, 0)) < 0 ||
      (uint64_t)count > SIZE_MAX) {
    rc = status == FLOWIE_CONTROL_DB_ROW ? SALTS_EPROTO : flowie_control_database_status(status);
    goto done;
  }
  *count_out = (size_t)count;
  rc = SALTS_OK;

done:
  if (statement) (void)flowie_control_database_finalize(statement);
  (void)flowie_control_database_close(database);
  return rc;
}
