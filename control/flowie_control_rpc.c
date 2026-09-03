#include "flowie_control_rpc_internal.h"

#include <json_parser.h>

#include <stdlib.h>
#include <string.h>

enum {
  FLOWIE_CONTROL_RPC_INITIAL_METHOD_CAPACITY = 64
};

static char *flowie_control_rpc_duplicate(const char *value) {
  size_t size;
  char *copy;
  if (!value) return NULL;
  size = strlen(value);
  copy = (char *)malloc(size + 1u);
  if (copy) memcpy(copy, value, size + 1u);
  return copy;
}

rpc_context_t *rpc_init(const rpc_config_t *config) {
  rpc_context_t *context;
  if (!config || !config->endpoint || config->endpoint[0] != '/' ||
      config->default_protocol != RPC_PROTOCOL_JSON || config->max_request_size == 0u)
    return NULL;
  context = (rpc_context_t *)calloc(1u, sizeof(*context));
  if (!context) return NULL;
  context->config = *config;
  context->config.endpoint = flowie_control_rpc_duplicate(config->endpoint);
  context->method_capacity = FLOWIE_CONTROL_RPC_INITIAL_METHOD_CAPACITY;
  context->methods = (rpc_method_t *)calloc(context->method_capacity, sizeof(*context->methods));
  if (!context->config.endpoint || !context->methods) {
    rpc_destroy(context);
    return NULL;
  }
  return context;
}

void rpc_destroy(rpc_context_t *context) {
  if (!context) return;
  for (size_t index = 0u; index < context->method_count; ++index) {
    free((void *)context->methods[index].name);
    free((void *)context->methods[index].description);
  }
  free(context->methods);
  free((void *)context->config.endpoint);
  memset(context, 0, sizeof(*context));
  free(context);
}

int rpc_register_method(rpc_context_t *context, const rpc_method_t *method) {
  char *name;
  char *description = NULL;
  if (!context || !method || !method->name || !method->name[0] || !method->handler ||
      context->method_count == context->method_capacity)
    return -1;
  for (size_t index = 0u; index < context->method_count; ++index)
    if (strcmp(context->methods[index].name, method->name) == 0) return -1;
  name = flowie_control_rpc_duplicate(method->name);
  if (method->description) description = flowie_control_rpc_duplicate(method->description);
  if (!name || (method->description && !description)) {
    free(name);
    free(description);
    return -1;
  }
  context->methods[context->method_count] = *method;
  context->methods[context->method_count].name = name;
  context->methods[context->method_count].description = description;
  ++context->method_count;
  return 0;
}

int rpc_unregister_method(rpc_context_t *context, const char *method_name) {
  if (!context || !method_name) return -1;
  for (size_t index = 0u; index < context->method_count; ++index) {
    if (strcmp(context->methods[index].name, method_name) != 0) continue;
    free((void *)context->methods[index].name);
    free((void *)context->methods[index].description);
    if (index + 1u < context->method_count)
      memmove(&context->methods[index], &context->methods[index + 1u],
              (context->method_count - index - 1u) * sizeof(*context->methods));
    --context->method_count;
    context->methods[context->method_count] = (rpc_method_t){0};
    return 0;
  }
  return -1;
}

static size_t flowie_control_rpc_member_count(const json_value_t *object, const char *name) {
  size_t count = 0u;
  if (!object || json_type(object) != JSON_OBJECT || !name) return 0u;
  for (size_t index = 0u; index < json_object_size(object); ++index)
    if (strcmp(json_object_key(object, index), name) == 0) ++count;
  return count;
}

int rpc_parse_request(Req *request, rpc_request_t *rpc_request) {
  json_value_t *root;
  json_value_t *version;
  json_value_t *method;
  json_value_t *params;
  json_value_t *id;
  char *serialized;
  size_t serialized_size = 0u;
  int rc = RPC_ERROR_INVALID_REQUEST;
  if (!request || !rpc_request || !request->arena || !request->body || request->body_len == 0u)
    return RPC_ERROR_INVALID_REQUEST;
  memset(rpc_request, 0, sizeof(*rpc_request));
  rpc_request->arena = request->arena;
  rpc_request->protocol = RPC_PROTOCOL_JSON;
  root = json_parse(request->body, request->body_len);
  if (!root) return RPC_ERROR_PARSE;
  if (json_type(root) != JSON_OBJECT || flowie_control_rpc_member_count(root, "jsonrpc") != 1u ||
      flowie_control_rpc_member_count(root, "method") != 1u ||
      flowie_control_rpc_member_count(root, "params") > 1u ||
      flowie_control_rpc_member_count(root, "id") > 1u)
    goto done;
  version = json_object_get(root, "jsonrpc");
  method = json_object_get(root, "method");
  params = json_object_get(root, "params");
  id = json_object_get(root, "id");
  if (!version || json_type(version) != JSON_STRING || strcmp(json_string(version), "2.0") != 0 ||
      !method || json_type(method) != JSON_STRING || json_string_len(method) == 0u)
    goto done;
  rpc_request->jsonrpc = mem_strdup(request->arena, "2.0");
  rpc_request->method = mem_strdup(request->arena, json_string(method));
  if (!rpc_request->jsonrpc || !rpc_request->method) {
    rc = RPC_ERROR_INTERNAL;
    goto done;
  }
  if (params) {
    if (json_type(params) != JSON_OBJECT && json_type(params) != JSON_ARRAY) {
      rc = RPC_ERROR_INVALID_PARAMS;
      goto done;
    }
    serialized = json_serialize(params, &serialized_size);
    if (!serialized) {
      rc = RPC_ERROR_INTERNAL;
      goto done;
    }
    rpc_request->params = mem_strdup(request->arena, serialized);
    json_serialize_free(serialized);
    if (!rpc_request->params) {
      rc = RPC_ERROR_INTERNAL;
      goto done;
    }
  }
  if (id) {
    if (json_type(id) != JSON_STRING && json_type(id) != JSON_NUMBER && !json_is_null(id))
      goto done;
    serialized = json_serialize(id, &serialized_size);
    if (!serialized) {
      rc = RPC_ERROR_INTERNAL;
      goto done;
    }
    rpc_request->id = mem_strdup(request->arena, serialized);
    json_serialize_free(serialized);
    if (!rpc_request->id) {
      rc = RPC_ERROR_INTERNAL;
      goto done;
    }
  }
  rc = 0;

done:
  json_free(root);
  return rc;
}

