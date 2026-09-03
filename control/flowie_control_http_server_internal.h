#ifndef FLOWIE_CONTROL_HTTP_SERVER_INTERNAL_H
#define FLOWIE_CONTROL_HTTP_SERVER_INTERNAL_H

#include "salts_buffer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  OK = 200,
  NO_CONTENT = 204,
  SEE_OTHER = 303,
  BAD_REQUEST = 400,
  UNAUTHORIZED = 401,
  FORBIDDEN = 403,
  NOT_FOUND = 404,
  CONFLICT = 409,
  PAYLOAD_TOO_LARGE = 413,
  UNSUPPORTED_MEDIA_TYPE = 415,
  TOO_MANY_REQUESTS = 429,
  INTERNAL_SERVER_ERROR = 500,
  SERVICE_UNAVAILABLE = 503
};

#define FLOWIE_CONTROL_HTTP_PEER_CERTIFICATE_SHA256_CAPACITY 72u

typedef struct request_item_s {
  char *key;
  char *value;
} request_item_t;

typedef struct request_s {
  request_item_t *items;
  int count;
  int capacity;
} request_t;

typedef struct flowie_control_http_security_context_s {
  bool authenticated;
} flowie_control_http_security_context_t;

typedef struct flowie_control_http_request_context_s {
  void *data;
  size_t size;
  void (*cleanup)(void *data);
} flowie_control_http_request_context_t;

typedef struct flowie_control_http_app_s flowie_control_http_app_t;

typedef struct Req {
  flowie_control_http_app_t *app;
  mem_pool_t *arena;
  void *client;
  char *method;
  char *path;
  char *body;
  size_t body_len;
  int body_stream;
  request_t headers;
  request_t query;
  request_t params;
  flowie_control_http_request_context_t context;
  flowie_control_http_security_context_t *security;
  const char *peer_certificate_sha256;
  const char *remote_address;
} Req;

typedef struct flowie_control_http_header_s {
  char *name;
  char *value;
} flowie_control_http_header_t;

typedef struct Res {
  unsigned int status;
  char *content_type;
  void *body;
  size_t body_len;
  flowie_control_http_header_t *headers;
  size_t header_count;
  size_t header_capacity;
  int completed;
  int error;
} Res;

typedef struct cookie_options_s {
  int max_age;
  char *path;
  char *same_site;
  bool http_only;
  bool secure;
} cookie_options_t;

typedef void (*flowie_control_http_handler_fn)(Req *request, Res *response);
struct Chain;
typedef int (*flowie_control_http_middleware_fn)(Req *request, Res *response,
                                                 struct Chain *chain);

typedef struct Chain {
  flowie_control_http_handler_fn terminal;
  int called;
} Chain;

typedef struct flowie_control_http_limits_s {
  size_t max_header_name_length;
  size_t max_header_value_length;
  size_t max_url_length;
  size_t max_request_body_size;
  size_t max_headers_count;
} flowie_control_http_limits_t;

typedef struct flowie_control_http_tls_config_s {
  size_t size;
  const char *cert_file;
  const char *key_file;
  const char *key_password;
  const char *ca_file;
  int client_auth_required;
} flowie_control_http_tls_config_t;

#define FLOWIE_CONTROL_HTTP_TLS_CONFIG_INIT                                                        \
  {sizeof(flowie_control_http_tls_config_t), NULL, NULL, NULL, NULL, 0}

flowie_control_http_app_t *flowie_control_http_app_create(void);
int flowie_control_http_app_configure(flowie_control_http_app_t *app,
                                      const flowie_control_http_limits_t *limits,
                                      size_t worker_count, size_t worker_queue_capacity);
void flowie_control_http_app_destroy(flowie_control_http_app_t *app);
int flowie_control_http_app_bind_context(flowie_control_http_app_t *app, const char *path,
                                         void *context);
int flowie_control_http_app_unbind_context(flowie_control_http_app_t *app, const char *path,
                                           void *context);
void *flowie_control_http_app_lookup_context(flowie_control_http_app_t *app, const char *path);
int flowie_control_http_app_get(flowie_control_http_app_t *app, const char *path,
                                flowie_control_http_handler_fn handler);
int flowie_control_http_app_post(flowie_control_http_app_t *app, const char *path,
                                 flowie_control_http_handler_fn handler);
int flowie_control_http_app_unget(flowie_control_http_app_t *app, const char *path,
                                  flowie_control_http_handler_fn handler);
int flowie_control_http_app_unpost(flowie_control_http_app_t *app, const char *path,
                                   flowie_control_http_handler_fn handler);
int flowie_control_http_app_use(flowie_control_http_app_t *app,
                                flowie_control_http_middleware_fn middleware);
int flowie_control_http_app_start_tls(flowie_control_http_app_t *app, const char *host,
                                      uint16_t port,
                                      const flowie_control_http_tls_config_t *tls);
int flowie_control_http_tls_validate(const flowie_control_http_tls_config_t *tls);
int flowie_control_http_app_port(const flowie_control_http_app_t *app, uint16_t *port_out);
int flowie_control_http_app_stop(flowie_control_http_app_t *app, uint32_t timeout_ms);
char *flowie_control_http_url_encode(const char *value);
char *flowie_control_http_url_decode(const char *value);

int next(Chain *chain, Req *request, Res *response);
void set_context(Req *request, void *data, size_t size, void (*cleanup)(void *));
void *get_context(Req *request);
void set_header(Res *response, const char *name, const char *value);
void reply(Res *response, int status, const char *content_type, const void *body,
           size_t body_len);
void set_cookie(Res *response, const char *name, const char *value,
                cookie_options_t *options);
void flowie_control_http_response_clear(Res *response);

static inline void send_text(Res *response, int status, const char *body) {
  size_t size = body ? strlen(body) : 0u;
  reply(response, status, "text/plain; charset=utf-8", body, size);
}

static inline void send_json(Res *response, int status, const char *body) {
  size_t size = body ? strlen(body) : 0u;
  reply(response, status, "application/json", body, size);
}

#ifdef __cplusplus
}
#endif

#endif
