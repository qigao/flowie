#include "flowie_control_data_options_internal.h"

#include "flowie_control_management_service_internal.h"
#include "turbo_error.h"
#include "turbo_parser.h"

#include <string.h>

static int data_option_copy(char *output, size_t capacity, const char *value) {
  const size_t size = value ? strnlen(value, capacity) : 0u;
  if (!output || !value || size == 0u) return TURBO_EINVAL;
  if (size >= capacity) return TURBO_ENAMETOOLONG;
  memcpy(output, value, size + 1u);
  return TURBO_OK;
}

static int data_option_take(int argc, char **argv, int *index, const char *name,
                            const char **value_out) {
  const char *argument = argv[*index];
  const size_t name_size = strlen(name);
  *value_out = NULL;
  if (strcmp(argument, name) == 0) {
    if (++*index >= argc || !argv[*index] || !argv[*index][0]) return TURBO_EINVAL;
    *value_out = argv[*index];
    return 1;
  }
  if (strncmp(argument, name, name_size) == 0 && argument[name_size] == '=') {
    if (!argument[name_size + 1u]) return TURBO_EINVAL;
    *value_out = argument + name_size + 1u;
    return 1;
  }
  return 0;
}

int flowie_control_data_options_parse(int argc, char **argv, flowie_control_data_options_t *out) {
  flowie_control_data_options_t value = FLOWIE_CONTROL_DATA_OPTIONS_INIT;
  int has_config = 0;
  int has_env = 0;
  int has_domain = 0;
  int has_data = 0;
  int rc = TURBO_OK;
  if (!out || out->size < sizeof(*out) || argc < 2 || !argv) return TURBO_EINVAL;
  *out = value;
  if (strcmp(argv[1], "export") == 0)
    value.command = FLOWIE_CONTROL_DATA_EXPORT;
  else if (strcmp(argv[1], "import") == 0)
    value.command = FLOWIE_CONTROL_DATA_IMPORT;
  else
    return TURBO_EINVAL;
  for (int index = 2; rc == TURBO_OK && index < argc; ++index) {
    const char *argument = argv[index];
    const char *selected;
    int matched;
    if (!argument) return TURBO_EINVAL;
    if (strcmp(argument, "--dry-run") == 0) {
      if (value.dry_run) return TURBO_EINVAL;
      value.dry_run = 1;
      continue;
    }
    matched = data_option_take(argc, argv, &index, "--config", &selected);
    if (matched < 0) return matched;
    if (matched) {
      if (has_config) return TURBO_EINVAL;
      has_config = 1;
      rc = data_option_copy(value.config_path, sizeof(value.config_path), selected);
      continue;
    }
    matched = data_option_take(argc, argv, &index, "--env-file", &selected);
    if (matched < 0) return matched;
    if (matched) {
      if (has_env) return TURBO_EINVAL;
      has_env = 1;
      rc = data_option_copy(value.env_file, sizeof(value.env_file), selected);
      continue;
    }
    matched = data_option_take(argc, argv, &index, "--domain", &selected);
    if (matched < 0) return matched;
    if (matched) {
      if (has_domain) return TURBO_EINVAL;
      has_domain = 1;
      rc = data_option_copy(value.domain_id, sizeof(value.domain_id), selected);
      continue;
    }
    matched = data_option_take(argc, argv, &index,
                               value.command == FLOWIE_CONTROL_DATA_EXPORT ? "--output"
                                                                          : "--input",
                               &selected);
    if (matched < 0) return matched;
    if (matched) {
      if (has_data) return TURBO_EINVAL;
      has_data = 1;
      rc = data_option_copy(value.data_path, sizeof(value.data_path), selected);
      continue;
    }
    return TURBO_EINVAL;
  }
  if (rc != TURBO_OK || !has_config || !has_data ||
      (value.command == FLOWIE_CONTROL_DATA_EXPORT &&
       (!has_domain || value.dry_run ||
        strcmp(value.domain_id, FLOWIE_CONTROL_MANAGEMENT_SYSTEM_DOMAIN) == 0)) ||
      (value.command == FLOWIE_CONTROL_DATA_IMPORT && has_domain))
    return rc == TURBO_OK ? TURBO_EINVAL : rc;
  if (value.env_file[0] && turbo_dotenv_load(value.env_file, false) != 0) return TURBO_EIO;
  *out = value;
  return TURBO_OK;
}
