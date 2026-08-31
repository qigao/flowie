#ifndef FLOWIE_SERVER_HTTP_SECURITY_INTERNAL_H
#define FLOWIE_SERVER_HTTP_SECURITY_INTERNAL_H

#include "flowie_security.h"
#include "flowie_server_config_internal.h"

#include <stddef.h>

typedef struct flowie_server_http_security_s flowie_server_http_security_t;

int flowie_server_http_security_create(coro_context_t *context,
                                       const flowie_server_http_provider_config_t *auth,
                                       const flowie_server_http_provider_config_t *acl,
                                       flowie_server_http_security_t **out);
void flowie_server_http_security_destroy(flowie_server_http_security_t *security);
const flowie_security_auth_provider_t *flowie_server_http_security_auth_provider(
    const flowie_server_http_security_t *security);
const flowie_security_authorization_provider_t *flowie_server_http_security_acl_provider(
    const flowie_server_http_security_t *security);

int flowie_server_http_auth_encode(const flowie_security_auth_request_t *request,
                                   char **body_out, size_t *body_size_out);
int flowie_server_http_auth_decode(const char *body, size_t body_size, const char *method,
                                   flowie_security_principal_t *principal_out);
int flowie_server_http_acl_encode(const flowie_security_request_t *request,
                                  char **body_out, size_t *body_size_out);
int flowie_server_http_acl_decode(const char *body, size_t body_size,
                                  flowie_security_decision_t *decision_out);
void flowie_server_http_body_destroy(char *body, size_t body_size, int sensitive);

#endif
