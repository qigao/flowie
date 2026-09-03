#ifndef FLOWIE_CONTROL_DATA_OPTIONS_INTERNAL_H
#define FLOWIE_CONTROL_DATA_OPTIONS_INTERNAL_H

#include "flowie_security.h"
#include "salts_fs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum flowie_control_data_command_e {
  FLOWIE_CONTROL_DATA_COMMAND_NONE = 0,
  FLOWIE_CONTROL_DATA_EXPORT,
  FLOWIE_CONTROL_DATA_IMPORT
} flowie_control_data_command_t;

typedef struct flowie_control_data_options_s {
  size_t size;
  flowie_control_data_command_t command;
  char config_path[SALTS_FS_MAX_PATH];
  char env_file[SALTS_FS_MAX_PATH];
  char domain_id[FLOWIE_SECURITY_ID_MAX + 1u];
  char data_path[SALTS_FS_MAX_PATH];
  int dry_run;
} flowie_control_data_options_t;

#define FLOWIE_CONTROL_DATA_OPTIONS_INIT                                                           \
  {sizeof(flowie_control_data_options_t), FLOWIE_CONTROL_DATA_COMMAND_NONE, {0}, {0}, {0}, {0}, 0}

int flowie_control_data_options_parse(int argc, char **argv, flowie_control_data_options_t *out);

#ifdef __cplusplus
}
#endif

#endif
