#include "flowie_control_data_options_internal.h"

#include "tinytest.h"
#include "salts_error.h"

#include <string.h>

spec("Flowie Control data CLI options") {
  it("parses export and import without accepting system Domain or export dry-run") {
    char *export_argv[] = {"flowie-control-data", "export", "--config", "control.yml",
                           "--domain", "root-a", "--output", "root-a.db"};
    char *import_argv[] = {"flowie-control-data", "import", "--config=control.yml",
                           "--input=root-a.db", "--dry-run"};
    char *system_argv[] = {"flowie-control-data", "export", "--config", "control.yml",
                           "--domain", "system", "--output", "system.db"};
    char *bad_argv[] = {"flowie-control-data", "export", "--config", "control.yml",
                        "--domain", "root-a", "--output", "root-a.db", "--dry-run"};
    char *missing_argv[] = {"flowie-control-data", "import", "--config"};
    flowie_control_data_options_t options = FLOWIE_CONTROL_DATA_OPTIONS_INIT;

    check_equal(flowie_control_data_options_parse(8, export_argv, &options), SALTS_OK);
    check_equal(options.command, FLOWIE_CONTROL_DATA_EXPORT);
    check_equal(options.config_path, "control.yml");
    check_equal(options.domain_id, "root-a");
    check_equal(options.data_path, "root-a.db");
    options = (flowie_control_data_options_t)FLOWIE_CONTROL_DATA_OPTIONS_INIT;
    check_equal(flowie_control_data_options_parse(5, import_argv, &options), SALTS_OK);
    check_equal(options.command, FLOWIE_CONTROL_DATA_IMPORT);
    check_true(options.dry_run);
    options = (flowie_control_data_options_t)FLOWIE_CONTROL_DATA_OPTIONS_INIT;
    check_equal(flowie_control_data_options_parse(8, system_argv, &options), SALTS_EINVAL);
    options = (flowie_control_data_options_t)FLOWIE_CONTROL_DATA_OPTIONS_INIT;
    check_equal(flowie_control_data_options_parse(9, bad_argv, &options), SALTS_EINVAL);
    options = (flowie_control_data_options_t)FLOWIE_CONTROL_DATA_OPTIONS_INIT;
    check_equal(flowie_control_data_options_parse(3, missing_argv, &options), SALTS_EINVAL);
  }
}
