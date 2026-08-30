#include "flowie_control_config_internal.h"
#include "flowie_control_data_options_internal.h"
#include "flowie_control_data_transfer_internal.h"
#include "flowie_control_database_config_internal.h"
#include "flowie_control_store_internal.h"

#include "turbo_error.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int data_report(const char *operation, int status) {
  (void)fprintf(stderr, "flowie-control-data: %s failed: status=%d (%s)\n", operation, status,
                turbo_strerror(status));
  return EXIT_FAILURE;
}

static void data_usage(void) {
  (void)printf(
      "Usage:\n"
      "  flowie-control-data export --config <control.yml> --domain <id> --output <manifest.db> "
      "[--env-file <file>]\n"
      "  flowie-control-data import --config <control.yml> --input <manifest.db> [--dry-run] "
      "[--env-file <file>]\n");
}

int main(int argc, char **argv) {
  flowie_control_data_options_t options = FLOWIE_CONTROL_DATA_OPTIONS_INIT;
  flowie_control_config_t config = FLOWIE_CONTROL_CONFIG_INIT;
  flowie_control_config_error_t config_error = FLOWIE_CONTROL_CONFIG_ERROR_INIT;
  flowie_control_data_transfer_result_t result = FLOWIE_CONTROL_DATA_TRANSFER_RESULT_INIT;
  flowie_control_store_config_t store_config = FLOWIE_CONTROL_STORE_CONFIG_INIT;
  orm_config_t database;
  orm_option_t database_options[FLOWIE_CONTROL_CONFIG_TURBODB_OPTION_COUNT_MAX];
  flowie_control_store_t *store = NULL;
  int rc;
  if (argc == 2 && argv && argv[1] &&
      (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
    data_usage();
    return EXIT_SUCCESS;
  }
  rc = flowie_control_data_options_parse(argc, argv, &options);
  if (rc != TURBO_OK) return data_report("option parsing", rc);
  rc = flowie_control_config_load(options.config_path, &config, &config_error);
  if (rc != TURBO_OK) {
    (void)fprintf(stderr,
                  "flowie-control-data: configuration failed: status=%d path=%s message=%s\n", rc,
                  config_error.path[0] ? config_error.path : "$",
                  config_error.message[0] ? config_error.message : "invalid configuration");
    return EXIT_FAILURE;
  }
  rc = flowie_control_database_config_resolve(&config, &database, database_options);
  if (rc != TURBO_OK) return data_report("database configuration", rc);
  store_config.database = &database;
  rc = flowie_control_store_open(&store_config, &store);
  if (rc != TURBO_OK) return data_report("database open", rc);
  if (options.command == FLOWIE_CONTROL_DATA_EXPORT)
    rc = flowie_control_data_export(flowie_control_store_repository(store), options.domain_id,
                                    options.data_path, &result);
  else
    rc = flowie_control_data_import(flowie_control_store_repository(store), options.data_path,
                                    options.dry_run, &result);
  flowie_control_store_destroy(store);
  if (rc != TURBO_OK) {
    if (result.mutated)
      (void)fprintf(stderr,
                    "flowie-control-data: import stopped after partial replay; "
                    "target_revision=%" PRIu64 "; rerun the same manifest after correction\n",
                    result.target_revision);
    return data_report(options.command == FLOWIE_CONTROL_DATA_EXPORT ? "export" : "import", rc);
  }
  (void)printf("flowie-control-data: %s%s source_revision=%" PRIu64
               " target_revision=%" PRIu64 " users=%zu groups=%zu memberships=%zu roles=%zu "
               "assignments=%zu policy_rules=%zu published=%d\n",
               options.command == FLOWIE_CONTROL_DATA_EXPORT ? "export" : "import",
               options.dry_run ? " dry-run" : "", result.source_revision, result.target_revision,
               result.user_count, result.group_count, result.membership_count, result.role_count,
               result.assignment_count, result.policy_rule_count, result.policy_published);
  return EXIT_SUCCESS;
}