static int flowie_control_rpc_add(json_value_t *object, const char *key, json_value_t *value) {
  if (!object || !key || !value) {
    json_free(value);
    return -1;
  }
  if (!json_object_add_checked(object, key, value)) {
    json_free(value);
    return -1;
  }
  return 0;
}

int rpc_build_response(rpc_response_t *rpc_response, char **output, size_t *output_size) {
  json_value_t *root = NULL;
  json_value_t *payload = NULL;
  json_value_t *id = NULL;
  char *serialized = NULL;
  size_t size = 0u;
  if (output) *output = NULL;
  if (output_size) *output_size = 0u;
  if (!rpc_response || !rpc_response->arena || !output || !output_size) return -1;
  root = json_create_object();
  if (!root || flowie_control_rpc_add(root, "jsonrpc", json_create_string("2.0")) != 0)
    goto fail;
  if (rpc_response->error_code != 0) {
    json_value_t *owned;
    payload = json_create_object();
    if (!payload ||
        flowie_control_rpc_add(payload, "code", json_create_int64(rpc_response->error_code)) != 0 ||
        flowie_control_rpc_add(payload, "message",
                               json_create_string(rpc_response->error_message
                                                      ? rpc_response->error_message
                                                      : "Unknown error")) != 0)
      goto fail;
    owned = payload;
    payload = NULL;
    if (flowie_control_rpc_add(root, "error", owned) != 0) goto fail;
  } else {
    json_value_t *owned;
    payload = rpc_response->result ? json_parse(rpc_response->result, strlen(rpc_response->result))
                                   : json_create_null();
    if (!payload) goto fail;
    owned = payload;
    payload = NULL;
    if (flowie_control_rpc_add(root, "result", owned) != 0) goto fail;
  }
  id = rpc_response->id ? json_parse(rpc_response->id, strlen(rpc_response->id))
                        : json_create_null();
  if (!id) goto fail;
  {
    json_value_t *owned = id;
    id = NULL;
    if (flowie_control_rpc_add(root, "id", owned) != 0) goto fail;
  }
  serialized = json_serialize(root, &size);
  if (!serialized ||
      (rpc_response->max_response_size != 0u && size > rpc_response->max_response_size))
    goto fail;
  *output = mem_strdup(rpc_response->arena, serialized);
  if (!*output) goto fail;
  *output_size = size;
  json_serialize_free(serialized);
  json_free(root);
  return 0;

fail:
  json_serialize_free(serialized);
  json_free(id);
  json_free(payload);
  json_free(root);
  return -1;
}

void rpc_send_response(Res *response, rpc_response_t *rpc_response) {
  static const char internal_error[] =
      "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":\"Internal error\"},\"id\":null}";
  char *body = NULL;
  size_t body_size = 0u;
  if (!response || !rpc_response) return;
  if (rpc_build_response(rpc_response, &body, &body_size) == 0)
    reply(response, OK, "application/json", body, body_size);
  else
    reply(response, INTERNAL_SERVER_ERROR, "application/json", internal_error,
          sizeof(internal_error) - 1u);
}

void rpc_set_result(rpc_response_t *rpc_response, const char *result) {
  if (!rpc_response || !rpc_response->arena || !result) return;
  rpc_response->result = mem_strdup(rpc_response->arena, result);
  if (!rpc_response->result) {
    rpc_response->error_code = RPC_ERROR_INTERNAL;
    rpc_response->error_message = "Out of memory";
    return;
  }
  rpc_response->error_code = 0;
  rpc_response->error_message = NULL;
}

void rpc_set_error(rpc_response_t *rpc_response, int error_code, const char *error_message) {
  if (!rpc_response || !rpc_response->arena) return;
  rpc_response->error_code = error_code;
  rpc_response->error_message =
      mem_strdup(rpc_response->arena, error_message ? error_message : "Unknown error");
  if (!rpc_response->error_message) {
    rpc_response->error_code = RPC_ERROR_INTERNAL;
    rpc_response->error_message = "Out of memory";
  }
  rpc_response->result = NULL;
}
