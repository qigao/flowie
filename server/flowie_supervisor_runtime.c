#include "flowie_supervisor_runtime_internal.h"

#include "salts_error.h"
#include "salts_str.h"

#include <stdlib.h>

struct flowie_supervisor_runtime_s {
  tstr worker_program;
  tstr profile;
  tstr config_path;
  tstr graph_path;
  tstr control_config_path;
  salts_process_t *process;
  size_t max_output_bytes;
  int check_only;
  int require_security;
  int capture_output;
  int started;
};

static void flowie_supervisor_error_reset(flowie_supervisor_error_t *error) {
  if (!error || error->size != sizeof(*error)) return;
  *error = (flowie_supervisor_error_t)FLOWIE_SUPERVISOR_ERROR_INIT;
}

static void flowie_supervisor_error_set(flowie_supervisor_error_t *error, const char *operation,
                                        int status) {
  if (!error || error->size != sizeof(*error)) return;
  flowie_supervisor_error_reset(error);
  error->operation = operation;
  error->status = status;
}

static int flowie_supervisor_copy_config(flowie_supervisor_runtime_t *runtime,
                                         const flowie_supervisor_runtime_config_t *config) {
  runtime->worker_program = tstr_dup(config->worker_program);
  runtime->profile = tstr_dup(config->profile);
  runtime->config_path = tstr_dup(config->config_path);
  runtime->graph_path = tstr_dup(config->graph_path);
  if (config->control_config_path)
    runtime->control_config_path = tstr_dup(config->control_config_path);
  if (!runtime->worker_program || !runtime->profile || !runtime->config_path ||
      !runtime->graph_path || (config->control_config_path && !runtime->control_config_path))
    return SALTS_ENOMEM;
  runtime->check_only = config->check_only != 0;
  runtime->require_security = config->require_security != 0;
  runtime->capture_output = config->capture_output != 0;
  runtime->max_output_bytes = config->max_output_bytes;
  return SALTS_OK;
}

int flowie_supervisor_runtime_create(const flowie_supervisor_runtime_config_t *config,
                                     flowie_supervisor_runtime_t **out,
                                     flowie_supervisor_error_t *error) {
  flowie_supervisor_runtime_t *runtime;
  int rc;
  if (out) *out = NULL;
  flowie_supervisor_error_reset(error);
  if (!config || config->size != sizeof(*config) || !out || !config->worker_program ||
      !config->worker_program[0] || !config->profile || !config->profile[0] ||
      !config->config_path || !config->config_path[0] || !config->graph_path ||
      !config->graph_path[0] ||
      (config->control_config_path && !config->control_config_path[0]) ||
      (config->capture_output && config->max_output_bytes == 0)) {
    flowie_supervisor_error_set(error, "validate supervisor configuration", SALTS_EINVAL);
    return SALTS_EINVAL;
  }
  runtime = (flowie_supervisor_runtime_t *)calloc(1u, sizeof(*runtime));
  if (!runtime) {
    flowie_supervisor_error_set(error, "create supervisor", SALTS_ENOMEM);
    return SALTS_ENOMEM;
  }
  rc = flowie_supervisor_copy_config(runtime, config);
  if (rc != SALTS_OK) {
    flowie_supervisor_error_set(error, "copy supervisor configuration", rc);
    flowie_supervisor_runtime_destroy(runtime);
    return rc;
  }
  *out = runtime;
  return SALTS_OK;
}

