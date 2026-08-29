#ifndef FLOWIE_CONTROL_TEST_TURBODB_H
#define FLOWIE_CONTROL_TEST_TURBODB_H

#include "flowie_control_database_internal.h"
#include "flowie_control_config_internal.h"
#include "orm.h"

#include <stdio.h>

typedef struct flowie_control_test_turbodb_s {
  orm_config_t config;
  orm_option_t options[3];
  char busy_timeout[16];
} flowie_control_test_turbodb_t;

static int flowie_control_test_turbodb_init(flowie_control_test_turbodb_t *database,
                                            const char *path) {
  int length;
  if (!database || !path || !path[0]) return -1;
  orm_config(&database->config);
  length = snprintf(database->busy_timeout, sizeof(database->busy_timeout), "%u", 1000u);
  if (length <= 0 || (size_t)length >= sizeof(database->busy_timeout)) return -1;
  database->options[0].keyword = orm_view("filename");
  database->options[0].value = orm_view(path);
  database->options[1].keyword = orm_view("open_mode");
  database->options[1].value = orm_view("read_write_create");
  database->options[2].keyword = orm_view("busy_timeout_ms");
  database->options[2].value = orm_view(database->busy_timeout);
  database->config.driver = orm_view("sqlite");
  database->config.options = database->options;
  database->config.option_count = 3u;
  return 0;
}

static int flowie_control_test_database_open(const char *path,
                                             flowie_control_database_t **database_out) {
  flowie_control_test_turbodb_t database;
  if (flowie_control_test_turbodb_init(&database, path) != 0)
    return FLOWIE_CONTROL_DB_MISMATCH;
  return flowie_control_database_open(&database.config, database_out);
}

static int flowie_control_test_runtime_turbodb(flowie_control_config_t *config,
                                               const char *path) {
  if (!config || !path || !path[0]) return -1;
  if (snprintf(config->turbodb.driver, sizeof(config->turbodb.driver), "%s", "sqlite") <= 0 ||
      snprintf(config->turbodb.options[0].keyword,
               sizeof(config->turbodb.options[0].keyword), "%s", "filename") <= 0 ||
      snprintf(config->turbodb.options[0].value, sizeof(config->turbodb.options[0].value), "%s",
               path) <= 0 ||
      snprintf(config->turbodb.options[1].keyword,
               sizeof(config->turbodb.options[1].keyword), "%s", "open_mode") <= 0 ||
      snprintf(config->turbodb.options[1].value, sizeof(config->turbodb.options[1].value), "%s",
               "read_write_create") <= 0 ||
      snprintf(config->turbodb.options[2].keyword,
               sizeof(config->turbodb.options[2].keyword), "%s", "busy_timeout_ms") <= 0 ||
      snprintf(config->turbodb.options[2].value, sizeof(config->turbodb.options[2].value), "%s",
               "1000") <= 0)
    return -1;
  config->turbodb.option_count = 3u;
  return 0;
}

#endif
