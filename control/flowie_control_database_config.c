#include "flowie_control_database_config_internal.h"

#include "salts_error.h"

#include <stdlib.h>
#include <string.h>

int flowie_control_database_config_resolve(
    const flowie_control_config_t *config, orm_config_t *database,
    orm_option_t options[FLOWIE_CONTROL_CONFIG_TURBODB_OPTION_COUNT_MAX]) {
  if (!config || config->size < sizeof(*config) || config->version != FLOWIE_CONTROL_CONFIG_VERSION ||
      !database || !options || !config->turbodb.driver[0] ||
      config->turbodb.option_count > FLOWIE_CONTROL_CONFIG_TURBODB_OPTION_COUNT_MAX)
    return SALTS_EINVAL;
  orm_config(database);
  database->driver = orm_view(config->turbodb.driver);
  database->options = options;
  database->option_count = (uint32_t)config->turbodb.option_count;
  for (size_t index = 0u; index < config->turbodb.option_count; ++index) {
    const flowie_control_config_turbodb_option_t *input = &config->turbodb.options[index];
    const char *value = input->value;
    if (!input->keyword[0] || !value[0]) return SALTS_EINVAL;
    if (flowie_control_config_turbodb_secret_option(input->keyword) &&
        !flowie_control_config_secret_ref_valid(value))
      return SALTS_EINVAL;
    if (strncmp(value, "env://", sizeof("env://") - 1u) == 0) {
      if (!flowie_control_config_secret_ref_valid(value)) return SALTS_EINVAL;
      value = getenv(value + sizeof("env://") - 1u);
      if (!value || !value[0]) return SALTS_ENOENT;
    }
    options[index].keyword = orm_view(input->keyword);
    options[index].value = orm_view(value);
  }
  return SALTS_OK;
}
