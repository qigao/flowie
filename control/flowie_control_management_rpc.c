#include "flowie_control_management_rpc_internal.h"

#include "flowie_control_async_internal.h"
#include "flowie_control_credential_internal.h"
#include "salts_error.h"
#include "salts_thread.h"
#include <json_parser.h>

#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

enum {
  FLOWIE_CONTROL_RPC_AUTH_REQUIRED = -32001,
  FLOWIE_CONTROL_RPC_FORBIDDEN = -32003,
  FLOWIE_CONTROL_RPC_NOT_FOUND = -32004,
  FLOWIE_CONTROL_RPC_CONFLICT = -32009,
  FLOWIE_CONTROL_RPC_SECRET_UNAVAILABLE = -32010,
  FLOWIE_CONTROL_RPC_DEFAULT_PAGE = 25
};

enum {
  FLOWIE_CONTROL_RPC_POLICY_VALIDATE = 1,
  FLOWIE_CONTROL_RPC_POLICY_PUBLISH = 2,
  FLOWIE_CONTROL_RPC_POLICY_DRY_RUN = 3
};

static const char *const FLOWIE_CONTROL_RPC_METHODS[] = {"control.system.status",
                                                         "control.auth.external_https.stats",
                                                         "control.domain.create",
                                                         "control.domain.admin.initialize",
                                                         "control.domain.list",
                                                         "control.user.get",
                                                         "control.user.list",
                                                         "control.user.create",
                                                         "control.user.disable",
                                                         "control.group.list",
                                                         "control.group.create",
                                                         "control.group.delete",
                                                         "control.group.member.add",
                                                         "control.group.member.remove",
                                                         "control.group.effective",
                                                         "control.role.list",
                                                         "control.role.create",
                                                         "control.role.disable",
                                                         "control.role.assign",
                                                         "control.role.remove",
                                                         "control.role.effective",
                                                         "control.policy.status",
                                                         "control.policy.subject_rule.get",
                                                         "control.policy.subject_rule.list",
                                                         "control.policy.subject_rule.put",
                                                         "control.policy.subject_rule.delete",
                                                         "control.policy.validate",
                                                         "control.policy.dry_run",
                                                         "control.policy.publish",
                                                         "control.audit.list",
                                                         "control.credential.generate",
                                                         "control.credential.rotate",
                                                         "control.credential.revoke",
                                                         "control.password.set",
                                                         "control.password.change"};

struct flowie_control_management_rpc_server_s {
  flowie_control_management_service_t *service;
  rpc_context_t *rpc_context;
  flowie_control_management_rpc_resolve_caller_fn resolve_caller;
  void *resolve_caller_ctx;
  flowie_control_management_rpc_clock_fn clock;
  void *clock_ctx;
  flowie_control_management_rpc_external_https_stats_fn external_https_stats;
  void *external_https_stats_ctx;
  salts_threadpool_t *policy_executor;
  uint32_t policy_executor_deadline_ms;
  flowie_control_http_app_t *bound_app;
  size_t registered_method_count;
};

typedef struct flowie_control_rpc_policy_job_s {
  flowie_control_management_service_t *service;
  atomic_uint references;
  atomic_int completed;
  int operation;
  int result;
  flowie_control_management_caller_t caller;
  flowie_control_policy_publish_command_t command;
  flowie_control_policy_validation_t validation;
  flowie_control_policy_dry_run_change_t *dry_run_changes;
  size_t dry_run_change_count;
  flowie_control_policy_dry_run_result_t dry_run_result;
  flowie_control_policy_diagnostic_t dry_run_diagnostics[FLOWIE_CONTROL_POLICY_DRY_RUN_MAX_CHANGES];
  flowie_control_policy_publish_result_t publish_result;
  char caller_domain_id[FLOWIE_SECURITY_ID_MAX + 1u];
  char actor[FLOWIE_CONTROL_ACTOR_MAX + 1u];
  char command_domain_id[FLOWIE_SECURITY_ID_MAX + 1u];
  char request_id[FLOWIE_CONTROL_REQUEST_ID_MAX + 1u];
} flowie_control_rpc_policy_job_t;

typedef struct flowie_control_rpc_domain_admin_job_s {
  flowie_control_management_service_t *service;
  atomic_uint references;
  atomic_int completed;
  int result;
  flowie_control_management_caller_t caller;
  flowie_control_domain_admin_initialize_command_t command;
  flowie_control_command_result_t command_result;
  char caller_domain_id[FLOWIE_SECURITY_ID_MAX + 1u];
  char actor[FLOWIE_CONTROL_ACTOR_MAX + 1u];
  char command_domain_id[FLOWIE_SECURITY_ID_MAX + 1u];
  char principal_id[FLOWIE_SECURITY_ID_MAX + 1u];
  char request_id[FLOWIE_CONTROL_REQUEST_ID_MAX + 1u];
  uint8_t initial_password[FLOWIE_CONTROL_CREDENTIAL_SECRET_MAX];
} flowie_control_rpc_domain_admin_job_t;

static void flowie_control_rpc_method(Req *request, Res *response);
static int flowie_control_rpc_registered_method(Req *request, Res *response,
                                                rpc_request_t *rpc_request,
                                                rpc_response_t *rpc_response);
static json_value_t *
flowie_control_rpc_command_result(const flowie_control_command_result_t *result);
static int flowie_control_rpc_error(rpc_response_t *response, int rc);

static void flowie_control_rpc_policy_job_release(flowie_control_rpc_policy_job_t *job) {
  if (!job || atomic_fetch_sub_explicit(&job->references, 1u, memory_order_acq_rel) != 1u) return;
  for (size_t index = 0u; index < job->dry_run_change_count; ++index) {
    free((void *)job->dry_run_changes[index].subject_id);
    free((void *)job->dry_run_changes[index].document);
  }
  free(job->dry_run_changes);
  memset(job, 0, sizeof(*job));
  free(job);
}

static void flowie_control_rpc_policy_job_run(void *arg) {
  flowie_control_rpc_policy_job_t *job = (flowie_control_rpc_policy_job_t *)arg;
  if (!job) return;
  if (job->operation == FLOWIE_CONTROL_RPC_POLICY_VALIDATE) {
    job->result =
        flowie_control_management_policy_validate(job->service, &job->caller, &job->validation);
  } else if (job->operation == FLOWIE_CONTROL_RPC_POLICY_PUBLISH) {
    job->result = flowie_control_management_policy_publish(job->service, &job->caller,
                                                           &job->command, &job->publish_result);
  } else if (job->operation == FLOWIE_CONTROL_RPC_POLICY_DRY_RUN) {
    job->result =
        flowie_control_management_policy_dry_run(job->service, &job->caller, job->dry_run_changes,
                                                 job->dry_run_change_count, &job->dry_run_result);
  } else {
    job->result = SALTS_EINVAL;
  }
  atomic_store_explicit(&job->completed, 1, memory_order_release);
  flowie_control_rpc_policy_job_release(job);
}

static int flowie_control_rpc_policy_copy_text(char *destination, size_t capacity,
                                               const char *source) {
  size_t size;
  if (!destination || capacity == 0u || !source) return SALTS_EINVAL;
  size = strnlen(source, capacity);
  if (size == 0u || size >= capacity) return SALTS_EINVAL;
  memcpy(destination, source, size + 1u);
  return SALTS_OK;
}

static void
flowie_control_rpc_domain_admin_job_release(flowie_control_rpc_domain_admin_job_t *job) {
  if (!job || atomic_fetch_sub_explicit(&job->references, 1u, memory_order_acq_rel) != 1u) return;
  flowie_control_credential_wipe(job, sizeof(*job));
  free(job);
}

static void flowie_control_rpc_domain_admin_job_run(void *arg) {
  flowie_control_rpc_domain_admin_job_t *job = (flowie_control_rpc_domain_admin_job_t *)arg;
  if (!job) return;
  job->result = flowie_control_management_domain_admin_initialize(
      job->service, &job->caller, &job->command, &job->command_result);
  atomic_store_explicit(&job->completed, 1, memory_order_release);
  flowie_control_rpc_domain_admin_job_release(job);
}

static int
flowie_control_rpc_policy_execute(flowie_control_management_rpc_server_t *server, int operation,
                                  const flowie_control_management_caller_t *caller,
                                  const flowie_control_policy_publish_command_t *command,
                                  const flowie_control_policy_dry_run_change_t *dry_run_changes,
                                  size_t dry_run_change_count,
                                  flowie_control_policy_dry_run_result_t *dry_run_out,
                                  flowie_control_policy_validation_t *validation_out,
                                  flowie_control_policy_publish_result_t *publish_out) {
  flowie_control_rpc_policy_job_t *job;
  int completed;
  int wait_rc;
  int rc;
  if (!server || !caller || caller->size < sizeof(*caller) || !caller->domain_id ||
      !caller->actor ||
      (operation != FLOWIE_CONTROL_RPC_POLICY_VALIDATE &&
       operation != FLOWIE_CONTROL_RPC_POLICY_PUBLISH &&
       operation != FLOWIE_CONTROL_RPC_POLICY_DRY_RUN) ||
      (operation == FLOWIE_CONTROL_RPC_POLICY_VALIDATE &&
       (!validation_out || validation_out->size < sizeof(*validation_out))) ||
      (operation == FLOWIE_CONTROL_RPC_POLICY_PUBLISH &&
       (!command || command->size < sizeof(*command) || !command->domain_id || !command->actor ||
        !command->request_id || !publish_out || publish_out->size < sizeof(*publish_out))) ||
      (operation == FLOWIE_CONTROL_RPC_POLICY_DRY_RUN &&
       (!dry_run_changes || dry_run_change_count == 0u ||
        dry_run_change_count > FLOWIE_CONTROL_POLICY_DRY_RUN_MAX_CHANGES || !dry_run_out ||
        dry_run_out->size < sizeof(*dry_run_out) || !dry_run_out->diagnostics ||
        dry_run_out->diagnostic_capacity < dry_run_change_count)))
    return SALTS_EINVAL;

  job = (flowie_control_rpc_policy_job_t *)calloc(1u, sizeof(*job));
  if (!job) return SALTS_ENOMEM;
  job->service = server->service;
  job->operation = operation;
  job->caller = *caller;
  job->command =
      (flowie_control_policy_publish_command_t)FLOWIE_CONTROL_POLICY_PUBLISH_COMMAND_INIT;
  job->validation = (flowie_control_policy_validation_t)FLOWIE_CONTROL_POLICY_VALIDATION_INIT;
  job->dry_run_result =
      (flowie_control_policy_dry_run_result_t)FLOWIE_CONTROL_POLICY_DRY_RUN_RESULT_INIT;
  job->dry_run_result.diagnostics = job->dry_run_diagnostics;
  job->dry_run_result.diagnostic_capacity = FLOWIE_CONTROL_POLICY_DRY_RUN_MAX_CHANGES;
  job->publish_result =
      (flowie_control_policy_publish_result_t)FLOWIE_CONTROL_POLICY_PUBLISH_RESULT_INIT;
  job->result = SALTS_EIO;
  rc = flowie_control_rpc_policy_copy_text(job->caller_domain_id, sizeof(job->caller_domain_id),
                                           caller->domain_id);
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_policy_copy_text(job->actor, sizeof(job->actor), caller->actor);
  if (rc == SALTS_OK && operation == FLOWIE_CONTROL_RPC_POLICY_PUBLISH)
    rc = flowie_control_rpc_policy_copy_text(job->command_domain_id, sizeof(job->command_domain_id),
                                             command->domain_id);
  if (rc == SALTS_OK && operation == FLOWIE_CONTROL_RPC_POLICY_PUBLISH)
    rc = flowie_control_rpc_policy_copy_text(job->request_id, sizeof(job->request_id),
                                             command->request_id);
  if (rc == SALTS_OK && operation == FLOWIE_CONTROL_RPC_POLICY_DRY_RUN) {
    job->dry_run_changes = (flowie_control_policy_dry_run_change_t *)calloc(
        dry_run_change_count, sizeof(*job->dry_run_changes));
    if (!job->dry_run_changes) rc = SALTS_ENOMEM;
  }
  for (size_t index = 0u; rc == SALTS_OK && operation == FLOWIE_CONTROL_RPC_POLICY_DRY_RUN &&
                          index < dry_run_change_count;
       ++index) {
    const flowie_control_policy_dry_run_change_t *source = &dry_run_changes[index];
    flowie_control_policy_dry_run_change_t *destination = &job->dry_run_changes[index];
    char *subject_id = NULL;
    char *document = NULL;
    if (source->size < sizeof(*source) || !source->subject_id ||
        (source->operation == FLOWIE_CONTROL_POLICY_DRY_RUN_PUT &&
         (!source->document || source->document_size == 0u))) {
      rc = SALTS_EINVAL;
      break;
    }
    subject_id = (char *)malloc(strlen(source->subject_id) + 1u);
    if (!subject_id) {
      rc = SALTS_ENOMEM;
      break;
    }
    memcpy(subject_id, source->subject_id, strlen(source->subject_id) + 1u);
    if (source->document) {
      document = (char *)malloc(source->document_size + 1u);
      if (!document) {
        free(subject_id);
        rc = SALTS_ENOMEM;
        break;
      }
      memcpy(document, source->document, source->document_size);
      document[source->document_size] = '\0';
    }
    *destination = *source;
    destination->subject_id = subject_id;
    destination->document = document;
    ++job->dry_run_change_count;
  }
  if (rc != SALTS_OK) {
    atomic_init(&job->references, 1u);
    flowie_control_rpc_policy_job_release(job);
    return rc;
  }
  job->caller.domain_id = job->caller_domain_id;
  job->caller.actor = job->actor;
  if (operation == FLOWIE_CONTROL_RPC_POLICY_PUBLISH) {
    job->command = *command;
    job->command.domain_id = job->command_domain_id;
    job->command.actor = job->actor;
    job->command.request_id = job->request_id;
  }
  atomic_init(&job->references, 2u);
  atomic_init(&job->completed, 0);
  if (salts_threadpool_try_submit(server->policy_executor, flowie_control_rpc_policy_job_run,
                                  job) != SALTS_OK) {
    flowie_control_rpc_policy_job_release(job);
    flowie_control_rpc_policy_job_release(job);
    return SALTS_EBUSY;
  }
  wait_rc = flowie_control_async_wait(&job->completed, server->policy_executor_deadline_ms);
  completed = atomic_load_explicit(&job->completed, memory_order_acquire);
  if (completed) {
    rc = job->result;
    if (rc == SALTS_OK) {
      if (operation == FLOWIE_CONTROL_RPC_POLICY_VALIDATE) *validation_out = job->validation;
      else if (operation == FLOWIE_CONTROL_RPC_POLICY_PUBLISH) *publish_out = job->publish_result;
      else {
        size_t diagnostic_count = job->dry_run_result.diagnostic_count;
        if (diagnostic_count > dry_run_out->diagnostic_capacity) rc = SALTS_ENOSPC;
        else {
          flowie_control_policy_diagnostic_t *diagnostics = dry_run_out->diagnostics;
          size_t diagnostic_capacity = dry_run_out->diagnostic_capacity;
          *dry_run_out = job->dry_run_result;
          dry_run_out->diagnostics = diagnostics;
          dry_run_out->diagnostic_capacity = diagnostic_capacity;
          memcpy(diagnostics, job->dry_run_diagnostics, diagnostic_count * sizeof(*diagnostics));
        }
      }
    }
  } else {
    /* Accepted publish work may already commit; request_id keeps retries convergent. */
    rc = wait_rc == SALTS_OK ? SALTS_ETIMEDOUT : wait_rc;
  }
  flowie_control_rpc_policy_job_release(job);
  return rc;
}

