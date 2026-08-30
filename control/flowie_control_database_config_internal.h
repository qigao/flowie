#ifndef FLOWIE_CONTROL_DATABASE_CONFIG_INTERNAL_H
#define FLOWIE_CONTROL_DATABASE_CONFIG_INTERNAL_H

#include "flowie_control_config_internal.h"
#include "orm.h"

#ifdef __cplusplus
extern "C" {
#endif

int flowie_control_database_config_resolve(
    const flowie_control_config_t *config, orm_config_t *database,
    orm_option_t options[FLOWIE_CONTROL_CONFIG_TURBODB_OPTION_COUNT_MAX]);

#ifdef __cplusplus
}
#endif

#endif
