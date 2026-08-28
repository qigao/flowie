#ifndef FLOWIE_SECURITY_TURBODB_H
#define FLOWIE_SECURITY_TURBODB_H

#include "flowie_security.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FLOWIE_SECURITY_TURBODB_API_VERSION 1u
#define FLOWIE_SECURITY_TURBODB_DEFAULT_MAX_RULES 4096u

typedef struct flowie_security_turbodb_provider_s flowie_security_turbodb_provider_t;

typedef struct flowie_security_turbodb_config_s {
  size_t size;
  uint32_t api_version;
  const char *database_path;
  const char *namespace_name;
  int busy_timeout_ms;
  size_t max_rules;
} flowie_security_turbodb_config_t;

#define FLOWIE_SECURITY_TURBODB_CONFIG_INIT                                                        \
  {                                                                                                \
      sizeof(flowie_security_turbodb_config_t),                                                    \
      FLOWIE_SECURITY_TURBODB_API_VERSION,                                                         \
      NULL,                                                                                        \
      NULL,                                                                                        \
      1000,                                                                                        \
      FLOWIE_SECURITY_TURBODB_DEFAULT_MAX_RULES}

int flowie_security_turbodb_provider_create(const flowie_security_turbodb_config_t *config,
                                            flowie_security_turbodb_provider_t **out);
const flowie_security_policy_provider_t *
flowie_security_turbodb_provider_interface(const flowie_security_turbodb_provider_t *provider);
void flowie_security_turbodb_provider_destroy(flowie_security_turbodb_provider_t *provider);

#ifdef __cplusplus
}
#endif

#endif