static int flowie_control_rpc_domain_admin_execute(
    flowie_control_management_rpc_server_t *server,
    const flowie_control_management_caller_t *caller,
    const flowie_control_domain_admin_initialize_command_t *command,
    flowie_control_command_result_t *result) {
  flowie_control_rpc_domain_admin_job_t *job;
  int completed;
  int wait_rc;
  int rc;
  if (!server || !caller || !caller->domain_id || !caller->actor || !command ||
      !command->domain_id || !command->principal_id || !command->request_id ||
      (command->initial_password_size > 0u && !command->initial_password) ||
      command->initial_password_size > FLOWIE_CONTROL_CREDENTIAL_SECRET_MAX || !result)
    return SALTS_EINVAL;
  job = (flowie_control_rpc_domain_admin_job_t *)calloc(1u, sizeof(*job));
  if (!job) return SALTS_ENOMEM;
  job->service = server->service;
  job->caller = *caller;
  job->command = *command;
  job->command_result = (flowie_control_command_result_t)FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  job->result = SALTS_EIO;
  rc = flowie_control_rpc_policy_copy_text(job->caller_domain_id, sizeof(job->caller_domain_id),
                                           caller->domain_id);
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_policy_copy_text(job->actor, sizeof(job->actor), caller->actor);
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_policy_copy_text(job->command_domain_id, sizeof(job->command_domain_id),
                                             command->domain_id);
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_policy_copy_text(job->principal_id, sizeof(job->principal_id),
                                             command->principal_id);
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_policy_copy_text(job->request_id, sizeof(job->request_id),
                                             command->request_id);
  if (rc == SALTS_OK && command->initial_password_size > 0u)
    memcpy(job->initial_password, command->initial_password, command->initial_password_size);
  if (rc != SALTS_OK) {
    flowie_control_credential_wipe(job, sizeof(*job));
    free(job);
    return rc;
  }
  job->caller.domain_id = job->caller_domain_id;
  job->caller.actor = job->actor;
  job->command.domain_id = job->command_domain_id;
  job->command.principal_id = job->principal_id;
  job->command.initial_password = job->initial_password;
  job->command.actor = job->actor;
  job->command.request_id = job->request_id;
  atomic_init(&job->references, 2u);
  atomic_init(&job->completed, 0);
  if (salts_threadpool_try_submit(server->policy_executor, flowie_control_rpc_domain_admin_job_run,
                                  job) != SALTS_OK) {
    flowie_control_rpc_domain_admin_job_release(job);
    flowie_control_rpc_domain_admin_job_release(job);
    return SALTS_EBUSY;
  }
  wait_rc = flowie_control_async_wait(&job->completed, server->policy_executor_deadline_ms);
  completed = atomic_load_explicit(&job->completed, memory_order_acquire);
  if (completed) {
    rc = job->result;
    if (rc == SALTS_OK) *result = job->command_result;
  } else {
    /* The resumable command converges through derived request IDs after a timeout. */
    rc = wait_rc == SALTS_OK ? SALTS_ETIMEDOUT : wait_rc;
  }
  flowie_control_rpc_domain_admin_job_release(job);
  return rc;
}

static void flowie_control_rpc_free_json_value(json_value_t *value) {
  json_value_t *owned = (json_value_t *)value;
  if (owned) json_free(owned);
}

static int flowie_control_rpc_add(json_value_t *object, const char *key, json_value_t *value) {
  if (value && json_object_add_checked(object, key, value)) return SALTS_OK;
  flowie_control_rpc_free_json_value(value);
  return SALTS_ENOMEM;
}

static int flowie_control_rpc_array_add(json_value_t *array, json_value_t *value) {
  if (value && json_array_add_checked(array, value)) return SALTS_OK;
  flowie_control_rpc_free_json_value(value);
  return SALTS_ENOMEM;
}

static int flowie_control_rpc_result(rpc_response_t *response, json_value_t *value) {
  char *json;
  size_t json_size = 0u;
  if (!response || !value) return SALTS_EINVAL;
  json = json_serialize(value, &json_size);
  flowie_control_rpc_free_json_value(value);
  if (!json || json_size == 0u) {
    json_serialize_free(json);
    return SALTS_ENOMEM;
  }
  rpc_set_result(response, json);
  json_serialize_free(json);
  return response->result ? SALTS_OK : SALTS_ENOMEM;
}

static int flowie_control_rpc_params(const rpc_request_t *request, const char *const *allowed,
                                     size_t allowed_count, json_value_t **document_out) {
  json_value_t *document = NULL;
  if (document_out) *document_out = NULL;
  if (!request || !document_out) return SALTS_EINVAL;
  if (!request->params) {
    document = (json_value_t *)json_create_object();
  } else if (!(document = json_parse(request->params, strlen(request->params))) ||
             json_type(document) != JSON_OBJECT) {
    json_free(document);
    return SALTS_EPROTO;
  }
  if (!document) return SALTS_ENOMEM;
  for (size_t index = 0u; index < json_object_size(document); ++index) {
    const char *key = json_object_key(document, index);
    int known = 0;
    for (size_t allowed_index = 0u; allowed_index < allowed_count; ++allowed_index) {
      if (key && strcmp(key, allowed[allowed_index]) == 0) {
        known = 1;
        break;
      }
    }
    if (!known) {
      json_free(document);
      return SALTS_EPROTO;
    }
  }
  *document_out = document;
  return SALTS_OK;
}

static int flowie_control_rpc_object_allowed(const json_value_t *object, const char *const *allowed,
                                             size_t allowed_count) {
  if (!object || json_type(object) != JSON_OBJECT) return SALTS_EPROTO;
  for (size_t index = 0u; index < json_object_size(object); ++index) {
    const char *key = json_object_key(object, index);
    int known = 0;
    for (size_t allowed_index = 0u; allowed_index < allowed_count; ++allowed_index) {
      if (key && strcmp(key, allowed[allowed_index]) == 0) {
        known = 1;
        break;
      }
    }
    if (!known) return SALTS_EPROTO;
  }
  return SALTS_OK;
}

static int flowie_control_rpc_string(const json_value_t *object, const char *key, size_t maximum,
                                     int required, const char **out) {
  json_value_t *value;
  const char *text;
  size_t size;
  if (out) *out = NULL;
  if (!object || !key || maximum == 0u || !out) return SALTS_EINVAL;
  value = json_object_get(object, key);
  if (!value) return required ? SALTS_EPROTO : SALTS_OK;
  if (json_type(value) != JSON_STRING) return SALTS_EPROTO;
  text = json_string(value);
  size = json_string_len(value);
  if (!text || size == 0u || size > maximum || memchr(text, '\0', size)) return SALTS_EPROTO;
  *out = text;
  return SALTS_OK;
}

static int flowie_control_rpc_u64(const json_value_t *object, const char *key, int required,
                                  uint64_t *out) {
  json_value_t *value;
  const char *text;
  char buffer[32];
  char *end = NULL;
  size_t size = 0u;
  unsigned long long parsed;
  if (!object || !key || !out) return SALTS_EINVAL;
  value = json_object_get(object, key);
  if (!value) return required ? SALTS_EPROTO : SALTS_OK;
  if (json_type(value) != JSON_NUMBER) return SALTS_EPROTO;
  text = json_number_text(value, &size);
  if (!text || size == 0u || size >= sizeof(buffer)) return SALTS_EPROTO;
  memcpy(buffer, text, size);
  buffer[size] = '\0';
  if (buffer[0] == '-' || buffer[0] == '+' || (size > 1u && buffer[0] == '0')) return SALTS_EPROTO;
  parsed = strtoull(buffer, &end, 10);
  if (!end || *end != '\0') return SALTS_EPROTO;
  *out = (uint64_t)parsed;
  return SALTS_OK;
}

static int flowie_control_rpc_page_limit(const json_value_t *params, size_t *limit_out) {
  uint64_t limit = FLOWIE_CONTROL_RPC_DEFAULT_PAGE;
  int rc = flowie_control_rpc_u64(params, "limit", 0, &limit);
  if (rc != SALTS_OK || limit == 0u || limit > FLOWIE_CONTROL_PAGE_MAX) return SALTS_EPROTO;
  *limit_out = (size_t)limit;
  return SALTS_OK;
}

static int flowie_control_rpc_target_root(const json_value_t *params,
                                          const flowie_control_management_caller_t *caller,
                                          const char **domain_id_out) {
  const char *domain_id = NULL;
  int rc;
  if (domain_id_out) *domain_id_out = NULL;
  if (!params || !caller || !caller->domain_id || !domain_id_out) return SALTS_EINVAL;
  rc = flowie_control_rpc_string(params, "domain_id", FLOWIE_SECURITY_ID_MAX, 0, &domain_id);
  if (rc != SALTS_OK) return rc;
  if (!domain_id) domain_id = caller->domain_id;
  if (strcmp(domain_id, caller->domain_id) != 0) return SALTS_EPERM;
  *domain_id_out = domain_id;
  return SALTS_OK;
}

static int flowie_control_rpc_scope(flowie_control_management_rpc_server_t *server,
                                    const json_value_t *params,
                                    const flowie_control_management_caller_t *caller,
                                    flowie_control_management_caller_t *scoped_out) {
  const char *domain_id = NULL;
  int rc;
  if (!server || !params || !caller || !scoped_out) return SALTS_EINVAL;
  rc = flowie_control_rpc_target_root(params, caller, &domain_id);
  if (rc != SALTS_OK) return rc;
  return flowie_control_management_scope_caller(server->service, caller, domain_id, scoped_out);
}

static int flowie_control_rpc_domain_create(flowie_control_management_rpc_server_t *server,
                                            const flowie_control_management_caller_t *caller,
                                            const rpc_request_t *request,
                                            rpc_response_t *response) {
  static const char *const allowed[] = {"domain_id", "request_id"};
  json_value_t *params = NULL;
  flowie_control_command_result_t result = FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  flowie_control_domain_create_command_t command = FLOWIE_CONTROL_DOMAIN_CREATE_COMMAND_INIT;
  const char *domain_id = NULL;
  const char *request_id = NULL;
  int rc = flowie_control_rpc_params(request, allowed, 2u, &params);
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_string(params, "domain_id", FLOWIE_SECURITY_ID_MAX, 1, &domain_id);
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_string(params, "request_id", FLOWIE_CONTROL_REQUEST_ID_MAX, 1,
                                   &request_id);
  if (rc == SALTS_OK) {
    command.domain_id = domain_id;
    command.actor = caller->actor;
    command.request_id = request_id;
    command.occurred_at = server->clock(server->clock_ctx);
    if (command.occurred_at == 0u) rc = SALTS_EIO;
    else rc = flowie_control_management_domain_create(server->service, caller, &command, &result);
  }
  json_free(params);
  return rc == SALTS_OK
             ? flowie_control_rpc_result(response, flowie_control_rpc_command_result(&result))
             : flowie_control_rpc_error(response, rc);
}

static int
flowie_control_rpc_domain_admin_initialize(flowie_control_management_rpc_server_t *server,
                                           const flowie_control_management_caller_t *caller,
                                           const rpc_request_t *request, rpc_response_t *response) {
  static const char *const allowed[] = {"domain_id", "principal_id", "initial_password",
                                        "request_id"};
  json_value_t *params = NULL;
  flowie_control_domain_admin_initialize_command_t command =
      FLOWIE_CONTROL_DOMAIN_ADMIN_INITIALIZE_COMMAND_INIT;
  flowie_control_command_result_t result = FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  const char *domain_id = NULL;
  const char *principal_id = NULL;
  const char *initial_password = NULL;
  const char *request_id = NULL;
  size_t password_size = 0u;
  int rc = flowie_control_rpc_params(request, allowed, 4u, &params);
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_string(params, "domain_id", FLOWIE_SECURITY_ID_MAX, 1, &domain_id);
  if (rc == SALTS_OK)
    rc =
        flowie_control_rpc_string(params, "principal_id", FLOWIE_SECURITY_ID_MAX, 1, &principal_id);
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_string(params, "initial_password", FLOWIE_CONTROL_CREDENTIAL_SECRET_MAX,
                                   1, &initial_password);
  if (rc == SALTS_OK) {
    password_size = json_string_len(json_object_get(params, "initial_password"));
    if (password_size < FLOWIE_CONTROL_HUMAN_PASSWORD_MIN_SIZE) rc = SALTS_EINVAL;
  }
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_string(params, "request_id", FLOWIE_CONTROL_REQUEST_ID_MAX, 1,
                                   &request_id);
  if (rc == SALTS_OK) {
    command.domain_id = domain_id;
    command.principal_id = principal_id;
    command.initial_password = initial_password;
    command.initial_password_size = password_size;
    command.actor = caller->actor;
    command.request_id = request_id;
    command.occurred_at = server->clock(server->clock_ctx);
    if (command.occurred_at == 0u) rc = SALTS_EIO;
    else rc = flowie_control_rpc_domain_admin_execute(server, caller, &command, &result);
  }
  if (initial_password) flowie_control_credential_wipe((void *)initial_password, password_size);
  json_free(params);
  return rc == SALTS_OK
             ? flowie_control_rpc_result(response, flowie_control_rpc_command_result(&result))
             : flowie_control_rpc_error(response, rc);
}

static int flowie_control_rpc_password_change(flowie_control_management_rpc_server_t *server,
                                              const flowie_control_management_caller_t *caller,
                                              const rpc_request_t *request,
                                              rpc_response_t *response) {
  static const char *const allowed[] = {"new_password", "request_id"};
  json_value_t *params = NULL;
  flowie_control_password_change_command_t command = FLOWIE_CONTROL_PASSWORD_CHANGE_COMMAND_INIT;
  flowie_control_command_result_t result = FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  const char *new_password = NULL;
  const char *request_id = NULL;
  size_t password_size = 0u;
  int rc = flowie_control_rpc_params(request, allowed, 3u, &params);
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_string(params, "new_password", FLOWIE_CONTROL_CREDENTIAL_SECRET_MAX, 1,
                                   &new_password);
  if (rc == SALTS_OK) {
    password_size = strlen(new_password);
    if (password_size < FLOWIE_CONTROL_HUMAN_PASSWORD_MIN_SIZE) rc = SALTS_ERANGE;
  }
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_string(params, "request_id", FLOWIE_CONTROL_REQUEST_ID_MAX, 1,
                                   &request_id);
  if (rc == SALTS_OK) {
    command.new_password = new_password;
    command.new_password_size = password_size;
    command.request_id = request_id;
    command.occurred_at = server->clock(server->clock_ctx);
    if (command.occurred_at == 0u) rc = SALTS_EIO;
    else rc = flowie_control_management_password_change(server->service, caller, &command, &result);
  }
  if (new_password) flowie_control_credential_wipe((void *)new_password, password_size);
  json_free(params);
  return rc == SALTS_OK
             ? flowie_control_rpc_result(response, flowie_control_rpc_command_result(&result))
             : flowie_control_rpc_error(response, rc);
}

