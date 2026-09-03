#include "flowie_server_turbodb_config_internal.h"

#include "tinytest.h"
#include "salts_error.h"

#include <string.h>

static int option_equal(const orm_option_t *option, const char *keyword, const char *value) {
  return option && keyword && value && option->keyword.len == strlen(keyword) &&
         option->value.len == strlen(value) &&
         memcmp(option->keyword.data, keyword, option->keyword.len) == 0 &&
         memcmp(option->value.data, value, option->value.len) == 0;
}

spec("Flowie server TurboDB configuration") {
  it("projects PostgreSQL driver and arbitrary string options without driver branching") {
    flowie_server_turbodb_config_t *config = NULL;
    const orm_config_t *database;

    check_equal(flowie_server_turbodb_config_create(
                    "postgresql",
                    "{\"conninfo\":\"host=pg dbname=flowie\",\"application_name\":\"flowie\"}",
                    &config),
                SALTS_OK);
    check_not_null(config);
    database = flowie_server_turbodb_config_database(config);
    check_not_null(database);
    check_equal(flowie_server_turbodb_config_driver(config), "postgresql");
    check_equal(database->option_count, 2u);
    check_true(option_equal(&database->options[0], "conninfo", "host=pg dbname=flowie"));
    check_true(option_equal(&database->options[1], "application_name", "flowie"));
    flowie_server_turbodb_config_destroy(config);
  }

  it("keeps SQLite filename as an option rather than a server-side special case") {
    flowie_server_turbodb_config_t *config = NULL;
    const orm_config_t *database;

    check_equal(
        flowie_server_turbodb_config_create("sqlite", "{\"filename\":\":memory:\"}", &config),
        SALTS_OK);
    database = flowie_server_turbodb_config_database(config);
    check_equal(database->option_count, 1u);
    check_true(option_equal(&database->options[0], "filename", ":memory:"));
    flowie_server_turbodb_config_destroy(config);
  }

  it("rejects malformed, non-string, duplicate, and unbounded options") {
    static const char too_many[] = "{\"k01\":\"v\",\"k02\":\"v\",\"k03\":\"v\",\"k04\":\"v\","
                                   "\"k05\":\"v\",\"k06\":\"v\",\"k07\":\"v\",\"k08\":\"v\","
                                   "\"k09\":\"v\",\"k10\":\"v\",\"k11\":\"v\",\"k12\":\"v\","
                                   "\"k13\":\"v\",\"k14\":\"v\",\"k15\":\"v\",\"k16\":\"v\","
                                   "\"k17\":\"v\"}";
    flowie_server_turbodb_config_t *config = (flowie_server_turbodb_config_t *)1;

    check_equal(flowie_server_turbodb_config_create("", "{}", &config), SALTS_EINVAL);
    check_null(config);
    check_equal(flowie_server_turbodb_config_create("sqlite", "[]", &config), SALTS_EINVAL);
    check_null(config);
    check_equal(flowie_server_turbodb_config_create("sqlite", "{\"filename\":1}", &config),
                SALTS_EINVAL);
    check_null(config);
    check_equal(flowie_server_turbodb_config_create(
                    "sqlite", "{\"filename\":\"a\",\"filename\":\"b\"}", &config),
                SALTS_EALREADY);
    check_null(config);
    check_equal(flowie_server_turbodb_config_create("sqlite", too_many, &config), SALTS_ENOSPC);
    check_null(config);
  }
}
