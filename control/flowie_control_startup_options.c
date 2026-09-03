#include "flowie_control_startup_options_internal.h"

#include "salts_error.h"
#include <cmd_arger.h>
#include <dotenv.h>

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static int flowie_control_startup_copy(char *destination, size_t capacity, const char *source,
                                       int required) {
  size_t length;
  if (!destination || capacity == 0u) return SALTS_EINVAL;
  destination[0] = '\0';
  if (!source || !source[0]) return required ? SALTS_EINVAL : SALTS_OK;
  length = strnlen(source, capacity);
  if (length >= capacity) return SALTS_ENAMETOOLONG;
  memcpy(destination, source, length + 1u);
  return SALTS_OK;
}

static int flowie_control_startup_env_file(int argc, char **argv, const char **env_file_out) {
  const char *selected = NULL;
  int found = 0;
  if (env_file_out) *env_file_out = NULL;
  if (argc <= 0 || !argv || !env_file_out) return SALTS_EINVAL;
  for (int index = 1; index < argc; ++index) {
    const char *argument = argv[index];
    if (!argument) return SALTS_EINVAL;
    if (strcmp(argument, "--env-file") == 0 || strcmp(argument, "-E") == 0) {
      if (found || ++index >= argc || !argv[index] || !argv[index][0]) return SALTS_EINVAL;
      selected = argv[index];
      found = 1;
    } else if (strncmp(argument, "--env-file=", sizeof("--env-file=") - 1u) == 0) {
      if (found || argument[sizeof("--env-file=") - 1u] == '\0') return SALTS_EINVAL;
      selected = argument + sizeof("--env-file=") - 1u;
      found = 1;
    }
  }
  if (!selected) selected = getenv(FLOWIE_CONTROL_ENV_ENV_FILE);
  if (selected && strnlen(selected, SALTS_FS_MAX_PATH) >= SALTS_FS_MAX_PATH)
    return SALTS_ENAMETOOLONG;
  *env_file_out = selected;
  return SALTS_OK;
}

int flowie_control_startup_options_parse(int argc, char **argv,
                                         flowie_control_startup_options_t *out) {
  flowie_control_startup_options_t resolved = FLOWIE_CONTROL_STARTUP_OPTIONS_INIT;
  const char *selected_env_file = NULL;
  char *config_path = NULL;
  char *env_file = NULL;
  CmdArgerBool check_only = cmd_arger_false;
  CmdArgerDesc options[3];
  int rc;
  if (!out || out->size < sizeof(*out)) return SALTS_EINVAL;
  *out = resolved;
  rc = flowie_control_startup_env_file(argc, argv, &selected_env_file);
  if (rc != SALTS_OK) return rc;
  if (selected_env_file && dotenv_load(selected_env_file, false) != 0) return SALTS_EIO;

  options[0] = cmd_arger_with_env(
      cmd_arger_desc_string_sh(&config_path, "config", "c", "Controller configuration file"),
      FLOWIE_CONTROL_ENV_CONFIG);
  options[1] = cmd_arger_with_env(
      cmd_arger_desc_string_sh(&env_file, "env-file", "E", "Explicit DotEnv file"),
      FLOWIE_CONTROL_ENV_ENV_FILE);
  options[2] = cmd_arger_with_env(
      cmd_arger_desc_flag(&check_only, "check", "Validate configuration and exit"),
      FLOWIE_CONTROL_ENV_CHECK);
  cmd_arger_parse(options, sizeof(options) / sizeof(options[0]), NULL, 0u, argc, argv,
                  "flowie-control 0.1.0", cmd_arger_false);

  rc = flowie_control_startup_copy(resolved.config_path, sizeof(resolved.config_path), config_path,
                                   1);
  if (rc == SALTS_OK)
    rc = flowie_control_startup_copy(resolved.env_file, sizeof(resolved.env_file), env_file, 0);
  if (rc == SALTS_OK) {
    resolved.check_only = check_only == cmd_arger_true ? 1 : 0;
    *out = resolved;
  }
  return rc;
}