static int flowie_control_rpc_password_set(flowie_control_management_rpc_server_t *server,
                                           const flowie_control_management_caller_t *caller,
                                           const rpc_request_t *request, rpc_response_t *response) {
  static const char *const allowed[] = {"domain_id", "principal_id", "new_password", "mode",
                                        "request_id"};
  json_value_t *params = NULL;
  flowie_control_password_set_command_t command = FLOWIE_CONTROL_PASSWORD_SET_COMMAND_INIT;
  flowie_control_command_result_t result = FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  const char *domain_id = NULL;
  const char *principal_id = NULL;
  const char *new_password = NULL;
  const char *mode = NULL;
  const char *request_id = NULL;
  size_t password_size = 0u;
  int rc = flowie_control_rpc_params(request, allowed, 5u, &params);
  if (rc == SALTS_OK) rc = flowie_control_rpc_target_root(params, caller, &domain_id);
  if (rc == SALTS_OK)
    rc =
        flowie_control_rpc_string(params, "principal_id", FLOWIE_SECURITY_ID_MAX, 1, &principal_id);
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_string(params, "new_password", FLOWIE_CONTROL_CREDENTIAL_SECRET_MAX, 1,
                                   &new_password);
  if (rc == SALTS_OK) {
    password_size = json_string_len(json_object_get(params, "new_password"));
    if (password_size < FLOWIE_CONTROL_HUMAN_PASSWORD_MIN_SIZE) rc = SALTS_EINVAL;
  }
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_string(params, "mode", sizeof("replace") - 1u, 1, &mode);
  if (rc == SALTS_OK) {
    if (strcmp(mode, "create") == 0) command.mode = FLOWIE_CONTROL_PASSWORD_CREATE;
    else if (strcmp(mode, "replace") == 0) command.mode = FLOWIE_CONTROL_PASSWORD_REPLACE;
    else rc = SALTS_EPROTO;
  }
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_string(params, "request_id", FLOWIE_CONTROL_REQUEST_ID_MAX, 1,
                                   &request_id);
  if (rc == SALTS_OK) {
    command.domain_id = domain_id;
    command.principal_id = principal_id;
    command.new_password = new_password;
    command.new_password_size = password_size;
    command.actor = caller->actor;
    command.request_id = request_id;
    command.occurred_at = server->clock(server->clock_ctx);
    if (command.occurred_at == 0u) rc = SALTS_EIO;
    else rc = flowie_control_management_password_set(server->service, caller, &command, &result);
  }
  if (rc == SALTS_EALREADY) rc = SALTS_EBUSY;
  if (new_password) flowie_control_credential_wipe((void *)new_password, password_size);
  json_free(params);
  return rc == SALTS_OK
             ? flowie_control_rpc_result(response, flowie_control_rpc_command_result(&result))
             : flowie_control_rpc_error(response, rc);
}

static json_value_t *
flowie_control_rpc_command_result(const flowie_control_command_result_t *result) {
  json_value_t *object = json_create_object();
  if (!object ||
      flowie_control_rpc_add(object, "replayed", json_create_bool(result->replayed != 0)) !=
          SALTS_OK) {
    flowie_control_rpc_free_json_value(object);
    return NULL;
  }
  return object;
}

static int flowie_control_rpc_error(rpc_response_t *response, int rc) {
  switch (rc) {
  case SALTS_EINVAL:
  case SALTS_EPROTO:
  case SALTS_ERANGE:
  case SALTS_ENOSPC:
    rpc_set_error(response, RPC_ERROR_INVALID_PARAMS, "Invalid params");
    break;
  case SALTS_EPERM:
    rpc_set_error(response, FLOWIE_CONTROL_RPC_FORBIDDEN, "Forbidden");
    break;
  case SALTS_ENOENT:
    rpc_set_error(response, FLOWIE_CONTROL_RPC_NOT_FOUND, "Not found");
    break;
  case SALTS_EBUSY:
    rpc_set_error(response, FLOWIE_CONTROL_RPC_CONFLICT, "Concurrent update conflict");
    break;
  default:
    rpc_set_error(response, RPC_ERROR_INTERNAL, "Internal error");
    break;
  }
  return rc;
}

static json_value_t *flowie_control_rpc_user(const flowie_control_user_view_t *user) {
  json_value_t *object = json_create_object();
  if (!object ||
      flowie_control_rpc_add(object, "id", json_create_string(user->principal_id)) !=
          SALTS_OK ||
      flowie_control_rpc_add(object, "type", json_create_string(user->principal_type)) !=
          SALTS_OK ||
      flowie_control_rpc_add(object, "enabled", json_create_bool(user->enabled != 0)) !=
          SALTS_OK ||
      flowie_control_rpc_add(object, "created_at", json_create_uint64(user->created_at)) !=
          SALTS_OK ||
      flowie_control_rpc_add(object, "updated_at", json_create_uint64(user->updated_at)) !=
          SALTS_OK) {
    flowie_control_rpc_free_json_value(object);
    return NULL;
  }
  return object;
}

static json_value_t *flowie_control_rpc_group(const flowie_control_group_view_t *group) {
  json_value_t *object = json_create_object();
  if (!object ||
      flowie_control_rpc_add(object, "id", json_create_string(group->group_id)) != SALTS_OK ||
      flowie_control_rpc_add(object, "parent_id",
                             group->parent_group_id[0]
                                 ? json_create_string(group->parent_group_id)
                                 : json_create_null()) != SALTS_OK ||
      flowie_control_rpc_add(object, "depth", json_create_uint64(group->depth)) != SALTS_OK ||
      flowie_control_rpc_add(object, "enabled", json_create_bool(group->enabled != 0)) !=
          SALTS_OK) {
    flowie_control_rpc_free_json_value(object);
    return NULL;
  }
  return object;
}

static json_value_t *flowie_control_rpc_role(const flowie_control_role_view_t *role) {
  json_value_t *object = json_create_object();
  if (!object ||
      flowie_control_rpc_add(object, "id", json_create_string(role->role_id)) != SALTS_OK ||
      flowie_control_rpc_add(object, "enabled", json_create_bool(role->enabled != 0)) !=
          SALTS_OK) {
    flowie_control_rpc_free_json_value(object);
    return NULL;
  }
  return object;
}

static int flowie_control_rpc_domain_list(flowie_control_management_rpc_server_t *server,
                                          const flowie_control_management_caller_t *caller,
                                          const rpc_request_t *request, rpc_response_t *response) {
  static const char *const allowed[] = {"after", "limit"};
  json_value_t *params = NULL;
  flowie_control_domain_view_t *items = NULL;
  json_value_t *result = NULL;
  json_value_t *array = NULL;
  const char *after = NULL;
  size_t capacity = 0u;
  size_t count = 0u;
  int has_more = 0;
  int rc = flowie_control_rpc_params(request, allowed, 2u, &params);
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_string(params, "after", FLOWIE_SECURITY_ID_MAX, 0, &after);
  if (rc == SALTS_OK) rc = flowie_control_rpc_page_limit(params, &capacity);
  if (rc == SALTS_OK) {
    items = (flowie_control_domain_view_t *)calloc(capacity, sizeof(*items));
    if (!items) rc = SALTS_ENOMEM;
  }
  for (size_t index = 0u; rc == SALTS_OK && index < capacity; ++index)
    items[index] = (flowie_control_domain_view_t)FLOWIE_CONTROL_DOMAIN_VIEW_INIT;
  if (rc == SALTS_OK)
    rc = flowie_control_management_domain_list(server->service, caller, after, items, capacity,
                                               &count, &has_more);
  if (rc == SALTS_OK) {
    result = json_create_object();
    array = json_create_array();
    if (!result || !array) rc = SALTS_ENOMEM;
  }
  for (size_t index = 0u; rc == SALTS_OK && index < count; ++index) {
    json_value_t *item = json_create_object();
    if (!item ||
        flowie_control_rpc_add(item, "domain_id",
                               json_create_string(items[index].domain_id)) != SALTS_OK) {
      flowie_control_rpc_free_json_value(item);
      rc = SALTS_ENOMEM;
    } else {
      rc = flowie_control_rpc_array_add(array, item);
    }
  }
  if (rc == SALTS_OK) {
    rc = flowie_control_rpc_add(result, "items", array);
    if (rc == SALTS_OK) array = NULL;
  }
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_add(result, "has_more", json_create_bool(has_more != 0));
  free(items);
  json_free(params);
  flowie_control_rpc_free_json_value(array);
  if (rc != SALTS_OK) {
    flowie_control_rpc_free_json_value(result);
    return flowie_control_rpc_error(response, rc);
  }
  return flowie_control_rpc_result(response, result);
}

static int flowie_control_rpc_system_status(flowie_control_management_rpc_server_t *server,
                                            const flowie_control_management_caller_t *caller,
                                            const rpc_request_t *request,
                                            rpc_response_t *response) {
  static const char *const allowed[] = {"domain_id"};
  flowie_control_management_status_t status = FLOWIE_CONTROL_MANAGEMENT_STATUS_INIT;
  flowie_control_management_caller_t scoped = FLOWIE_CONTROL_MANAGEMENT_CALLER_INIT;
  char domain_id[FLOWIE_SECURITY_ID_MAX + 1u] = "";
  json_value_t *params = NULL;
  json_value_t *object = NULL;
  int rc = flowie_control_rpc_params(request, allowed, 1u, &params);
  if (rc == SALTS_OK) rc = flowie_control_rpc_scope(server, params, caller, &scoped);
  if (rc == SALTS_OK)
    rc = flowie_control_management_system_status(server->service, &scoped, &status);
  if (rc == SALTS_OK) memcpy(domain_id, scoped.domain_id, strlen(scoped.domain_id) + 1u);
  json_free(params);
  if (rc != SALTS_OK) return flowie_control_rpc_error(response, rc);
  object = json_create_object();
  if (!object ||
      flowie_control_rpc_add(object, "domain", json_create_string(domain_id)) != SALTS_OK ||
      flowie_control_rpc_add(object, "policy_version",
                             json_create_uint64(status.policy.policy_version)) != SALTS_OK ||
      flowie_control_rpc_add(object, "draft_rules",
                             json_create_uint64(status.policy.draft_rule_count)) !=
          SALTS_OK ||
      flowie_control_rpc_add(object, "published_rules",
                             json_create_uint64(status.policy.published_rule_count)) !=
          SALTS_OK) {
    flowie_control_rpc_free_json_value(object);
    return flowie_control_rpc_error(response, SALTS_ENOMEM);
  }
  return flowie_control_rpc_result(response, object);
}

static int flowie_control_rpc_external_https_stats(flowie_control_management_rpc_server_t *server,
                                                   const flowie_control_management_caller_t *caller,
                                                   const rpc_request_t *request,
                                                   rpc_response_t *response) {
  flowie_control_external_https_authenticator_stats_t stats =
      FLOWIE_CONTROL_EXTERNAL_HTTPS_AUTHENTICATOR_STATS_INIT;
  json_value_t *params = NULL;
  json_value_t *object = NULL;
  int rc = flowie_control_rpc_params(request, NULL, 0u, &params);
  if (rc == SALTS_OK)
    rc = flowie_control_management_authorize(server->service, caller,
                                             FLOWIE_CONTROL_MANAGEMENT_SYSTEM_ADMIN);
  if (rc == SALTS_OK && strcmp(caller->domain_id, FLOWIE_CONTROL_MANAGEMENT_SYSTEM_DOMAIN) != 0)
    rc = SALTS_EPERM;
  if (rc == SALTS_OK) rc = server->external_https_stats(server->external_https_stats_ctx, &stats);
  json_free(params);
  if (rc != SALTS_OK && rc != SALTS_ENOENT) return flowie_control_rpc_error(response, rc);
  object = json_create_object();
  if (!object || flowie_control_rpc_add(object, "enabled",
                                        json_create_bool(rc == SALTS_OK)) != SALTS_OK) {
    flowie_control_rpc_free_json_value(object);
    return flowie_control_rpc_error(response, SALTS_ENOMEM);
  }
  if (rc == SALTS_ENOENT) return flowie_control_rpc_result(response, object);
  if (flowie_control_rpc_add(object, "started_requests",
                             json_create_uint64(stats.started_requests)) != SALTS_OK ||
      flowie_control_rpc_add(object, "in_flight", json_create_uint64(stats.in_flight)) !=
          SALTS_OK ||
      flowie_control_rpc_add(object, "succeeded", json_create_uint64(stats.succeeded)) !=
          SALTS_OK ||
      flowie_control_rpc_add(object, "denied", json_create_uint64(stats.denied)) !=
          SALTS_OK ||
      flowie_control_rpc_add(object, "local_overload",
                             json_create_uint64(stats.local_overload)) != SALTS_OK ||
      flowie_control_rpc_add(object, "remote_overload",
                             json_create_uint64(stats.remote_overload)) != SALTS_OK ||
      flowie_control_rpc_add(object, "remote_server_failures",
                             json_create_uint64(stats.remote_server_failures)) != SALTS_OK ||
      flowie_control_rpc_add(object, "transport_failures",
                             json_create_uint64(stats.transport_failures)) != SALTS_OK ||
      flowie_control_rpc_add(object, "protocol_failures",
                             json_create_uint64(stats.protocol_failures)) != SALTS_OK ||
      flowie_control_rpc_add(object, "local_failures",
                             json_create_uint64(stats.local_failures)) != SALTS_OK) {
    flowie_control_rpc_free_json_value(object);
    return flowie_control_rpc_error(response, SALTS_ENOMEM);
  }
  return flowie_control_rpc_result(response, object);
}

static int flowie_control_rpc_user_get(flowie_control_management_rpc_server_t *server,
                                       const flowie_control_management_caller_t *caller,
                                       const rpc_request_t *request, rpc_response_t *response) {
  static const char *const allowed[] = {"domain_id", "principal_id"};
  flowie_control_management_caller_t scoped = FLOWIE_CONTROL_MANAGEMENT_CALLER_INIT;
  flowie_control_user_view_t user = FLOWIE_CONTROL_USER_VIEW_INIT;
  json_value_t *params = NULL;
  const char *principal_id = NULL;
  int rc = flowie_control_rpc_params(request, allowed, 2u, &params);
  if (rc == SALTS_OK) rc = flowie_control_rpc_scope(server, params, caller, &scoped);
  if (rc == SALTS_OK)
    rc =
        flowie_control_rpc_string(params, "principal_id", FLOWIE_SECURITY_ID_MAX, 1, &principal_id);
  if (rc == SALTS_OK)
    rc = flowie_control_management_user_get(server->service, &scoped, principal_id, &user);
  json_free(params);
  return rc == SALTS_OK ? flowie_control_rpc_result(response, flowie_control_rpc_user(&user))
                        : flowie_control_rpc_error(response, rc);
}

