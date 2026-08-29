#ifndef FLOWIE_SERVER_TURBODB_CONFIG_INTERNAL_H
#define FLOWIE_SERVER_TURBODB_CONFIG_INTERNAL_H

#include "orm.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FLOWIE_SERVER_TURBODB_DEFAULT_DRIVER "sqlite"
#define FLOWIE_SERVER_TURBODB_DEFAULT_OPTIONS "{\"filename\":\"flowie-protocol.sqlite3\"}"

typedef struct flowie_server_turbodb_config_s flowie_server_turbodb_config_t;

int flowie_server_turbodb_config_create(const char *driver, const char *options_json,
                                        flowie_server_turbodb_config_t **out);
const orm_config_t *
flowie_server_turbodb_config_database(const flowie_server_turbodb_config_t *config);
const char *flowie_server_turbodb_config_driver(const flowie_server_turbodb_config_t *config);
void flowie_server_turbodb_config_destroy(flowie_server_turbodb_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
