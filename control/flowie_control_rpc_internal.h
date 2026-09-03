#ifndef FLOWIE_CONTROL_RPC_INTERNAL_H
#define FLOWIE_CONTROL_RPC_INTERNAL_H

#include "flowie_control_http_server_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum rpc_protocol_e {
  RPC_PROTOCOL_JSON = 0
} rpc_protocol_t;

enum {
  RPC_ERROR_PARSE = -32700,
  RPC_ERROR_INVALID_REQUEST = -32600,
  RPC_ERROR_METHOD_NOT_FOUND = -32601,
  RPC_ERROR_INVALID_PARAMS = -32602,
  RPC_ERROR_INTERNAL = -32603
};

typedef struct rpc_request_s {
  mem_pool_t *arena;
  char *jsonrpc;
  char *method;
  char *params;
  char *id;
  rpc_protocol_t protocol;
} rpc_request_t;

typedef struct rpc_response_s {
  mem_pool_t *arena;
  char *jsonrpc;
  char *result;
  char *error_message;
  int error_code;
  char *id;
  rpc_protocol_t protocol;
  size_t max_response_size;
} rpc_response_t;

typedef int (*rpc_method_handler_t)(Req *request, Res *response, rpc_request_t *rpc_request,
                                    rpc_response_t *rpc_response);

typedef struct rpc_method_s {
  const char *name;
  rpc_method_handler_t handler;
  const char *description;
  int requires_auth;
} rpc_method_t;

typedef struct rpc_config_s {
  const char *endpoint;
  rpc_protocol_t default_protocol;
  int enable_introspection;
  int enable_batch;
  int max_batch_size;
  size_t max_request_size;
  size_t max_response_size;
} rpc_config_t;

typedef struct rpc_context_s {
  rpc_config_t config;
  rpc_method_t *methods;
  size_t method_count;
  size_t method_capacity;
} rpc_context_t;

#define RPC_DEFAULT_CONFIG()                                                                       \
  {.endpoint = "/rpc",                                                                            \
   .default_protocol = RPC_PROTOCOL_JSON,                                                         \
   .enable_introspection = 1,                                                                     \
   .enable_batch = 1,                                                                             \
   .max_batch_size = 10,                                                                          \
   .max_request_size = 1024u * 1024u}

rpc_context_t *rpc_init(const rpc_config_t *config);
void rpc_destroy(rpc_context_t *context);
int rpc_register_method(rpc_context_t *context, const rpc_method_t *method);
int rpc_unregister_method(rpc_context_t *context, const char *method_name);
int rpc_parse_request(Req *request, rpc_request_t *rpc_request);
int rpc_build_response(rpc_response_t *rpc_response, char **output, size_t *output_size);
void rpc_send_response(Res *response, rpc_response_t *rpc_response);
void rpc_set_result(rpc_response_t *rpc_response, const char *result);
void rpc_set_error(rpc_response_t *rpc_response, int error_code, const char *error_message);

#ifdef __cplusplus
}
#endif

#endif