static int flowie_control_rpc_user_list(flowie_control_management_rpc_server_t *server,
                                        const flowie_control_management_caller_t *caller,
                                        const rpc_request_t *request, rpc_response_t *response) {
  static const char *const allowed[] = {"domain_id", "after", "limit"};
  flowie_control_management_caller_t scoped = FLOWIE_CONTROL_MANAGEMENT_CALLER_INIT;
  json_value_t *params = NULL;
  flowie_control_user_view_t *items = NULL;
  json_value_t *result = NULL;
  json_value_t *array = NULL;
  const char *after = NULL;
  size_t capacity = 0u;
  size_t count = 0u;
  int has_more = 0;
  int rc = flowie_control_rpc_params(request, allowed, 3u, &params);
  if (rc == SALTS_OK) rc = flowie_control_rpc_scope(server, params, caller, &scoped);
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_string(params, "after", FLOWIE_SECURITY_ID_MAX, 0, &after);
  if (rc == SALTS_OK) rc = flowie_control_rpc_page_limit(params, &capacity);
  if (rc == SALTS_OK) {
    items = (flowie_control_user_view_t *)calloc(capacity, sizeof(*items));
    if (!items) rc = SALTS_ENOMEM;
  }
  for (size_t index = 0u; rc == SALTS_OK && index < capacity; ++index)
    items[index] = (flowie_control_user_view_t)FLOWIE_CONTROL_USER_VIEW_INIT;
  if (rc == SALTS_OK)
    rc = flowie_control_management_user_list(server->service, &scoped, after, items, capacity,
                                             &count, &has_more);
  if (rc == SALTS_OK) {
    result = json_create_object();
    array = json_create_array();
    if (!result || !array) rc = SALTS_ENOMEM;
  }
  for (size_t index = 0u; rc == SALTS_OK && index < count; ++index)
    rc = flowie_control_rpc_array_add(array, flowie_control_rpc_user(&items[index]));
  if (rc == SALTS_OK) {
    rc = flowie_control_rpc_add(result, "items", array);
    if (rc == SALTS_OK) array = NULL;
  }
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_add(result, "has_more", json_create_bool(has_more != 0));
  free(items);
  json_free(params);
  flowie_control_rpc_free_json_value(array);
  if (rc != SALTS_OK) {
    flowie_control_rpc_free_json_value(result);
    return flowie_control_rpc_error(response, rc);
  }
  return flowie_control_rpc_result(response, result);
}

static int flowie_control_rpc_user_write(flowie_control_management_rpc_server_t *server,
                                         const flowie_control_management_caller_t *caller,
                                         const rpc_request_t *request, rpc_response_t *response,
                                         int disable) {
  static const char *const create_allowed[] = {"domain_id", "principal_id", "principal_type",
                                               "request_id"};
  static const char *const disable_allowed[] = {"domain_id", "principal_id", "request_id"};
  json_value_t *params = NULL;
  flowie_control_command_result_t result = FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  const char *principal_id = NULL;
  const char *principal_type = NULL;
  const char *request_id = NULL;
  const char *domain_id = NULL;
  uint64_t occurred_at = 0u;
  int rc = flowie_control_rpc_params(request, disable ? disable_allowed : create_allowed,
                                     disable ? 3u : 4u, &params);
  if (rc == SALTS_OK) rc = flowie_control_rpc_target_root(params, caller, &domain_id);
  if (rc == SALTS_OK)
    rc =
        flowie_control_rpc_string(params, "principal_id", FLOWIE_SECURITY_ID_MAX, 1, &principal_id);
  if (rc == SALTS_OK && !disable)
    rc = flowie_control_rpc_string(params, "principal_type", FLOWIE_SECURITY_TYPE_MAX, 1,
                                   &principal_type);
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_string(params, "request_id", FLOWIE_CONTROL_REQUEST_ID_MAX, 1,
                                   &request_id);
  if (rc == SALTS_OK) occurred_at = server->clock(server->clock_ctx);
  if (rc == SALTS_OK && occurred_at == 0u) rc = SALTS_EIO;
  if (rc == SALTS_OK && disable) {
    flowie_control_user_disable_command_t command = FLOWIE_CONTROL_USER_DISABLE_COMMAND_INIT;
    command.domain_id = domain_id;
    command.principal_id = principal_id;
    command.actor = caller->actor;
    command.request_id = request_id;
    command.occurred_at = occurred_at;
    rc = flowie_control_management_user_disable(server->service, caller, &command, &result);
  } else if (rc == SALTS_OK) {
    flowie_control_user_create_command_t command = FLOWIE_CONTROL_USER_CREATE_COMMAND_INIT;
    command.domain_id = domain_id;
    command.principal_id = principal_id;
    command.principal_type = principal_type;
    command.actor = caller->actor;
    command.request_id = request_id;
    command.occurred_at = occurred_at;
    rc = flowie_control_management_user_create(server->service, caller, &command, &result);
  }
  json_free(params);
  return rc == SALTS_OK
             ? flowie_control_rpc_result(response, flowie_control_rpc_command_result(&result))
             : flowie_control_rpc_error(response, rc);
}

static int flowie_control_rpc_credential_issue(flowie_control_management_rpc_server_t *server,
                                               const flowie_control_management_caller_t *caller,
                                               const rpc_request_t *request,
                                               rpc_response_t *response, int rotate) {
  static const char *const allowed[] = {"domain_id", "principal_id", "request_id"};
  json_value_t *params = NULL;
  flowie_control_generated_credential_t generated = FLOWIE_CONTROL_GENERATED_CREDENTIAL_INIT;
  json_value_t *object = NULL;
  const char *principal_id = NULL;
  const char *request_id = NULL;
  const char *domain_id = NULL;
  uint64_t occurred_at = 0u;
  int rc = flowie_control_rpc_params(request, allowed, 3u, &params);
  if (rc == SALTS_OK) rc = flowie_control_rpc_target_root(params, caller, &domain_id);
  if (rc == SALTS_OK)
    rc =
        flowie_control_rpc_string(params, "principal_id", FLOWIE_SECURITY_ID_MAX, 1, &principal_id);
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_string(params, "request_id", FLOWIE_CONTROL_REQUEST_ID_MAX, 1,
                                   &request_id);
  if (rc == SALTS_OK) {
    occurred_at = server->clock(server->clock_ctx);
    if (occurred_at == 0u) rc = SALTS_EIO;
  }
  if (rc == SALTS_OK) {
    flowie_control_credential_issue_command_t command =
        FLOWIE_CONTROL_CREDENTIAL_ISSUE_COMMAND_INIT;
    command.domain_id = domain_id;
    command.principal_id = principal_id;
    command.actor = caller->actor;
    command.request_id = request_id;
    command.occurred_at = occurred_at;
    rc = rotate ? flowie_control_management_credential_rotate(server->service, caller, &command,
                                                              &generated)
                : flowie_control_management_credential_generate(server->service, caller, &command,
                                                                &generated);
  }
  json_free(params);
  if (rc == SALTS_EALREADY) {
    flowie_control_generated_credential_wipe(&generated);
    rpc_set_error(response, FLOWIE_CONTROL_RPC_SECRET_UNAVAILABLE,
                  "Credential token is unavailable; use a new request_id");
    return rc;
  }
  if (rc != SALTS_OK) {
    flowie_control_generated_credential_wipe(&generated);
    return flowie_control_rpc_error(response, rc);
  }
  if (generated.token_size != FLOWIE_CONTROL_CREDENTIAL_TOKEN_SIZE ||
      generated.token[generated.token_size] != '\0') {
    rc = SALTS_EIO;
    goto done;
  }
  object = json_create_object();
  if (!object || flowie_control_rpc_add(object, "token",
                                        json_create_string(generated.token)) != SALTS_OK) {
    rc = SALTS_ENOMEM;
    goto done;
  }
  rc = flowie_control_rpc_result(response, object);
  object = NULL;

done:
  flowie_control_rpc_free_json_value(object);
  flowie_control_generated_credential_wipe(&generated);
  if (rc != SALTS_OK) return flowie_control_rpc_error(response, rc);
  return SALTS_OK;
}

static int flowie_control_rpc_credential_revoke(flowie_control_management_rpc_server_t *server,
                                                const flowie_control_management_caller_t *caller,
                                                const rpc_request_t *request,
                                                rpc_response_t *response) {
  static const char *const allowed[] = {"domain_id", "principal_id", "request_id"};
  json_value_t *params = NULL;
  flowie_control_credential_revoke_command_t command =
      FLOWIE_CONTROL_CREDENTIAL_REVOKE_COMMAND_INIT;
  flowie_control_command_result_t result = FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  const char *principal_id = NULL;
  const char *request_id = NULL;
  const char *domain_id = NULL;
  uint64_t occurred_at = 0u;
  int rc = flowie_control_rpc_params(request, allowed, 3u, &params);
  if (rc == SALTS_OK) rc = flowie_control_rpc_target_root(params, caller, &domain_id);
  if (rc == SALTS_OK)
    rc =
        flowie_control_rpc_string(params, "principal_id", FLOWIE_SECURITY_ID_MAX, 1, &principal_id);
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_string(params, "request_id", FLOWIE_CONTROL_REQUEST_ID_MAX, 1,
                                   &request_id);
  if (rc == SALTS_OK) {
    occurred_at = server->clock(server->clock_ctx);
    if (occurred_at == 0u) rc = SALTS_EIO;
  }
  if (rc == SALTS_OK) {
    command.domain_id = domain_id;
    command.principal_id = principal_id;
    command.actor = caller->actor;
    command.request_id = request_id;
    command.occurred_at = occurred_at;
    rc = flowie_control_management_credential_revoke(server->service, caller, &command, &result);
  }
  json_free(params);
  if (rc != SALTS_OK) return flowie_control_rpc_error(response, rc);
  return flowie_control_rpc_result(response, flowie_control_rpc_command_result(&result));
}

static int flowie_control_rpc_named_list(flowie_control_management_rpc_server_t *server,
                                         const flowie_control_management_caller_t *caller,
                                         const rpc_request_t *request, rpc_response_t *response,
                                         int groups) {
  static const char *const allowed[] = {"domain_id", "after", "limit"};
  flowie_control_management_caller_t scoped = FLOWIE_CONTROL_MANAGEMENT_CALLER_INIT;
  json_value_t *params = NULL;
  void *items = NULL;
  json_value_t *result = NULL;
  json_value_t *array = NULL;
  const char *after = NULL;
  size_t capacity = 0u;
  size_t count = 0u;
  int has_more = 0;
  int rc = flowie_control_rpc_params(request, allowed, 3u, &params);
  if (rc == SALTS_OK) rc = flowie_control_rpc_scope(server, params, caller, &scoped);
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_string(params, "after", FLOWIE_SECURITY_ID_MAX, 0, &after);
  if (rc == SALTS_OK) rc = flowie_control_rpc_page_limit(params, &capacity);
  if (rc == SALTS_OK) {
    items = calloc(capacity, groups ? sizeof(flowie_control_group_view_t)
                                    : sizeof(flowie_control_role_view_t));
    if (!items) rc = SALTS_ENOMEM;
  }
  for (size_t index = 0u; rc == SALTS_OK && index < capacity; ++index) {
    if (groups)
      ((flowie_control_group_view_t *)items)[index] =
          (flowie_control_group_view_t)FLOWIE_CONTROL_GROUP_VIEW_INIT;
    else
      ((flowie_control_role_view_t *)items)[index] =
          (flowie_control_role_view_t)FLOWIE_CONTROL_ROLE_VIEW_INIT;
  }
  if (rc == SALTS_OK && groups)
    rc = flowie_control_management_group_list(server->service, &scoped, after, items, capacity,
                                              &count, &has_more);
  else if (rc == SALTS_OK)
    rc = flowie_control_management_role_list(server->service, &scoped, after, items, capacity,
                                             &count, &has_more);
  if (rc == SALTS_OK) {
    result = json_create_object();
    array = json_create_array();
    if (!result || !array) rc = SALTS_ENOMEM;
  }
  for (size_t index = 0u; rc == SALTS_OK && index < count; ++index) {
    json_value_t *item =
        groups ? flowie_control_rpc_group(&((flowie_control_group_view_t *)items)[index])
               : flowie_control_rpc_role(&((flowie_control_role_view_t *)items)[index]);
    rc = flowie_control_rpc_array_add(array, item);
  }
  if (rc == SALTS_OK) {
    rc = flowie_control_rpc_add(result, "items", array);
    if (rc == SALTS_OK) array = NULL;
  }
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_add(result, "has_more", json_create_bool(has_more != 0));
  free(items);
  json_free(params);
  flowie_control_rpc_free_json_value(array);
  if (rc != SALTS_OK) {
    flowie_control_rpc_free_json_value(result);
    return flowie_control_rpc_error(response, rc);
  }
  return flowie_control_rpc_result(response, result);
}

static int flowie_control_rpc_group_write(flowie_control_management_rpc_server_t *server,
                                          const flowie_control_management_caller_t *caller,
                                          const rpc_request_t *request, rpc_response_t *response,
                                          int operation) {
  static const char *const create_allowed[] = {"domain_id", "group_id", "parent_group_id",
                                               "request_id"};
  static const char *const delete_allowed[] = {"domain_id", "group_id", "request_id"};
  static const char *const member_allowed[] = {"domain_id", "principal_id", "group_id",
                                               "request_id"};
  const char *const *allowed = operation == 0   ? create_allowed
                               : operation == 1 ? delete_allowed
                                                : member_allowed;
  size_t allowed_count = 4u;
  json_value_t *params = NULL;
  flowie_control_command_result_t result = FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  const char *group_id = NULL;
  const char *parent_group_id = NULL;
  const char *principal_id = NULL;
  const char *request_id = NULL;
  const char *domain_id = NULL;
  uint64_t occurred_at = 0u;
  int rc = flowie_control_rpc_params(request, allowed, allowed_count, &params);
  if (rc == SALTS_OK) rc = flowie_control_rpc_target_root(params, caller, &domain_id);
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_string(params, "group_id", FLOWIE_SECURITY_ID_MAX, 1, &group_id);
  if (rc == SALTS_OK && operation == 0)
    rc = flowie_control_rpc_string(params, "parent_group_id", FLOWIE_SECURITY_ID_MAX, 0,
                                   &parent_group_id);
  if (rc == SALTS_OK && operation >= 2)
    rc =
        flowie_control_rpc_string(params, "principal_id", FLOWIE_SECURITY_ID_MAX, 1, &principal_id);
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_string(params, "request_id", FLOWIE_CONTROL_REQUEST_ID_MAX, 1,
                                   &request_id);
  if (rc == SALTS_OK) occurred_at = server->clock(server->clock_ctx);
  if (rc == SALTS_OK && occurred_at == 0u) rc = SALTS_EIO;
  if (rc == SALTS_OK && operation == 0) {
    flowie_control_group_create_command_t command = FLOWIE_CONTROL_GROUP_CREATE_COMMAND_INIT;
    command.domain_id = domain_id;
    command.group_id = group_id;
    command.parent_group_id = parent_group_id;
    command.actor = caller->actor;
    command.request_id = request_id;
    command.occurred_at = occurred_at;
    rc = flowie_control_management_group_create(server->service, caller, &command, &result);
  } else if (rc == SALTS_OK && operation == 1) {
    flowie_control_group_delete_command_t command = FLOWIE_CONTROL_GROUP_DELETE_COMMAND_INIT;
    command.domain_id = domain_id;
    command.group_id = group_id;
    command.actor = caller->actor;
    command.request_id = request_id;
    command.occurred_at = occurred_at;
    rc = flowie_control_management_group_delete(server->service, caller, &command, &result);
  } else if (rc == SALTS_OK && operation == 2) {
    flowie_control_membership_add_command_t command = FLOWIE_CONTROL_MEMBERSHIP_ADD_COMMAND_INIT;
    command.domain_id = domain_id;
    command.principal_id = principal_id;
    command.group_id = group_id;
    command.actor = caller->actor;
    command.request_id = request_id;
    command.occurred_at = occurred_at;
    rc = flowie_control_management_membership_add(server->service, caller, &command, &result);
  } else if (rc == SALTS_OK) {
    flowie_control_membership_remove_command_t command =
        FLOWIE_CONTROL_MEMBERSHIP_REMOVE_COMMAND_INIT;
    command.domain_id = domain_id;
    command.principal_id = principal_id;
    command.group_id = group_id;
    command.actor = caller->actor;
    command.request_id = request_id;
    command.occurred_at = occurred_at;
    rc = flowie_control_management_membership_remove(server->service, caller, &command, &result);
  }
  json_free(params);
  return rc == SALTS_OK
             ? flowie_control_rpc_result(response, flowie_control_rpc_command_result(&result))
             : flowie_control_rpc_error(response, rc);
}