int flowie_supervisor_runtime_start(flowie_supervisor_runtime_t *runtime,
                                    flowie_supervisor_error_t *error) {
  const char *args[10];
  size_t count = 0;
  salts_process_options_t options;
  int rc;
  flowie_supervisor_error_reset(error);
  if (!runtime) {
    flowie_supervisor_error_set(error, "start worker", SALTS_EINVAL);
    return SALTS_EINVAL;
  }
  if (runtime->started) {
    flowie_supervisor_error_set(error, "start worker", SALTS_EALREADY);
    return SALTS_EALREADY;
  }
  if (runtime->check_only) args[count++] = "--check";
  if (runtime->require_security) args[count++] = "--require-security";
  args[count++] = "--profile";
  args[count++] = runtime->profile;
  if (runtime->control_config_path) {
    args[count++] = "--control-config";
    args[count++] = runtime->control_config_path;
  }
  args[count++] = runtime->config_path;
  args[count++] = runtime->graph_path;
  args[count] = NULL;

  salts_process_options_init(&options);
  options.program = runtime->worker_program;
  options.args = args;
  options.flags =
      runtime->capture_output ? SALTS_PROCESS_CAPTURE_STDOUT | SALTS_PROCESS_CAPTURE_STDERR : 0u;
  options.timeout_ms = 0;
  options.max_output_bytes = runtime->max_output_bytes;
  rc = salts_process_spawn(&options, &runtime->process);
  if (rc != SALTS_OK) {
    flowie_supervisor_error_set(error, "spawn worker", rc);
    return rc;
  }
  runtime->started = 1;
  return SALTS_OK;
}

int flowie_supervisor_runtime_wait_for(flowie_supervisor_runtime_t *runtime, uint64_t timeout_ms,
                                       salts_process_result_t *result,
                                       flowie_supervisor_error_t *error) {
  int rc;
  flowie_supervisor_error_reset(error);
  if (!runtime || !runtime->started || !result) {
    flowie_supervisor_error_set(error, "wait for worker", SALTS_EINVAL);
    return SALTS_EINVAL;
  }
  rc = salts_process_wait_for(runtime->process, timeout_ms, result);
  if (rc != SALTS_OK && rc != SALTS_ETIMEDOUT)
    flowie_supervisor_error_set(error, "wait for worker", rc);
  return rc;
}

int flowie_supervisor_runtime_stop(flowie_supervisor_runtime_t *runtime,
                                   salts_process_result_t *result,
                                   flowie_supervisor_error_t *error) {
  salts_process_result_t local_result;
  int rc;
  flowie_supervisor_error_reset(error);
  if (!runtime || !runtime->started) {
    flowie_supervisor_error_set(error, "stop worker", SALTS_EINVAL);
    return SALTS_EINVAL;
  }
  rc = salts_process_poll(runtime->process, &local_result);
  if (rc == SALTS_EBUSY) {
    rc = salts_process_terminate(runtime->process);
    if (rc != SALTS_OK) {
      flowie_supervisor_error_set(error, "terminate worker", rc);
      return rc;
    }
    rc = salts_process_wait(runtime->process, &local_result);
  }
  if (rc != SALTS_OK) {
    flowie_supervisor_error_set(error, "reap worker", rc);
    return rc;
  }
  if (result) *result = local_result;
  return SALTS_OK;
}

int flowie_supervisor_runtime_read_stdout(flowie_supervisor_runtime_t *runtime, void *buffer,
                                          size_t capacity, size_t *out_read) {
  if (out_read) *out_read = 0;
  if (!runtime || !runtime->started || !runtime->capture_output) return SALTS_ENOTSUP;
  return salts_process_read_stdout(runtime->process, buffer, capacity, out_read);
}

int flowie_supervisor_runtime_read_stderr(flowie_supervisor_runtime_t *runtime, void *buffer,
                                          size_t capacity, size_t *out_read) {
  if (out_read) *out_read = 0;
  if (!runtime || !runtime->started || !runtime->capture_output) return SALTS_ENOTSUP;
  return salts_process_read_stderr(runtime->process, buffer, capacity, out_read);
}

void flowie_supervisor_runtime_destroy(flowie_supervisor_runtime_t *runtime) {
  if (!runtime) return;
  salts_process_destroy(runtime->process);
  tstr_free(runtime->control_config_path);
  tstr_free(runtime->graph_path);
  tstr_free(runtime->config_path);
  tstr_free(runtime->profile);
  tstr_free(runtime->worker_program);
  free(runtime);
}
