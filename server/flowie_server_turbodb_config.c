#include "flowie_server_turbodb_config_internal.h"

#include "turbo_error.h"
#include "turbo_parser.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
  FLOWIE_SERVER_TURBODB_DRIVER_MAX = 63,
  FLOWIE_SERVER_TURBODB_OPTION_COUNT_MAX = 16,
  FLOWIE_SERVER_TURBODB_OPTION_KEY_MAX = 63,
  FLOWIE_SERVER_TURBODB_OPTION_VALUE_MAX = 4095
};

struct flowie_server_turbodb_config_s {
  orm_config_t database;
  orm_option_t options[FLOWIE_SERVER_TURBODB_OPTION_COUNT_MAX];
  char driver[FLOWIE_SERVER_TURBODB_DRIVER_MAX + 1u];
  char keywords[FLOWIE_SERVER_TURBODB_OPTION_COUNT_MAX][FLOWIE_SERVER_TURBODB_OPTION_KEY_MAX + 1u];
  char values[FLOWIE_SERVER_TURBODB_OPTION_COUNT_MAX][FLOWIE_SERVER_TURBODB_OPTION_VALUE_MAX + 1u];
};

static int flowie_server_turbodb_text_valid(const char *value, size_t maximum) {
  size_t length;
  if (!value || maximum == 0u) return 0;
  length = strnlen(value, maximum + 1u);
  if (length == 0u || length > maximum) return 0;
  for (size_t index = 0u; index < length; ++index) {
    const unsigned char byte = (unsigned char)value[index];
    if (byte < 0x20u || byte == 0x7fu) return 0;
  }
  return 1;
}

static void flowie_server_turbodb_wipe(void *memory, size_t size) {
  volatile unsigned char *cursor = (volatile unsigned char *)memory;
  while (cursor && size-- > 0u)
    *cursor++ = 0u;
}

int flowie_server_turbodb_config_create(const char *driver, const char *options_json,
                                        flowie_server_turbodb_config_t **out) {
  flowie_server_turbodb_config_t *config = NULL;
  turbo_json_doc_t *document = NULL;
  size_t option_count;
  int rc = TURBO_EINVAL;
  if (out) *out = NULL;
  if (!out || !flowie_server_turbodb_text_valid(driver, FLOWIE_SERVER_TURBODB_DRIVER_MAX) ||
      !options_json || !options_json[0])
    return TURBO_EINVAL;
  if (turbo_parse_json((const uint8_t *)options_json, strlen(options_json), &document) != 0 ||
      !document || turbo_json_type((const json_value_t *)document) != TURBO_JSON_OBJECT)
    goto done;
  option_count = turbo_json_object_size((const json_value_t *)document);
  if (option_count > FLOWIE_SERVER_TURBODB_OPTION_COUNT_MAX) {
    rc = TURBO_ENOSPC;
    goto done;
  }
  config = (flowie_server_turbodb_config_t *)calloc(1u, sizeof(*config));
  if (!config) {
    rc = TURBO_ENOMEM;
    goto done;
  }
  memcpy(config->driver, driver, strlen(driver) + 1u);
  orm_config(&config->database);
  config->database.driver = orm_view(config->driver);
  config->database.options = config->options;
  config->database.option_count = (uint32_t)option_count;
  for (size_t index = 0u; index < option_count; ++index) {
    const char *keyword = turbo_json_object_key((const json_value_t *)document, index);
    const json_value_t *value = turbo_json_object_value((const json_value_t *)document, index);
    const char *text;
    if (!flowie_server_turbodb_text_valid(keyword, FLOWIE_SERVER_TURBODB_OPTION_KEY_MAX) ||
        !value || turbo_json_type(value) != TURBO_JSON_STRING) {
      rc = TURBO_EINVAL;
      goto done;
    }
    for (size_t prior = 0u; prior < index; ++prior) {
      if (strcmp(config->keywords[prior], keyword) == 0) {
        rc = TURBO_EALREADY;
        goto done;
      }
    }
    text = turbo_json_string(value);
    if (!flowie_server_turbodb_text_valid(text, FLOWIE_SERVER_TURBODB_OPTION_VALUE_MAX)) {
      rc = TURBO_EINVAL;
      goto done;
    }
    memcpy(config->keywords[index], keyword, strlen(keyword) + 1u);
    memcpy(config->values[index], text, strlen(text) + 1u);
    config->options[index].keyword = orm_view(config->keywords[index]);
    config->options[index].value = orm_view(config->values[index]);
  }
  *out = config;
  config = NULL;
  rc = TURBO_OK;

done:
  turbo_free_json(&document);
  flowie_server_turbodb_config_destroy(config);
  return rc;
}

const orm_config_t *
flowie_server_turbodb_config_database(const flowie_server_turbodb_config_t *config) {
  return config ? &config->database : NULL;
}

const char *flowie_server_turbodb_config_driver(const flowie_server_turbodb_config_t *config) {
  return config ? config->driver : NULL;
}

void flowie_server_turbodb_config_destroy(flowie_server_turbodb_config_t *config) {
  if (!config) return;
  flowie_server_turbodb_wipe(config, sizeof(*config));
  free(config);
}