static int flowie_control_rpc_role_write(flowie_control_management_rpc_server_t *server,
                                         const flowie_control_management_caller_t *caller,
                                         const rpc_request_t *request, rpc_response_t *response,
                                         int operation) {
  static const char *const role_allowed[] = {"domain_id", "role_id", "request_id"};
  static const char *const assignment_allowed[] = {"domain_id", "principal_id", "role_id",
                                                   "request_id"};
  json_value_t *params = NULL;
  flowie_control_command_result_t result = FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  const char *role_id = NULL;
  const char *principal_id = NULL;
  const char *request_id = NULL;
  const char *domain_id = NULL;
  uint64_t occurred_at = 0u;
  int rc = flowie_control_rpc_params(request, operation < 2 ? role_allowed : assignment_allowed,
                                     operation < 2 ? 3u : 4u, &params);
  if (rc == SALTS_OK) rc = flowie_control_rpc_target_root(params, caller, &domain_id);
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_string(params, "role_id", FLOWIE_SECURITY_TYPE_MAX, 1, &role_id);
  if (rc == SALTS_OK && operation >= 2)
    rc =
        flowie_control_rpc_string(params, "principal_id", FLOWIE_SECURITY_ID_MAX, 1, &principal_id);
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_string(params, "request_id", FLOWIE_CONTROL_REQUEST_ID_MAX, 1,
                                   &request_id);
  if (rc == SALTS_OK) occurred_at = server->clock(server->clock_ctx);
  if (rc == SALTS_OK && occurred_at == 0u) rc = SALTS_EIO;
  if (rc == SALTS_OK && operation == 0) {
    flowie_control_role_create_command_t command = FLOWIE_CONTROL_ROLE_CREATE_COMMAND_INIT;
    command.domain_id = domain_id;
    command.role_id = role_id;
    command.actor = caller->actor;
    command.request_id = request_id;
    command.occurred_at = occurred_at;
    rc = flowie_control_management_role_create(server->service, caller, &command, &result);
  } else if (rc == SALTS_OK && operation == 1) {
    flowie_control_role_disable_command_t command = FLOWIE_CONTROL_ROLE_DISABLE_COMMAND_INIT;
    command.domain_id = domain_id;
    command.role_id = role_id;
    command.actor = caller->actor;
    command.request_id = request_id;
    command.occurred_at = occurred_at;
    rc = flowie_control_management_role_disable(server->service, caller, &command, &result);
  } else if (rc == SALTS_OK && operation == 2) {
    flowie_control_user_role_add_command_t command = FLOWIE_CONTROL_USER_ROLE_ADD_COMMAND_INIT;
    command.domain_id = domain_id;
    command.principal_id = principal_id;
    command.role_id = role_id;
    command.actor = caller->actor;
    command.request_id = request_id;
    command.occurred_at = occurred_at;
    rc = flowie_control_management_user_role_add(server->service, caller, &command, &result);
  } else if (rc == SALTS_OK) {
    flowie_control_user_role_remove_command_t command =
        FLOWIE_CONTROL_USER_ROLE_REMOVE_COMMAND_INIT;
    command.domain_id = domain_id;
    command.principal_id = principal_id;
    command.role_id = role_id;
    command.actor = caller->actor;
    command.request_id = request_id;
    command.occurred_at = occurred_at;
    rc = flowie_control_management_user_role_remove(server->service, caller, &command, &result);
  }
  json_free(params);
  return rc == SALTS_OK
             ? flowie_control_rpc_result(response, flowie_control_rpc_command_result(&result))
             : flowie_control_rpc_error(response, rc);
}

static int flowie_control_rpc_effective(flowie_control_management_rpc_server_t *server,
                                        const flowie_control_management_caller_t *caller,
                                        const rpc_request_t *request, rpc_response_t *response,
                                        int groups) {
  static const char *const allowed[] = {"domain_id", "principal_id"};
  flowie_control_management_caller_t scoped = FLOWIE_CONTROL_MANAGEMENT_CALLER_INIT;
  json_value_t *params = NULL;
  const char *principal_id = NULL;
  json_value_t *array = NULL;
  flowie_control_effective_groups_view_t group_view = FLOWIE_CONTROL_EFFECTIVE_GROUPS_VIEW_INIT;
  flowie_control_effective_roles_view_t role_view = FLOWIE_CONTROL_EFFECTIVE_ROLES_VIEW_INIT;
  int rc = flowie_control_rpc_params(request, allowed, 2u, &params);
  if (rc == SALTS_OK) rc = flowie_control_rpc_scope(server, params, caller, &scoped);
  if (rc == SALTS_OK)
    rc =
        flowie_control_rpc_string(params, "principal_id", FLOWIE_SECURITY_ID_MAX, 1, &principal_id);
  if (rc == SALTS_OK && groups)
    rc = flowie_control_management_effective_groups(server->service, &scoped, principal_id,
                                                    &group_view);
  else if (rc == SALTS_OK)
    rc = flowie_control_management_effective_roles(server->service, &scoped, principal_id,
                                                   &role_view);
  json_free(params);
  if (rc != SALTS_OK) return flowie_control_rpc_error(response, rc);
  array = json_create_array();
  if (!array) return flowie_control_rpc_error(response, SALTS_ENOMEM);
  for (uint32_t index = 0u;
       rc == SALTS_OK && index < (groups ? group_view.group_count : role_view.role_count);
       ++index) {
    const char *value = groups ? group_view.groups[index] : role_view.roles[index];
    rc = flowie_control_rpc_array_add(array, json_create_string(value));
  }
  if (rc != SALTS_OK) {
    flowie_control_rpc_free_json_value(array);
    return flowie_control_rpc_error(response, rc);
  }
  return flowie_control_rpc_result(response, array);
}

static int flowie_control_rpc_policy_status(flowie_control_management_rpc_server_t *server,
                                            const flowie_control_management_caller_t *caller,
                                            const rpc_request_t *request,
                                            rpc_response_t *response) {
  static const char *const allowed[] = {"domain_id"};
  flowie_control_policy_status_t status = FLOWIE_CONTROL_POLICY_STATUS_INIT;
  flowie_control_management_caller_t scoped = FLOWIE_CONTROL_MANAGEMENT_CALLER_INIT;
  json_value_t *params = NULL;
  json_value_t *object = NULL;
  int rc = flowie_control_rpc_params(request, allowed, 1u, &params);
  if (rc == SALTS_OK) rc = flowie_control_rpc_scope(server, params, caller, &scoped);
  if (rc == SALTS_OK)
    rc = flowie_control_management_policy_status(server->service, &scoped, &status);
  json_free(params);
  if (rc != SALTS_OK) return flowie_control_rpc_error(response, rc);
  object = json_create_object();
  if (!object ||
      flowie_control_rpc_add(object, "policy_version",
                             json_create_uint64(status.policy_version)) != SALTS_OK ||
      flowie_control_rpc_add(object, "expires_at", json_create_uint64(status.expires_at)) !=
          SALTS_OK ||
      flowie_control_rpc_add(object, "draft_rules",
                             json_create_uint64(status.draft_rule_count)) != SALTS_OK ||
      flowie_control_rpc_add(object, "published_rules",
                             json_create_uint64(status.published_rule_count)) != SALTS_OK) {
    flowie_control_rpc_free_json_value(object);
    return flowie_control_rpc_error(response, SALTS_ENOMEM);
  }
  return flowie_control_rpc_result(response, object);
}

static int flowie_control_rpc_subject_kind_parse(const char *text,
                                                 flowie_security_subject_kind_t *out) {
  if (!text || !out) return SALTS_EINVAL;
  if (strcmp(text, "user") == 0) *out = FLOWIE_SECURITY_SUBJECT_PRINCIPAL;
  else if (strcmp(text, "role") == 0) *out = FLOWIE_SECURITY_SUBJECT_ROLE;
  else if (strcmp(text, "group") == 0) *out = FLOWIE_SECURITY_SUBJECT_GROUP;
  else return SALTS_EPROTO;
  return SALTS_OK;
}

static const char *
flowie_control_rpc_subject_kind_name(flowie_security_subject_kind_t subject_kind) {
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

static const char *flowie_control_rpc_effect_name(flowie_security_effect_t effect) {
  return effect == FLOWIE_SECURITY_ALLOW ? "allow"
                                         : (effect == FLOWIE_SECURITY_DENY ? "deny" : NULL);
}

static const char *flowie_control_rpc_access_name(uint32_t action_mask) {
  if (action_mask == FLOWIE_SECURITY_ACTION_SUBSCRIBE) return "read";
  if (action_mask == FLOWIE_SECURITY_ACTION_PUBLISH) return "write";
  if (action_mask == (FLOWIE_SECURITY_ACTION_SUBSCRIBE | FLOWIE_SECURITY_ACTION_PUBLISH))
    return "readwrite";
  return NULL;
}

static const char *
flowie_control_rpc_policy_diagnostic_code(flowie_control_policy_diagnostic_code_t code) {
  switch (code) {
  case FLOWIE_CONTROL_POLICY_DIAGNOSTIC_INVALID_DOCUMENT:
    return "invalid_document";
  case FLOWIE_CONTROL_POLICY_DIAGNOSTIC_SUBJECT_NOT_FOUND:
    return "subject_not_found";
  case FLOWIE_CONTROL_POLICY_DIAGNOSTIC_SUBJECT_DISABLED:
    return "subject_disabled";
  case FLOWIE_CONTROL_POLICY_DIAGNOSTIC_ORDINAL_CONFLICT:
    return "ordinal_conflict";
  case FLOWIE_CONTROL_POLICY_DIAGNOSTIC_DUPLICATE_CHANGE:
    return "duplicate_change";
  case FLOWIE_CONTROL_POLICY_DIAGNOSTIC_DELETE_TARGET_NOT_FOUND:
    return "delete_target_not_found";
  case FLOWIE_CONTROL_POLICY_DIAGNOSTIC_RULE_LIMIT:
    return "rule_limit";
  case FLOWIE_CONTROL_POLICY_DIAGNOSTIC_EMPTY_POLICY:
    return "empty_policy";
  default:
    return "invalid_document";
  }
}

static const char *
flowie_control_rpc_policy_diagnostic_field(flowie_control_policy_diagnostic_field_t field) {
  switch (field) {
  case FLOWIE_CONTROL_POLICY_DIAGNOSTIC_FIELD_CHANGES:
    return "changes";
  case FLOWIE_CONTROL_POLICY_DIAGNOSTIC_FIELD_SUBJECT_ID:
    return "subject_id";
  case FLOWIE_CONTROL_POLICY_DIAGNOSTIC_FIELD_ORDINAL:
    return "ordinal";
  case FLOWIE_CONTROL_POLICY_DIAGNOSTIC_FIELD_ENTRIES:
    return "entries";
  default:
    return NULL;
  }
}

static const char *
flowie_control_rpc_policy_diagnostic_message(flowie_control_policy_diagnostic_code_t code) {
  switch (code) {
  case FLOWIE_CONTROL_POLICY_DIAGNOSTIC_SUBJECT_NOT_FOUND:
    return "Policy subject does not exist";
  case FLOWIE_CONTROL_POLICY_DIAGNOSTIC_SUBJECT_DISABLED:
    return "Policy subject is disabled";
  case FLOWIE_CONTROL_POLICY_DIAGNOSTIC_ORDINAL_CONFLICT:
    return "Policy ordinal conflicts with another subject";
  case FLOWIE_CONTROL_POLICY_DIAGNOSTIC_DUPLICATE_CHANGE:
    return "Policy subject appears more than once in changes";
  case FLOWIE_CONTROL_POLICY_DIAGNOSTIC_DELETE_TARGET_NOT_FOUND:
    return "Policy delete target does not exist";
  case FLOWIE_CONTROL_POLICY_DIAGNOSTIC_RULE_LIMIT:
    return "Policy expands beyond the rule limit";
  case FLOWIE_CONTROL_POLICY_DIAGNOSTIC_EMPTY_POLICY:
    return "Policy must contain at least one rule";
  default:
    return "Invalid policy document";
  }
}

static int flowie_control_rpc_subject_document(const json_value_t *params,
                                               flowie_control_acl_document_t *out) {
  static const char *const entry_allowed[] = {"effect", "access", "topic"};
  flowie_control_acl_document_t document = FLOWIE_CONTROL_ACL_DOCUMENT_INIT;
  json_value_t *entries;
  const char *subject_kind = NULL;
  const char *subject_id = NULL;
  const char *connection = NULL;
  int rc;
  if (!params || !out) return SALTS_EINVAL;
  rc = flowie_control_rpc_string(params, "subject_kind", 5u, 1, &subject_kind);
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_string(params, "subject_id", FLOWIE_SECURITY_ID_MAX, 1, &subject_id);
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_string(params, "connection", sizeof("allow") - 1u, 1, &connection);
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_subject_kind_parse(subject_kind, &document.subject_kind);
  if (rc == SALTS_OK) {
    size_t subject_size = strlen(subject_id);
    memcpy(document.subject, subject_id, subject_size + 1u);
    if (strcmp(connection, "allow") == 0) document.connection_effect = FLOWIE_SECURITY_ALLOW;
    else if (strcmp(connection, "deny") == 0) document.connection_effect = FLOWIE_SECURITY_DENY;
    else rc = SALTS_EPROTO;
  }
  entries = json_object_get(params, "entries");
  if (rc == SALTS_OK && (!entries || json_type(entries) != JSON_ARRAY ||
                         json_array_size(entries) > FLOWIE_CONTROL_ACL_MAX_ENTRIES))
    rc = SALTS_EPROTO;
  for (size_t index = 0u; rc == SALTS_OK && index < json_array_size(entries); ++index) {
    flowie_control_acl_entry_t *entry = &document.entries[index];
    json_value_t *item = json_array_get(entries, index);
    const char *effect = NULL;
    const char *access = NULL;
    const char *topic = NULL;
    if (!item || json_type(item) != JSON_OBJECT) {
      rc = SALTS_EPROTO;
      break;
    }
    for (size_t key_index = 0u; key_index < json_object_size(item); ++key_index) {
      const char *key = json_object_key(item, key_index);
      int known = 0;
      for (size_t allowed_index = 0u;
           allowed_index < sizeof(entry_allowed) / sizeof(entry_allowed[0]); ++allowed_index) {
        if (key && strcmp(key, entry_allowed[allowed_index]) == 0) {
          known = 1;
          break;
        }
      }
      if (!known) {
        rc = SALTS_EPROTO;
        break;
      }
    }
    if (rc == SALTS_OK)
      rc = flowie_control_rpc_string(item, "effect", sizeof("allow") - 1u, 1, &effect);
    if (rc == SALTS_OK)
      rc = flowie_control_rpc_string(item, "access", sizeof("readwrite") - 1u, 1, &access);
    if (rc == SALTS_OK)
      rc = flowie_control_rpc_string(item, "topic", FLOWIE_SECURITY_PATTERN_MAX, 1, &topic);
    if (rc == SALTS_OK) {
      if (strcmp(effect, "allow") == 0) entry->effect = FLOWIE_SECURITY_ALLOW;
      else if (strcmp(effect, "deny") == 0) entry->effect = FLOWIE_SECURITY_DENY;
      else rc = SALTS_EPROTO;
    }
    if (rc == SALTS_OK) {
      if (strcmp(access, "read") == 0) entry->action_mask = FLOWIE_SECURITY_ACTION_SUBSCRIBE;
      else if (strcmp(access, "write") == 0) entry->action_mask = FLOWIE_SECURITY_ACTION_PUBLISH;
      else if (strcmp(access, "readwrite") == 0)
        entry->action_mask = FLOWIE_SECURITY_ACTION_SUBSCRIBE | FLOWIE_SECURITY_ACTION_PUBLISH;
      else rc = SALTS_EPROTO;
    }
    if (rc == SALTS_OK) {
      memcpy(entry->topic, topic, strlen(topic) + 1u);
      entry->alternative_count = 1u;
      ++document.entry_count;
    }
  }
  if (rc == SALTS_OK && document.connection_effect == FLOWIE_SECURITY_DENY &&
      document.entry_count != 0u)
    rc = SALTS_EPROTO;
  if (rc == SALTS_OK) *out = document;
  return rc;
}

static json_value_t *
flowie_control_rpc_subject_rule_json(const flowie_control_policy_subject_rule_view_t *view) {
  json_value_t *object = NULL;
  json_value_t *entries = NULL;
  const char *subject_kind;
  const char *connection;
  int rc = SALTS_OK;
  if (!view || view->size < sizeof(*view) ||
      !(subject_kind = flowie_control_rpc_subject_kind_name(view->document.subject_kind)) ||
      !(connection = flowie_control_rpc_effect_name(view->document.connection_effect)))
    return NULL;
  object = json_create_object();
  entries = json_create_array();
  if (!object || !entries) rc = SALTS_ENOMEM;
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_add(object, "subject_kind", json_create_string(subject_kind));
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_add(object, "subject_id",
                                json_create_string(view->document.subject));
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_add(object, "ordinal", json_create_uint64(view->ordinal));
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_add(object, "connection", json_create_string(connection));
  for (size_t index = 0u; rc == SALTS_OK && index < view->document.entry_count; ++index) {
    const flowie_control_acl_entry_t *entry = &view->document.entries[index];
    const char *effect = flowie_control_rpc_effect_name(entry->effect);
    const char *access = flowie_control_rpc_access_name(entry->action_mask);
    json_value_t *item = json_create_object();
    if (!effect || !access || !item ||
        flowie_control_rpc_add(item, "effect", json_create_string(effect)) != SALTS_OK ||
        flowie_control_rpc_add(item, "access", json_create_string(access)) != SALTS_OK ||
        flowie_control_rpc_add(item, "topic", json_create_string(entry->topic)) != SALTS_OK) {
      flowie_control_rpc_free_json_value(item);
      rc = SALTS_ENOMEM;
    } else {
      rc = flowie_control_rpc_array_add(entries, item);
    }
  }
  if (rc == SALTS_OK) {
    rc = flowie_control_rpc_add(object, "entries", entries);
    if (rc == SALTS_OK) entries = NULL;
  }
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_add(object, "revision", json_create_uint64(view->revision));
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_add(object, "updated_at", json_create_uint64(view->updated_at));
  flowie_control_rpc_free_json_value(entries);
  if (rc != SALTS_OK) {
    flowie_control_rpc_free_json_value(object);
    return NULL;
  }
  return object;
}

static int
flowie_control_rpc_policy_subject_rule_get(flowie_control_management_rpc_server_t *server,
                                           const flowie_control_management_caller_t *caller,
                                           const rpc_request_t *request, rpc_response_t *response) {
  static const char *const allowed[] = {"domain_id", "subject_kind", "subject_id"};
  flowie_control_management_caller_t scoped = FLOWIE_CONTROL_MANAGEMENT_CALLER_INIT;
  flowie_control_policy_subject_rule_view_t view = FLOWIE_CONTROL_POLICY_SUBJECT_RULE_VIEW_INIT;
  json_value_t *params = NULL;
  const char *kind_text = NULL;
  const char *subject_id = NULL;
  flowie_security_subject_kind_t subject_kind = FLOWIE_SECURITY_SUBJECT_ANY;
  json_value_t *object;
  int rc = flowie_control_rpc_params(request, allowed, 3u, &params);
  if (rc == SALTS_OK) rc = flowie_control_rpc_scope(server, params, caller, &scoped);
  if (rc == SALTS_OK) rc = flowie_control_rpc_string(params, "subject_kind", 5u, 1, &kind_text);
  if (rc == SALTS_OK) rc = flowie_control_rpc_subject_kind_parse(kind_text, &subject_kind);
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_string(params, "subject_id", FLOWIE_SECURITY_ID_MAX, 1, &subject_id);
  if (rc == SALTS_OK)
    rc = flowie_control_management_policy_subject_rule_get(server->service, &scoped, subject_kind,
                                                           subject_id, &view);
  json_free(params);
  if (rc != SALTS_OK) return flowie_control_rpc_error(response, rc);
  object = flowie_control_rpc_subject_rule_json(&view);
  return object ? flowie_control_rpc_result(response, object)
                : flowie_control_rpc_error(response, SALTS_ENOMEM);
}

static int
flowie_control_rpc_policy_subject_rule_list(flowie_control_management_rpc_server_t *server,
                                            const flowie_control_management_caller_t *caller,
                                            const rpc_request_t *request,
                                            rpc_response_t *response) {
  static const char *const allowed[] = {"domain_id", "subject_kind", "after_ordinal", "limit"};
  flowie_control_management_caller_t scoped = FLOWIE_CONTROL_MANAGEMENT_CALLER_INIT;
  flowie_control_policy_subject_rule_view_t *items = NULL;
  json_value_t *params = NULL;
  json_value_t *result = NULL;
  json_value_t *array = NULL;
  const char *kind_text = NULL;
  flowie_security_subject_kind_t subject_kind = FLOWIE_SECURITY_SUBJECT_ANY;
  uint64_t after = 0u;
  size_t capacity = 0u;
  size_t count = 0u;
  int has_after = 0;
  int has_more = 0;
  int rc = flowie_control_rpc_params(request, allowed, 4u, &params);
  if (rc == SALTS_OK) rc = flowie_control_rpc_scope(server, params, caller, &scoped);
  if (rc == SALTS_OK && json_object_get(params, "subject_kind")) {
    rc = flowie_control_rpc_string(params, "subject_kind", 5u, 1, &kind_text);
    if (rc == SALTS_OK) rc = flowie_control_rpc_subject_kind_parse(kind_text, &subject_kind);
  }
  if (rc == SALTS_OK && json_object_get(params, "after_ordinal")) {
    has_after = 1;
    rc = flowie_control_rpc_u64(params, "after_ordinal", 1, &after);
    if (rc == SALTS_OK && after >= FLOWIE_SECURITY_MAX_RULES) rc = SALTS_ERANGE;
  }
  if (rc == SALTS_OK) rc = flowie_control_rpc_page_limit(params, &capacity);
  if (rc == SALTS_OK) {
    items = (flowie_control_policy_subject_rule_view_t *)calloc(capacity, sizeof(*items));
    if (!items) rc = SALTS_ENOMEM;
  }
  for (size_t index = 0u; rc == SALTS_OK && index < capacity; ++index)
    items[index] =
        (flowie_control_policy_subject_rule_view_t)FLOWIE_CONTROL_POLICY_SUBJECT_RULE_VIEW_INIT;
  if (rc == SALTS_OK)
    rc = flowie_control_management_policy_subject_rule_list(server->service, &scoped, subject_kind,
                                                            (uint32_t)after, has_after, items,
                                                            capacity, &count, &has_more);
  if (rc == SALTS_OK) {
    result = json_create_object();
    array = json_create_array();
    if (!result || !array) rc = SALTS_ENOMEM;
  }
  for (size_t index = 0u; rc == SALTS_OK && index < count; ++index) {
    json_value_t *item = flowie_control_rpc_subject_rule_json(&items[index]);
    rc = item ? flowie_control_rpc_array_add(array, item) : SALTS_ENOMEM;
  }
  if (rc == SALTS_OK) {
    rc = flowie_control_rpc_add(result, "items", array);
    if (rc == SALTS_OK) array = NULL;
  }
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_add(result, "has_more", json_create_bool(has_more != 0));
  free(items);
  json_free(params);
  flowie_control_rpc_free_json_value(array);
  if (rc != SALTS_OK) {
    flowie_control_rpc_free_json_value(result);
    return flowie_control_rpc_error(response, rc);
  }
  return flowie_control_rpc_result(response, result);
}

static int
flowie_control_rpc_policy_subject_rule_write(flowie_control_management_rpc_server_t *server,
                                             const flowie_control_management_caller_t *caller,
                                             const rpc_request_t *request, rpc_response_t *response,
                                             int remove) {
  static const char *const put_allowed[] = {"domain_id",  "subject_kind", "subject_id", "ordinal",
                                            "connection", "entries",      "request_id"};
  static const char *const delete_allowed[] = {"domain_id", "subject_kind", "subject_id",
                                               "request_id"};
  flowie_control_command_result_t result = FLOWIE_CONTROL_COMMAND_RESULT_INIT;
  flowie_control_acl_document_t *document = NULL;
  json_value_t *params = NULL;
  const char *domain_id = NULL;
  const char *request_id = NULL;
  const char *kind_text = NULL;
  const char *subject_id = NULL;
  flowie_security_subject_kind_t subject_kind = FLOWIE_SECURITY_SUBJECT_ANY;
  uint64_t ordinal = 0u;
  uint64_t occurred_at = 0u;
  int rc = flowie_control_rpc_params(request, remove ? delete_allowed : put_allowed,
                                     remove ? 4u : 7u, &params);
  if (rc == SALTS_OK) rc = flowie_control_rpc_target_root(params, caller, &domain_id);
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_string(params, "request_id", FLOWIE_CONTROL_REQUEST_ID_MAX, 1,
                                   &request_id);
  if (rc == SALTS_OK && remove)
    rc = flowie_control_rpc_string(params, "subject_kind", 5u, 1, &kind_text);
  if (rc == SALTS_OK && remove)
    rc = flowie_control_rpc_subject_kind_parse(kind_text, &subject_kind);
  if (rc == SALTS_OK && remove)
    rc = flowie_control_rpc_string(params, "subject_id", FLOWIE_SECURITY_ID_MAX, 1, &subject_id);
  if (rc == SALTS_OK && !remove) {
    document = (flowie_control_acl_document_t *)malloc(sizeof(*document));
    if (!document) rc = SALTS_ENOMEM;
    else {
      flowie_control_acl_document_init(document);
      rc = flowie_control_rpc_subject_document(params, document);
    }
  }
  if (rc == SALTS_OK && !remove) {
    rc = flowie_control_rpc_u64(params, "ordinal", 1, &ordinal);
    if (rc == SALTS_OK && ordinal >= FLOWIE_SECURITY_MAX_RULES) rc = SALTS_ERANGE;
  }
  if (rc == SALTS_OK) occurred_at = server->clock(server->clock_ctx);
  if (rc == SALTS_OK && occurred_at == 0u) rc = SALTS_EIO;
  if (rc == SALTS_OK && remove) {
    flowie_control_policy_subject_rule_delete_command_t command =
        FLOWIE_CONTROL_POLICY_SUBJECT_RULE_DELETE_COMMAND_INIT;
    command.domain_id = domain_id;
    command.subject_kind = subject_kind;
    command.subject_id = subject_id;
    command.actor = caller->actor;
    command.request_id = request_id;
    command.occurred_at = occurred_at;
    rc = flowie_control_management_policy_subject_rule_delete(server->service, caller, &command,
                                                              &result);
  } else if (rc == SALTS_OK) {
    flowie_control_policy_subject_rule_put_command_t command =
        FLOWIE_CONTROL_POLICY_SUBJECT_RULE_PUT_COMMAND_INIT;
    command.domain_id = domain_id;
    command.ordinal = (uint32_t)ordinal;
    command.document = document;
    command.actor = caller->actor;
    command.request_id = request_id;
    command.occurred_at = occurred_at;
    rc = flowie_control_management_policy_subject_rule_put(server->service, caller, &command,
                                                           &result);
  }
  json_free(params);
  free(document);
  return rc == SALTS_OK
             ? flowie_control_rpc_result(response, flowie_control_rpc_command_result(&result))
             : flowie_control_rpc_error(response, rc);
}

static int flowie_control_rpc_policy_validate(flowie_control_management_rpc_server_t *server,
                                              const flowie_control_management_caller_t *caller,
                                              const rpc_request_t *request,
                                              rpc_response_t *response) {
  static const char *const allowed[] = {"domain_id"};
  flowie_control_policy_validation_t validation = FLOWIE_CONTROL_POLICY_VALIDATION_INIT;
  flowie_control_management_caller_t scoped = FLOWIE_CONTROL_MANAGEMENT_CALLER_INIT;
  json_value_t *params = NULL;
  json_value_t *object = NULL;
  int rc = flowie_control_rpc_params(request, allowed, 1u, &params);
  if (rc == SALTS_OK) rc = flowie_control_rpc_scope(server, params, caller, &scoped);
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_policy_execute(server, FLOWIE_CONTROL_RPC_POLICY_VALIDATE, &scoped,
                                           NULL, NULL, 0u, NULL, &validation, NULL);
  json_free(params);
  if (rc != SALTS_OK) return flowie_control_rpc_error(response, rc);
  object = json_create_object();
  if (!object ||
      flowie_control_rpc_add(object, "rule_count",
                             json_create_uint64(validation.rule_count)) != SALTS_OK ||
      flowie_control_rpc_add(object, "deny_rule_count",
                             json_create_uint64(validation.deny_rule_count)) != SALTS_OK) {
    flowie_control_rpc_free_json_value(object);
    return flowie_control_rpc_error(response, SALTS_ENOMEM);
  }
  return flowie_control_rpc_result(response, object);
}

static int flowie_control_rpc_policy_dry_run(flowie_control_management_rpc_server_t *server,
                                             const flowie_control_management_caller_t *caller,
                                             const rpc_request_t *request,
                                             rpc_response_t *response) {
  static const char *const allowed[] = {"domain_id", "changes"};
  static const char *const put_allowed[] = {"operation", "subject_kind", "subject_id",
                                            "ordinal",   "connection",   "entries"};
  static const char *const delete_allowed[] = {"operation", "subject_kind", "subject_id"};
  flowie_control_management_caller_t scoped = FLOWIE_CONTROL_MANAGEMENT_CALLER_INIT;
  flowie_control_policy_dry_run_change_t *changes = NULL;
  flowie_control_policy_diagnostic_t *diagnostics = NULL;
  flowie_control_policy_dry_run_result_t result = FLOWIE_CONTROL_POLICY_DRY_RUN_RESULT_INIT;
  json_value_t *params = NULL;
  json_value_t *change_values = NULL;
  json_value_t *object = NULL;
  json_value_t *diagnostic_values = NULL;
  size_t change_count = 0u;
  int rc = flowie_control_rpc_params(request, allowed, 2u, &params);

  if (rc == SALTS_OK) rc = flowie_control_rpc_scope(server, params, caller, &scoped);
  if (rc == SALTS_OK) {
    change_values = json_object_get(params, "changes");
    if (!change_values || json_type(change_values) != JSON_ARRAY) rc = SALTS_EPROTO;
    else {
      change_count = json_array_size(change_values);
      if (change_count == 0u || change_count > FLOWIE_CONTROL_POLICY_DRY_RUN_MAX_CHANGES)
        rc = SALTS_EPROTO;
    }
  }
  if (rc == SALTS_OK) {
    changes = (flowie_control_policy_dry_run_change_t *)calloc(change_count, sizeof(*changes));
    diagnostics = (flowie_control_policy_diagnostic_t *)calloc(
        FLOWIE_CONTROL_POLICY_DRY_RUN_MAX_CHANGES, sizeof(*diagnostics));
    if (!changes || !diagnostics) rc = SALTS_ENOMEM;
  }
  for (size_t index = 0u; rc == SALTS_OK && index < change_count; ++index) {
    json_value_t *item = json_array_get(change_values, index);
    flowie_control_policy_dry_run_change_t *change = &changes[index];
    const char *operation = NULL;
    const char *kind_text = NULL;
    const char *subject_id = NULL;
    uint64_t ordinal = 0u;
    *change = (flowie_control_policy_dry_run_change_t)FLOWIE_CONTROL_POLICY_DRY_RUN_CHANGE_INIT;
    if (!item || json_type(item) != JSON_OBJECT) {
      rc = SALTS_EPROTO;
      break;
    }
    rc = flowie_control_rpc_string(item, "operation", sizeof("delete") - 1u, 1, &operation);
    if (rc == SALTS_OK && strcmp(operation, "put") == 0) {
      flowie_control_acl_document_t document = FLOWIE_CONTROL_ACL_DOCUMENT_INIT;
      char *text = NULL;
      size_t text_size = 0u;
      rc = flowie_control_rpc_object_allowed(item, put_allowed,
                                             sizeof(put_allowed) / sizeof(put_allowed[0]));
      if (rc == SALTS_OK) rc = flowie_control_rpc_subject_document(item, &document);
      if (rc == SALTS_OK) rc = flowie_control_rpc_u64(item, "ordinal", 1, &ordinal);
      if (rc == SALTS_OK && ordinal >= FLOWIE_SECURITY_MAX_RULES) rc = SALTS_EPROTO;
      if (rc == SALTS_OK) {
        text = (char *)malloc(FLOWIE_CONTROL_ACL_DOCUMENT_MAX + 1u);
        if (!text) rc = SALTS_ENOMEM;
        else
          rc = flowie_control_acl_format(&document, text, FLOWIE_CONTROL_ACL_DOCUMENT_MAX + 1u,
                                         &text_size);
      }
      if (rc != SALTS_OK) {
        free(text);
        break;
      }
      change->operation = FLOWIE_CONTROL_POLICY_DRY_RUN_PUT;
      change->ordinal = (uint32_t)ordinal;
      change->subject_kind = document.subject_kind;
      change->subject_id = json_string(json_object_get(item, "subject_id"));
      change->document = text;
      change->document_size = text_size;
    } else if (rc == SALTS_OK && strcmp(operation, "delete") == 0) {
      rc = flowie_control_rpc_object_allowed(item, delete_allowed,
                                             sizeof(delete_allowed) / sizeof(delete_allowed[0]));
      if (rc == SALTS_OK) rc = flowie_control_rpc_string(item, "subject_kind", 5u, 1, &kind_text);
      if (rc == SALTS_OK)
        rc = flowie_control_rpc_subject_kind_parse(kind_text, &change->subject_kind);
      if (rc == SALTS_OK)
        rc = flowie_control_rpc_string(item, "subject_id", FLOWIE_SECURITY_ID_MAX, 1, &subject_id);
      if (rc == SALTS_OK) {
        change->operation = FLOWIE_CONTROL_POLICY_DRY_RUN_DELETE;
        change->subject_id = subject_id;
      }
    } else if (rc == SALTS_OK) {
      rc = SALTS_EPROTO;
    }
  }
  if (rc == SALTS_OK) {
    result.diagnostics = diagnostics;
    result.diagnostic_capacity = FLOWIE_CONTROL_POLICY_DRY_RUN_MAX_CHANGES;
    rc = flowie_control_rpc_policy_execute(server, FLOWIE_CONTROL_RPC_POLICY_DRY_RUN, &scoped, NULL,
                                           changes, change_count, &result, NULL, NULL);
  }
  for (size_t index = 0u; changes && index < change_count; ++index)
    free((void *)changes[index].document);
  free(changes);
  json_free(params);
  if (rc != SALTS_OK) {
    free(diagnostics);
    return flowie_control_rpc_error(response, rc);
  }

  object = json_create_object();
  diagnostic_values = json_create_array();
  if (!object || !diagnostic_values ||
      flowie_control_rpc_add(object, "valid", json_create_bool(result.valid != 0)) !=
          SALTS_OK ||
      flowie_control_rpc_add(object, "store_revision",
                             json_create_uint64(result.store_revision)) != SALTS_OK ||
      flowie_control_rpc_add(object, "rule_count", json_create_uint64(result.rule_count)) !=
          SALTS_OK ||
      flowie_control_rpc_add(object, "deny_rule_count",
                             json_create_uint64(result.deny_rule_count)) != SALTS_OK)
    rc = SALTS_ENOMEM;
  for (size_t index = 0u; rc == SALTS_OK && index < result.diagnostic_count; ++index) {
    const flowie_control_policy_diagnostic_t *diagnostic = &diagnostics[index];
    const char *subject_kind = flowie_control_rpc_subject_kind_name(diagnostic->subject_kind);
    const char *field = flowie_control_rpc_policy_diagnostic_field(diagnostic->field);
    json_value_t *item = json_create_object();
    if (!item ||
        flowie_control_rpc_add(item, "code",
                               json_create_string(flowie_control_rpc_policy_diagnostic_code(
                                   diagnostic->code))) != SALTS_OK ||
        flowie_control_rpc_add(
            item, "message",
            json_create_string(
                flowie_control_rpc_policy_diagnostic_message(diagnostic->code))) != SALTS_OK)
      rc = SALTS_ENOMEM;
    if (rc == SALTS_OK && diagnostic->has_change_index)
      rc = flowie_control_rpc_add(item, "change_index",
                                  json_create_uint64(diagnostic->change_index));
    if (rc == SALTS_OK && subject_kind)
      rc = flowie_control_rpc_add(item, "subject_kind", json_create_string(subject_kind));
    if (rc == SALTS_OK && diagnostic->subject_id[0])
      rc = flowie_control_rpc_add(item, "subject_id",
                                  json_create_string(diagnostic->subject_id));
    if (rc == SALTS_OK && field)
      rc = flowie_control_rpc_add(item, "field", json_create_string(field));
    if (rc == SALTS_OK) rc = flowie_control_rpc_array_add(diagnostic_values, item);
    else flowie_control_rpc_free_json_value(item);
  }
  if (rc == SALTS_OK) {
    rc = flowie_control_rpc_add(object, "diagnostics", diagnostic_values);
    if (rc == SALTS_OK) diagnostic_values = NULL;
  }
  free(diagnostics);
  if (rc != SALTS_OK) {
    flowie_control_rpc_free_json_value(diagnostic_values);
    flowie_control_rpc_free_json_value(object);
    return flowie_control_rpc_error(response, rc);
  }
  return flowie_control_rpc_result(response, object);
}

static int flowie_control_rpc_policy_publish(flowie_control_management_rpc_server_t *server,
                                             const flowie_control_management_caller_t *caller,
                                             const rpc_request_t *request,
                                             rpc_response_t *response) {
  static const char *const allowed[] = {"domain_id", "request_id", "expires_at"};
  json_value_t *params = NULL;
  flowie_control_policy_publish_result_t result = FLOWIE_CONTROL_POLICY_PUBLISH_RESULT_INIT;
  const char *request_id = NULL;
  const char *domain_id = NULL;
  uint64_t expires_at = 0u;
  uint64_t occurred_at = 0u;
  int rc = flowie_control_rpc_params(request, allowed, 3u, &params);
  if (rc == SALTS_OK) rc = flowie_control_rpc_target_root(params, caller, &domain_id);
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_string(params, "request_id", FLOWIE_CONTROL_REQUEST_ID_MAX, 1,
                                   &request_id);
  if (rc == SALTS_OK) rc = flowie_control_rpc_u64(params, "expires_at", 0, &expires_at);
  occurred_at = server->clock(server->clock_ctx);
  if (rc == SALTS_OK && occurred_at == 0u) rc = SALTS_EIO;
  if (rc == SALTS_OK) {
    flowie_control_policy_publish_command_t command = FLOWIE_CONTROL_POLICY_PUBLISH_COMMAND_INIT;
    command.domain_id = domain_id;
    command.actor = caller->actor;
    command.request_id = request_id;
    command.occurred_at = occurred_at;
    command.expires_at = expires_at;
    rc = flowie_control_rpc_policy_execute(server, FLOWIE_CONTROL_RPC_POLICY_PUBLISH, caller,
                                           &command, NULL, 0u, NULL, NULL, &result);
  }
  json_free(params);
  if (rc != SALTS_OK) return flowie_control_rpc_error(response, rc);
  {
    json_value_t *object = json_create_object();
    if (!object ||
        flowie_control_rpc_add(object, "policy_version",
                               json_create_uint64(result.policy_version)) != SALTS_OK ||
        flowie_control_rpc_add(object, "replayed", json_create_bool(result.replayed != 0)) !=
            SALTS_OK) {
      flowie_control_rpc_free_json_value(object);
      return flowie_control_rpc_error(response, SALTS_ENOMEM);
    }
    return flowie_control_rpc_result(response, object);
  }
}

static int flowie_control_rpc_audit_list(flowie_control_management_rpc_server_t *server,
                                         const flowie_control_management_caller_t *caller,
                                         const rpc_request_t *request, rpc_response_t *response) {
  static const char *const allowed[] = {"domain_id", "after", "limit"};
  flowie_control_management_caller_t scoped = FLOWIE_CONTROL_MANAGEMENT_CALLER_INIT;
  json_value_t *params = NULL;
  flowie_control_audit_view_t *items = NULL;
  json_value_t *result = NULL;
  json_value_t *array = NULL;
  uint64_t after = 0u;
  size_t capacity = 0u;
  size_t count = 0u;
  int has_more = 0;
  int rc = flowie_control_rpc_params(request, allowed, 3u, &params);
  if (rc == SALTS_OK) rc = flowie_control_rpc_scope(server, params, caller, &scoped);
  if (rc == SALTS_OK) rc = flowie_control_rpc_u64(params, "after", 0, &after);
  if (rc == SALTS_OK) rc = flowie_control_rpc_page_limit(params, &capacity);
  if (rc == SALTS_OK) {
    items = (flowie_control_audit_view_t *)calloc(capacity, sizeof(*items));
    if (!items) rc = SALTS_ENOMEM;
  }
  for (size_t index = 0u; rc == SALTS_OK && index < capacity; ++index)
    items[index] = (flowie_control_audit_view_t)FLOWIE_CONTROL_AUDIT_VIEW_INIT;
  if (rc == SALTS_OK)
    rc = flowie_control_management_audit_list(server->service, &scoped, after, items, capacity,
                                              &count, &has_more);
  if (rc == SALTS_OK) {
    result = json_create_object();
    array = json_create_array();
    if (!result || !array) rc = SALTS_ENOMEM;
  }
  for (size_t index = 0u; rc == SALTS_OK && index < count; ++index) {
    json_value_t *item = json_create_object();
    if (!item ||
        flowie_control_rpc_add(item, "request_id",
                               json_create_string(items[index].request_id)) != SALTS_OK ||
        flowie_control_rpc_add(item, "actor", json_create_string(items[index].actor)) !=
            SALTS_OK ||
        flowie_control_rpc_add(item, "operation",
                               json_create_string(items[index].operation)) != SALTS_OK ||
        flowie_control_rpc_add(item, "target", json_create_string(items[index].target_id)) !=
            SALTS_OK ||
        flowie_control_rpc_add(item, "cursor", json_create_uint64(items[index].revision)) !=
            SALTS_OK ||
        flowie_control_rpc_add(item, "occurred_at",
                               json_create_uint64(items[index].occurred_at)) != SALTS_OK) {
      flowie_control_rpc_free_json_value(item);
      rc = SALTS_ENOMEM;
    } else {
      rc = flowie_control_rpc_array_add(array, item);
    }
  }
  if (rc == SALTS_OK) {
    rc = flowie_control_rpc_add(result, "items", array);
    if (rc == SALTS_OK) array = NULL;
  }
  if (rc == SALTS_OK)
    rc = flowie_control_rpc_add(result, "has_more", json_create_bool(has_more != 0));
  free(items);
  json_free(params);
  flowie_control_rpc_free_json_value(array);
  if (rc != SALTS_OK) {
    flowie_control_rpc_free_json_value(result);
    return flowie_control_rpc_error(response, rc);
  }
  return flowie_control_rpc_result(response, result);
}

static int flowie_control_rpc_dispatch(flowie_control_management_rpc_server_t *server,
                                       const flowie_control_management_caller_t *caller,
                                       const rpc_request_t *request, rpc_response_t *response) {
  const char *method = request->method;
  if (strcmp(method, "control.system.status") == 0)
    return flowie_control_rpc_system_status(server, caller, request, response);
  if (strcmp(method, "control.auth.external_https.stats") == 0)
    return flowie_control_rpc_external_https_stats(server, caller, request, response);
  if (strcmp(method, "control.domain.create") == 0)
    return flowie_control_rpc_domain_create(server, caller, request, response);
  if (strcmp(method, "control.domain.admin.initialize") == 0)
    return flowie_control_rpc_domain_admin_initialize(server, caller, request, response);
  if (strcmp(method, "control.domain.list") == 0)
    return flowie_control_rpc_domain_list(server, caller, request, response);
  if (strcmp(method, "control.user.get") == 0)
    return flowie_control_rpc_user_get(server, caller, request, response);
  if (strcmp(method, "control.user.list") == 0)
    return flowie_control_rpc_user_list(server, caller, request, response);
  if (strcmp(method, "control.user.create") == 0)
    return flowie_control_rpc_user_write(server, caller, request, response, 0);
  if (strcmp(method, "control.user.disable") == 0)
    return flowie_control_rpc_user_write(server, caller, request, response, 1);
  if (strcmp(method, "control.credential.generate") == 0)
    return flowie_control_rpc_credential_issue(server, caller, request, response, 0);
  if (strcmp(method, "control.credential.rotate") == 0)
    return flowie_control_rpc_credential_issue(server, caller, request, response, 1);
  if (strcmp(method, "control.credential.revoke") == 0)
    return flowie_control_rpc_credential_revoke(server, caller, request, response);
  if (strcmp(method, "control.password.set") == 0)
    return flowie_control_rpc_password_set(server, caller, request, response);
  if (strcmp(method, "control.password.change") == 0)
    return flowie_control_rpc_password_change(server, caller, request, response);
  if (strcmp(method, "control.group.list") == 0)
    return flowie_control_rpc_named_list(server, caller, request, response, 1);
  if (strcmp(method, "control.group.create") == 0)
    return flowie_control_rpc_group_write(server, caller, request, response, 0);
  if (strcmp(method, "control.group.delete") == 0)
    return flowie_control_rpc_group_write(server, caller, request, response, 1);
  if (strcmp(method, "control.group.member.add") == 0)
    return flowie_control_rpc_group_write(server, caller, request, response, 2);
  if (strcmp(method, "control.group.member.remove") == 0)
    return flowie_control_rpc_group_write(server, caller, request, response, 3);
  if (strcmp(method, "control.group.effective") == 0)
    return flowie_control_rpc_effective(server, caller, request, response, 1);
  if (strcmp(method, "control.role.list") == 0)
    return flowie_control_rpc_named_list(server, caller, request, response, 0);
  if (strcmp(method, "control.role.create") == 0)
    return flowie_control_rpc_role_write(server, caller, request, response, 0);
  if (strcmp(method, "control.role.disable") == 0)
    return flowie_control_rpc_role_write(server, caller, request, response, 1);
  if (strcmp(method, "control.role.assign") == 0)
    return flowie_control_rpc_role_write(server, caller, request, response, 2);
  if (strcmp(method, "control.role.remove") == 0)
    return flowie_control_rpc_role_write(server, caller, request, response, 3);
  if (strcmp(method, "control.role.effective") == 0)
    return flowie_control_rpc_effective(server, caller, request, response, 0);
  if (strcmp(method, "control.policy.status") == 0)
    return flowie_control_rpc_policy_status(server, caller, request, response);
  if (strcmp(method, "control.policy.subject_rule.get") == 0)
    return flowie_control_rpc_policy_subject_rule_get(server, caller, request, response);
  if (strcmp(method, "control.policy.subject_rule.list") == 0)
    return flowie_control_rpc_policy_subject_rule_list(server, caller, request, response);
  if (strcmp(method, "control.policy.subject_rule.put") == 0)
    return flowie_control_rpc_policy_subject_rule_write(server, caller, request, response, 0);
  if (strcmp(method, "control.policy.subject_rule.delete") == 0)
    return flowie_control_rpc_policy_subject_rule_write(server, caller, request, response, 1);
  if (strcmp(method, "control.policy.validate") == 0)
    return flowie_control_rpc_policy_validate(server, caller, request, response);
  if (strcmp(method, "control.policy.dry_run") == 0)
    return flowie_control_rpc_policy_dry_run(server, caller, request, response);
  if (strcmp(method, "control.policy.publish") == 0)
    return flowie_control_rpc_policy_publish(server, caller, request, response);
  if (strcmp(method, "control.audit.list") == 0)
    return flowie_control_rpc_audit_list(server, caller, request, response);
  rpc_set_error(response, RPC_ERROR_METHOD_NOT_FOUND, "Method not found");
  return SALTS_ENOENT;
}

static void flowie_control_rpc_method(Req *request, Res *response) {
  flowie_control_management_rpc_server_t *server;
  rpc_response_t rpc_response;
  if (!request || !response || !request->app || !request->path) return;
  set_header(response, "Cache-Control", "no-store");
  set_header(response, "Pragma", "no-cache");
  set_header(response, "X-Content-Type-Options", "nosniff");
  server = (flowie_control_management_rpc_server_t *)flowie_control_http_app_lookup_context(
      request->app, request->path);
  if (!server) {
    memset(&rpc_response, 0, sizeof(rpc_response));
    rpc_response.arena = request->arena;
    rpc_response.jsonrpc = "2.0";
    rpc_set_error(&rpc_response, RPC_ERROR_INTERNAL, "RPC context unavailable");
  } else {
    (void)flowie_control_management_rpc_server_execute(server, request, &rpc_response);
  }
  rpc_send_response(response, &rpc_response);
  if (rpc_response.result)
    flowie_control_credential_wipe((void *)rpc_response.result, strlen(rpc_response.result));
}

int flowie_control_management_rpc_server_execute(flowie_control_management_rpc_server_t *server,
                                                 Req *request, rpc_response_t *response_out) {
  rpc_request_t rpc_request;
  rpc_method_handler_t handler = NULL;
  int rc;
  if (!server || !request || !response_out || !request->arena || !request->app || !request->path)
    return SALTS_EINVAL;
  memset(response_out, 0, sizeof(*response_out));
  response_out->arena = request->arena;
  response_out->jsonrpc = "2.0";
  response_out->protocol = RPC_PROTOCOL_JSON;
  response_out->max_response_size = server->rpc_context->config.max_response_size;
  if (!request->security || !request->security->authenticated) {
    rpc_set_error(response_out, FLOWIE_CONTROL_RPC_AUTH_REQUIRED, "Authentication required");
    return SALTS_EPERM;
  }
  if (request->body_len == 0u || request->body_len > server->rpc_context->config.max_request_size) {
    rpc_set_error(response_out, RPC_ERROR_INVALID_REQUEST, "Invalid request size");
    return SALTS_EPROTO;
  }
  for (size_t index = 0u; index < request->body_len; ++index) {
    unsigned char byte = (unsigned char)request->body[index];
    if (byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n') continue;
    if (byte == '[') {
      rpc_set_error(response_out, RPC_ERROR_INVALID_REQUEST, "Batch requests are disabled");
      return SALTS_EPROTO;
    }
    break;
  }
  memset(&rpc_request, 0, sizeof(rpc_request));
  rc = rpc_parse_request(request, &rpc_request);
  if (rc != 0) {
    rpc_set_error(response_out, rc, "Invalid request");
    return SALTS_EPROTO;
  }
  response_out->id = rpc_request.id;
  if (!rpc_request.id) {
    rpc_set_error(response_out, RPC_ERROR_INVALID_REQUEST, "Notifications are disabled");
    return SALTS_EPROTO;
  }
  for (size_t index = 0u; index < server->rpc_context->method_count; ++index) {
    if (strcmp(server->rpc_context->methods[index].name, rpc_request.method) == 0) {
      handler = server->rpc_context->methods[index].handler;
      break;
    }
  }
  if (!handler) {
    rpc_set_error(response_out, RPC_ERROR_METHOD_NOT_FOUND, "Method not found");
    return SALTS_ENOENT;
  }
  rc = handler(request, NULL, &rpc_request, response_out);
  if (rc != SALTS_OK && response_out->error_code == 0)
    rpc_set_error(response_out, RPC_ERROR_INTERNAL, "Internal error");
  return rc;
}

static int flowie_control_rpc_registered_method(Req *request, Res *response,
                                                rpc_request_t *rpc_request,
                                                rpc_response_t *rpc_response) {
  flowie_control_management_rpc_server_t *server;
  flowie_control_management_caller_t caller = FLOWIE_CONTROL_MANAGEMENT_CALLER_INIT;
  int rc;
  (void)response;
  if (!request || !request->app || !request->path || !rpc_request || !rpc_response) {
    if (rpc_response) rpc_set_error(rpc_response, RPC_ERROR_INTERNAL, "Internal error");
    return SALTS_EINVAL;
  }
  server = (flowie_control_management_rpc_server_t *)flowie_control_http_app_lookup_context(
      request->app, request->path);
  if (!server) {
    rpc_set_error(rpc_response, RPC_ERROR_INTERNAL, "RPC context unavailable");
    return SALTS_EINVAL;
  }
  rc = server->resolve_caller(server->resolve_caller_ctx, request, &caller);
  if (rc != SALTS_OK) {
    rpc_set_error(rpc_response,
                  rc == SALTS_EPERM ? FLOWIE_CONTROL_RPC_FORBIDDEN : RPC_ERROR_INTERNAL,
                  rc == SALTS_EPERM ? "Forbidden" : "Caller resolution failed");
    return rc;
  }
  return flowie_control_rpc_dispatch(server, &caller, rpc_request, rpc_response);
}

void flowie_control_management_rpc_server_handle(flowie_control_management_rpc_server_t *server,
                                                 Req *request, Res *response) {
  if (!server || !request || !response) return;
  if (!request->app && server->bound_app) request->app = server->bound_app;
  flowie_control_rpc_method(request, response);
}

int flowie_control_management_rpc_server_create(
    const flowie_control_management_rpc_server_config_t *config,
    flowie_control_management_rpc_server_t **out) {
  flowie_control_management_rpc_server_t *server;
  rpc_method_t method;
  if (out) *out = NULL;
  if (!config || config->size < sizeof(*config) || !config->service || !config->rpc_context ||
      !config->resolve_caller || !config->clock || !config->external_https_stats || !out ||
      config->rpc_context->method_count != 0u ||
      config->rpc_context->config.default_protocol != RPC_PROTOCOL_JSON ||
      config->rpc_context->config.enable_batch ||
      config->rpc_context->config.enable_introspection || !config->rpc_context->config.endpoint ||
      !config->rpc_context->config.endpoint[0] ||
      config->rpc_context->config.max_request_size == 0u ||
      config->rpc_context->config.max_request_size > FLOWIE_CONTROL_MANAGEMENT_RPC_REQUEST_MAX ||
      config->policy_executor_workers == 0u ||
      config->policy_executor_workers > FLOWIE_CONTROL_MANAGEMENT_RPC_POLICY_EXECUTOR_MAX_WORKERS ||
      config->policy_executor_queue_capacity == 0u ||
      config->policy_executor_queue_capacity >
          FLOWIE_CONTROL_MANAGEMENT_RPC_POLICY_EXECUTOR_MAX_QUEUE_CAPACITY ||
      config->policy_executor_deadline_ms == 0u ||
      config->policy_executor_deadline_ms >
          FLOWIE_CONTROL_MANAGEMENT_RPC_POLICY_EXECUTOR_MAX_DEADLINE_MS)
    return SALTS_EINVAL;
  server = (flowie_control_management_rpc_server_t *)calloc(1u, sizeof(*server));
  if (!server) return SALTS_ENOMEM;
  server->service = config->service;
  server->rpc_context = config->rpc_context;
  server->resolve_caller = config->resolve_caller;
  server->resolve_caller_ctx = config->resolve_caller_ctx;
  server->clock = config->clock;
  server->clock_ctx = config->clock_ctx;
  server->external_https_stats = config->external_https_stats;
  server->external_https_stats_ctx = config->external_https_stats_ctx;
  server->policy_executor_deadline_ms = config->policy_executor_deadline_ms;
  {
    salts_threadpool_config_t executor_config = {(int)config->policy_executor_workers,
                                                 config->policy_executor_queue_capacity};
    server->policy_executor = salts_threadpool_create_with_config(&executor_config);
    if (!server->policy_executor) {
      free(server);
      return SALTS_ENOMEM;
    }
  }
  memset(&method, 0, sizeof(method));
  method.handler = flowie_control_rpc_registered_method;
  method.description = "Flowie management method";
  method.requires_auth = 1;
  for (size_t index = 0u;
       index < sizeof(FLOWIE_CONTROL_RPC_METHODS) / sizeof(FLOWIE_CONTROL_RPC_METHODS[0]);
       ++index) {
    method.name = FLOWIE_CONTROL_RPC_METHODS[index];
    if (rpc_register_method(server->rpc_context, &method) != 0) {
      flowie_control_management_rpc_server_destroy(server);
      return SALTS_ENOMEM;
    }
    ++server->registered_method_count;
  }
  *out = server;
  return SALTS_OK;
}

int flowie_control_management_rpc_server_bind(flowie_control_management_rpc_server_t *server,
                                              flowie_control_http_app_t *app) {
  const char *endpoint;
  if (!server || !app || server->bound_app) return SALTS_EINVAL;
  endpoint = server->rpc_context->config.endpoint;
  if (flowie_control_http_app_lookup_context(app, endpoint) ||
      flowie_control_http_app_bind_context(app, endpoint, server) != SALTS_OK)
    return SALTS_EBUSY;
  server->bound_app = app;
  if (flowie_control_http_app_post(app, endpoint, flowie_control_rpc_method) != SALTS_OK) {
    server->bound_app = NULL;
    (void)flowie_control_http_app_unbind_context(app, endpoint, server);
    return SALTS_EBUSY;
  }
  return SALTS_OK;
}

void flowie_control_management_rpc_server_unbind(flowie_control_management_rpc_server_t *server) {
  if (!server || !server->bound_app) return;
  (void)flowie_control_http_app_unpost(server->bound_app, server->rpc_context->config.endpoint,
                                       flowie_control_rpc_method);
  (void)flowie_control_http_app_unbind_context(server->bound_app,
                                               server->rpc_context->config.endpoint, server);
  server->bound_app = NULL;
}

void flowie_control_management_rpc_server_destroy(flowie_control_management_rpc_server_t *server) {
  if (!server) return;
  flowie_control_management_rpc_server_unbind(server);
  while (server->registered_method_count > 0u) {
    --server->registered_method_count;
    (void)rpc_unregister_method(server->rpc_context,
                                FLOWIE_CONTROL_RPC_METHODS[server->registered_method_count]);
  }
  salts_threadpool_destroy(server->policy_executor);
  server->policy_executor = NULL;
  memset(server, 0, sizeof(*server));
  free(server);
}
