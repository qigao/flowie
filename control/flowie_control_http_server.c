#include "flowie_control_http_server_internal.h"

#include <chttp/chttp.h>

#include "salts_error.h"
#include "salts_thread.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  FLOWIE_CONTROL_HTTP_ROUTE_CAPACITY = 32,
  FLOWIE_CONTROL_HTTP_CONTEXT_CAPACITY = 32,
  FLOWIE_CONTROL_HTTP_DEFAULT_WORKERS = 4,
  FLOWIE_CONTROL_HTTP_DEFAULT_QUEUE_CAPACITY = 128,
  FLOWIE_CONTROL_HTTP_CONNECTION_CAPACITY = 128,
  FLOWIE_CONTROL_HTTP_COMMAND_CAPACITY = 256,
  FLOWIE_CONTROL_HTTP_COMMAND_BUFFER_CAPACITY = 64 * 1024 * 1024,
  FLOWIE_CONTROL_HTTP_REQUEST_CAPACITY = 256,
  FLOWIE_CONTROL_HTTP_COMPLETION_BATCH_CAPACITY = 64,
  FLOWIE_CONTROL_HTTP_EVENT_CAPACITY = 256,
  FLOWIE_CONTROL_HTTP_RESPONSE_HEADER_CAPACITY = 32,
  FLOWIE_CONTROL_HTTP_RESPONSE_BODY_CAPACITY = 16 * 1024 * 1024,
  FLOWIE_CONTROL_HTTP_IO_OVERHEAD_CAPACITY = 64 * 1024,
  FLOWIE_CONTROL_HTTP_RESPONSE_WIRE_OVERHEAD = 512,
  FLOWIE_CONTROL_HTTP_QUERY_CAPACITY = 16,
  FLOWIE_CONTROL_HTTP_POLL_SLICE_MS = 10,
  FLOWIE_CONTROL_HTTP_IO_TIMEOUT_MS = 30000,
  FLOWIE_CONTROL_HTTP_STOP_TIMEOUT_MS = 30000,
  FLOWIE_CONTROL_HTTP_REMOTE_ADDRESS_CAPACITY = 96
};

static native_io_backend_kind flowie_control_http_native_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

typedef struct flowie_control_http_route_s {
  flowie_control_http_app_t *app;
  char *path;
  chttp_method method;
  flowie_control_http_handler_fn handler;
} flowie_control_http_route_t;

typedef struct flowie_control_http_context_binding_s {
  char *path;
  void *context;
} flowie_control_http_context_binding_t;

typedef struct flowie_control_http_job_s {
  flowie_control_http_app_t *app;
  flowie_control_http_route_t *route;
  chttp_server_deferred deferred;
  char *method;
  char *path;
  char *target;
  char *body;
  size_t body_size;
  request_item_t *headers;
  size_t header_count;
  char *peer_certificate_sha256;
  char remote_address[FLOWIE_CONTROL_HTTP_REMOTE_ADDRESS_CAPACITY];
} flowie_control_http_job_t;

struct flowie_control_http_app_s {
  chttp_server server;
  salts_threadpool_t *workers;
  flowie_control_http_route_t routes[FLOWIE_CONTROL_HTTP_ROUTE_CAPACITY];
  flowie_control_http_context_binding_t contexts[FLOWIE_CONTROL_HTTP_CONTEXT_CAPACITY];
  size_t route_count;
  size_t context_count;
  flowie_control_http_middleware_fn middleware;
  flowie_control_http_limits_t limits;
  size_t worker_count;
  size_t worker_queue_capacity;
  int configured;
  int initialized;
  int started;
};

static char *flowie_control_http_duplicate(const char *value) {
  size_t size;
  char *copy;
  if (!value) return NULL;
  size = strlen(value);
  copy = (char *)malloc(size + 1u);
  if (copy) memcpy(copy, value, size + 1u);
  return copy;
}

static int flowie_control_http_text_valid(const char *value) {
  return value && !strchr(value, '\r') && !strchr(value, '\n');
}

static int flowie_control_http_ascii_equal(const char *left, const char *right) {
  if (!left || !right) return 0;
  while (*left && *right) {
    if (tolower((unsigned char)*left++) != tolower((unsigned char)*right++)) return 0;
  }
  return *left == '\0' && *right == '\0';
}

static void flowie_control_http_job_destroy(flowie_control_http_job_t *job) {
  if (!job) return;
  for (size_t index = 0u; index < job->header_count; ++index) {
    free(job->headers[index].key);
    free(job->headers[index].value);
  }
  free(job->headers);
  free(job->peer_certificate_sha256);
  free(job->body);
  free(job->target);
  free(job->path);
  free(job->method);
  memset(job, 0, sizeof(*job));
  free(job);
}

static const char *flowie_control_http_method_name(chttp_method method) {
  switch (method) {
  case CHTTP_METHOD_GET:
    return "GET";
  case CHTTP_METHOD_HEAD:
    return "HEAD";
  case CHTTP_METHOD_POST:
    return "POST";
  case CHTTP_METHOD_PUT:
    return "PUT";
  case CHTTP_METHOD_DELETE:
    return "DELETE";
  case CHTTP_METHOD_PATCH:
    return "PATCH";
  case CHTTP_METHOD_OPTIONS:
    return "OPTIONS";
  default:
    return NULL;
  }
}

static void flowie_control_http_peer_format(const cnet_stream_peer *peer, char *output,
                                            size_t capacity) {
  int written = -1;
  if (!output || capacity == 0u) return;
  output[0] = '\0';
  if (!peer) {
    (void)snprintf(output, capacity, "unknown");
    return;
  }
  if (peer->family == CNET_DATAGRAM_ADDRESS_IPV4) {
    written = snprintf(output, capacity, "%u.%u.%u.%u:%u", (unsigned int)peer->address[0],
                       (unsigned int)peer->address[1], (unsigned int)peer->address[2],
                       (unsigned int)peer->address[3], (unsigned int)peer->port);
  } else if (peer->family == CNET_DATAGRAM_ADDRESS_IPV6) {
    written = snprintf(output, capacity, "[%x:%x:%x:%x:%x:%x:%x:%x]:%u",
                       ((unsigned int)peer->address[0] << 8u) | peer->address[1],
                       ((unsigned int)peer->address[2] << 8u) | peer->address[3],
                       ((unsigned int)peer->address[4] << 8u) | peer->address[5],
                       ((unsigned int)peer->address[6] << 8u) | peer->address[7],
                       ((unsigned int)peer->address[8] << 8u) | peer->address[9],
                       ((unsigned int)peer->address[10] << 8u) | peer->address[11],
                       ((unsigned int)peer->address[12] << 8u) | peer->address[13],
                       ((unsigned int)peer->address[14] << 8u) | peer->address[15],
                       (unsigned int)peer->port);
  }
  if (written < 0 || (size_t)written >= capacity) output[0] = '\0';
}

static flowie_control_http_job_t *
flowie_control_http_job_create(flowie_control_http_app_t *app, flowie_control_http_route_t *route,
                               const chttp_server_request_view *request) {
  const char *method = flowie_control_http_method_name(request ? request->method : 0);
  flowie_control_http_job_t *job;
  if (!app || !route || !request || !method ||
      request->header_count > app->limits.max_headers_count)
    return NULL;
  job = (flowie_control_http_job_t *)calloc(1u, sizeof(*job));
  if (!job) return NULL;
  job->deferred = (chttp_server_deferred)CHTTP_SERVER_DEFERRED_INIT;
  job->app = app;
  job->route = route;
  job->method = flowie_control_http_duplicate(method);
  job->path = flowie_control_http_duplicate(request->path);
  job->target = flowie_control_http_duplicate(request->target);
  if (!job->method || !job->path || !job->target) goto fail;
  if (request->body_size > 0u) {
    job->body = (char *)malloc(request->body_size + 1u);
    if (!job->body) goto fail;
    memcpy(job->body, request->body, request->body_size);
    job->body[request->body_size] = '\0';
    job->body_size = request->body_size;
  }
  if (request->header_count > 0u) {
    job->headers = (request_item_t *)calloc(request->header_count, sizeof(*job->headers));
    if (!job->headers) goto fail;
    for (size_t index = 0u; index < request->header_count; ++index) {
      job->headers[index].key = flowie_control_http_duplicate(request->headers[index].name);
      job->headers[index].value = flowie_control_http_duplicate(request->headers[index].value);
      ++job->header_count;
      if (!job->headers[index].key || !job->headers[index].value) goto fail;
    }
  }
  if (request->peer_certificate_sha256) {
    size_t digest_size = strlen(request->peer_certificate_sha256);
    job->peer_certificate_sha256 = (char *)malloc(sizeof("sha256:") - 1u + digest_size + 1u);
    if (!job->peer_certificate_sha256) goto fail;
    memcpy(job->peer_certificate_sha256, "sha256:", sizeof("sha256:") - 1u);
    memcpy(job->peer_certificate_sha256 + sizeof("sha256:") - 1u, request->peer_certificate_sha256,
           digest_size + 1u);
  }
  flowie_control_http_peer_format(request->peer, job->remote_address, sizeof(job->remote_address));
  return job;

fail:
  flowie_control_http_job_destroy(job);
  return NULL;
}

static int flowie_control_http_hex_value(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

static int flowie_control_http_query_decode(const char *begin, size_t size, char **out) {
  char *value;
  size_t write = 0u;
  if (out) *out = NULL;
  if (!begin || !out) return SALTS_EINVAL;
  value = (char *)malloc(size + 1u);
  if (!value) return SALTS_ENOMEM;
  for (size_t read = 0u; read < size; ++read) {
    if (begin[read] == '%') {
      int high;
      int low;
      if (read + 2u >= size || (high = flowie_control_http_hex_value(begin[read + 1u])) < 0 ||
          (low = flowie_control_http_hex_value(begin[read + 2u])) < 0 || (high == 0 && low == 0)) {
        free(value);
        return SALTS_EPROTO;
      }
      value[write++] = (char)((high << 4) | low);
      read += 2u;
    } else {
      value[write++] = begin[read] == '+' ? ' ' : begin[read];
    }
  }
  value[write] = '\0';
  *out = value;
  return SALTS_OK;
}

char *flowie_control_http_url_decode(const char *value) {
  char *decoded = NULL;
  if (!value || flowie_control_http_query_decode(value, strlen(value), &decoded) != SALTS_OK)
    return NULL;
  return decoded;
}

char *flowie_control_http_url_encode(const char *value) {
  static const char hex[] = "0123456789ABCDEF";
  size_t size;
  size_t capacity;
  size_t write = 0u;
  char *encoded;
  if (!value) return NULL;
  size = strlen(value);
  if (size > (SIZE_MAX - 1u) / 3u) return NULL;
  capacity = size * 3u + 1u;
  encoded = (char *)malloc(capacity);
  if (!encoded) return NULL;
  for (size_t read = 0u; read < size; ++read) {
    unsigned char byte = (unsigned char)value[read];
    if (isalnum(byte) || byte == '-' || byte == '.' || byte == '_' || byte == '~') {
      encoded[write++] = (char)byte;
    } else {
      encoded[write++] = '%';
      encoded[write++] = hex[byte >> 4u];
      encoded[write++] = hex[byte & 0x0fu];
    }
  }
  encoded[write] = '\0';
  return encoded;
}

static void flowie_control_http_query_clear(request_t *query) {
  if (!query) return;
  for (int index = 0; index < query->count; ++index) {
    free(query->items[index].key);
    free(query->items[index].value);
  }
  free(query->items);
  *query = (request_t){0};
}

static int flowie_control_http_query_parse(const char *target, request_t *query) {
  const char *cursor;
  size_t count = 0u;
  if (!target || !query) return SALTS_EINVAL;
  *query = (request_t){0};
  cursor = strchr(target, '?');
  if (!cursor || !cursor[1]) return SALTS_OK;
  ++cursor;
  for (const char *scan = cursor; *scan; ++scan)
    if (*scan == '&') ++count;
  ++count;
  if (count > FLOWIE_CONTROL_HTTP_QUERY_CAPACITY || count > (size_t)INT_MAX) return SALTS_EPROTO;
  query->items = (request_item_t *)calloc(count, sizeof(*query->items));
  if (!query->items) return SALTS_ENOMEM;
  query->capacity = (int)count;
  while (*cursor) {
    const char *end = strchr(cursor, '&');
    const char *equals;
    int rc;
    if (!end) end = cursor + strlen(cursor);
    equals = (const char *)memchr(cursor, '=', (size_t)(end - cursor));
    if (!equals || equals == cursor) {
      flowie_control_http_query_clear(query);
      return SALTS_EPROTO;
    }
    rc = flowie_control_http_query_decode(cursor, (size_t)(equals - cursor),
                                          &query->items[query->count].key);
    if (rc == SALTS_OK)
      rc = flowie_control_http_query_decode(equals + 1u, (size_t)(end - equals - 1u),
                                            &query->items[query->count].value);
    if (rc != SALTS_OK) {
      flowie_control_http_query_clear(query);
      return rc;
    }
    ++query->count;
    cursor = *end == '&' ? end + 1u : end;
  }
  return SALTS_OK;
}

void flowie_control_http_response_clear(Res *response) {
  if (!response) return;
  for (size_t index = 0u; index < response->header_count; ++index) {
    free(response->headers[index].name);
    free(response->headers[index].value);
  }
  free(response->headers);
  free(response->content_type);
  free(response->body);
  memset(response, 0, sizeof(*response));
}

void set_header(Res *response, const char *name, const char *value) {
  flowie_control_http_header_t *headers;
  if (!response || response->completed || !flowie_control_http_text_valid(name) ||
      !flowie_control_http_text_valid(value)) {
    if (response) response->error = SALTS_EINVAL;
    return;
  }
  for (size_t index = 0u; index < response->header_count; ++index) {
    if (flowie_control_http_ascii_equal(response->headers[index].name, name)) {
      char *copy = flowie_control_http_duplicate(value);
      if (!copy) {
        response->error = SALTS_ENOMEM;
        return;
      }
      free(response->headers[index].value);
      response->headers[index].value = copy;
      return;
    }
  }
  if (response->header_count == FLOWIE_CONTROL_HTTP_RESPONSE_HEADER_CAPACITY) {
    response->error = SALTS_ENOBUFS;
    return;
  }
  if (response->header_count == response->header_capacity) {
    size_t capacity = response->header_capacity ? response->header_capacity * 2u : 8u;
    if (capacity > FLOWIE_CONTROL_HTTP_RESPONSE_HEADER_CAPACITY)
      capacity = FLOWIE_CONTROL_HTTP_RESPONSE_HEADER_CAPACITY;
    headers =
        (flowie_control_http_header_t *)realloc(response->headers, capacity * sizeof(*headers));
    if (!headers) {
      response->error = SALTS_ENOMEM;
      return;
    }
    response->headers = headers;
    response->header_capacity = capacity;
  }
  response->headers[response->header_count].name = flowie_control_http_duplicate(name);
  response->headers[response->header_count].value = flowie_control_http_duplicate(value);
  if (!response->headers[response->header_count].name ||
      !response->headers[response->header_count].value) {
    free(response->headers[response->header_count].name);
    free(response->headers[response->header_count].value);
    response->headers[response->header_count] = (flowie_control_http_header_t){0};
    response->error = SALTS_ENOMEM;
    return;
  }
  ++response->header_count;
}

void reply(Res *response, int status, const char *content_type, const void *body, size_t body_len) {
  char *content_type_copy;
  void *body_copy = NULL;
  if (!response || response->completed || status < 100 || status > 599 ||
      !flowie_control_http_text_valid(content_type) || (body_len > 0u && !body)) {
    if (response) response->error = SALTS_EINVAL;
    return;
  }
  if (body_len > FLOWIE_CONTROL_HTTP_RESPONSE_BODY_CAPACITY) {
    response->error = SALTS_EMSGSIZE;
    return;
  }
  content_type_copy = flowie_control_http_duplicate(content_type);
  if (!content_type_copy) {
    response->error = SALTS_ENOMEM;
    return;
  }
  if (body_len > 0u) {
    body_copy = malloc(body_len + 1u);
    if (!body_copy) {
      free(content_type_copy);
      response->error = SALTS_ENOMEM;
      return;
    }
    memcpy(body_copy, body, body_len);
    ((unsigned char *)body_copy)[body_len] = 0u;
  }
  response->status = (unsigned int)status;
  response->content_type = content_type_copy;
  response->body = body_copy;
  response->body_len = body_len;
  response->completed = 1;
}

void set_cookie(Res *response, const char *name, const char *value, cookie_options_t *options) {
  char buffer[1024];
  int written;
  if (!response || !flowie_control_http_text_valid(name) ||
      !flowie_control_http_text_valid(value) || !options ||
      !flowie_control_http_text_valid(options->path ? options->path : "/") ||
      !flowie_control_http_text_valid(options->same_site ? options->same_site : "Strict") ||
      strchr(name, ';') || strchr(name, '=') || strchr(value, ';')) {
    if (response) response->error = SALTS_EINVAL;
    return;
  }
  written = snprintf(buffer, sizeof(buffer), "%s=%s; Path=%s; Max-Age=%d; SameSite=%s%s%s", name,
                     value, options->path ? options->path : "/", options->max_age,
                     options->same_site ? options->same_site : "Strict",
                     options->http_only ? "; HttpOnly" : "", options->secure ? "; Secure" : "");
  if (written < 0 || (size_t)written >= sizeof(buffer)) {
    response->error = SALTS_EMSGSIZE;
    return;
  }
  set_header(response, "Set-Cookie", buffer);
}

void set_context(Req *request, void *data, size_t size, void (*cleanup)(void *)) {
  if (!request || request->context.data) return;
  request->context.data = data;
  request->context.size = size;
  request->context.cleanup = cleanup;
}

void *get_context(Req *request) { return request ? request->context.data : NULL; }

int next(Chain *chain, Req *request, Res *response) {
  if (!chain || !request || !response || !chain->terminal || chain->called) return 1;
  chain->called = 1;
  chain->terminal(request, response);
  return 1;
}

static void flowie_control_http_job_reply(flowie_control_http_job_t *job, Res *response) {
  static const char internal_error[] = "Internal server error";
  chttp_header *headers = NULL;
  chttp_server_deferred_response deferred = {0};
  int rc;
  if (!job || !response) return;
  if (!response->completed || response->error != SALTS_OK) {
    flowie_control_http_response_clear(response);
    reply(response, INTERNAL_SERVER_ERROR, "text/plain; charset=utf-8", internal_error,
          sizeof(internal_error) - 1u);
  }
  if (response->header_count > 0u) {
    headers = (chttp_header *)calloc(response->header_count, sizeof(*headers));
    if (!headers) {
      flowie_control_http_response_clear(response);
      reply(response, INTERNAL_SERVER_ERROR, "text/plain; charset=utf-8", internal_error,
            sizeof(internal_error) - 1u);
    } else {
      for (size_t index = 0u; index < response->header_count; ++index) {
        headers[index].name = response->headers[index].name;
        headers[index].value = response->headers[index].value;
      }
    }
  }
  deferred.size = sizeof(deferred);
  deferred.status_code = response->status;
  deferred.content_type = response->content_type;
  deferred.headers = headers;
  deferred.header_count = headers ? response->header_count : 0u;
  deferred.body = response->body;
  deferred.body_size = response->body_len;
  rc = chttp_server_deferred_reply(&job->deferred, &deferred);
  if (rc != SALTS_OK && job->deferred.impl) {
    deferred.status_code = INTERNAL_SERVER_ERROR;
    deferred.content_type = "text/plain; charset=utf-8";
    deferred.headers = NULL;
    deferred.header_count = 0u;
    deferred.body = internal_error;
    deferred.body_size = sizeof(internal_error) - 1u;
    (void)chttp_server_deferred_reply(&job->deferred, &deferred);
  }
  free(headers);
}

static void flowie_control_http_job_run(void *arg) {
  flowie_control_http_job_t *job = (flowie_control_http_job_t *)arg;
  flowie_control_http_security_context_t security = {0};
  mem_pool_t arena;
  Req request = {0};
  Res response = {0};
  Chain chain;
  int arena_initialized = 0;
  if (!job || !job->app || !job->route || !job->route->handler) goto done;
  memset(&arena, 0, sizeof(arena));
  if (mem_init(&arena, 0u) != 0) {
    response.error = SALTS_ENOMEM;
    flowie_control_http_job_reply(job, &response);
    goto done;
  }
  arena_initialized = 1;
  request.app = job->app;
  request.arena = &arena;
  request.client = job;
  request.method = job->method;
  request.path = job->path;
  request.body = job->body;
  request.body_len = job->body_size;
  request.headers.items = job->headers;
  request.headers.count = (int)job->header_count;
  request.headers.capacity = (int)job->header_count;
  request.security = &security;
  request.peer_certificate_sha256 = job->peer_certificate_sha256;
  request.remote_address = job->remote_address;
  if (flowie_control_http_query_parse(job->target, &request.query) != SALTS_OK) {
    reply(&response, BAD_REQUEST, "text/plain; charset=utf-8", "Invalid query", 13u);
  } else {
    chain = (Chain){job->route->handler, 0};
    if (job->app->middleware) (void)job->app->middleware(&request, &response, &chain);
    else (void)next(&chain, &request, &response);
  }
  if (!response.completed && response.error == SALTS_OK)
    reply(&response, NO_CONTENT, "text/plain; charset=utf-8", NULL, 0u);
  flowie_control_http_job_reply(job, &response);

done:
  if (request.context.cleanup && request.context.data)
    request.context.cleanup(request.context.data);
  flowie_control_http_query_clear(&request.query);
  flowie_control_http_response_clear(&response);
  if (arena_initialized) mem_destroy(&arena);
  flowie_control_http_job_destroy(job);
}

static int flowie_control_http_chttp_handler(void *user, const chttp_server_request_view *request,
                                             chttp_server_response *response) {
  static const char unavailable[] = "Service unavailable";
  flowie_control_http_route_t *route = (flowie_control_http_route_t *)user;
  flowie_control_http_app_t *app;
  flowie_control_http_job_t *job;
  chttp_server_deferred_response deferred;
  int rc;
  if (!route || !request || !response) return SALTS_EINVAL;
  app = route->app;
  if (!app || !app->workers) return SALTS_EINVAL;
  job = flowie_control_http_job_create(app, route, request);
  if (!job)
    return chttp_server_reply(response, SERVICE_UNAVAILABLE, "text/plain; charset=utf-8",
                              unavailable, sizeof(unavailable) - 1u);
  rc = chttp_server_response_defer(response, &job->deferred);
  if (rc != SALTS_OK) {
    flowie_control_http_job_destroy(job);
    return rc;
  }
  rc = salts_threadpool_try_submit(app->workers, flowie_control_http_job_run, job);
  if (rc == SALTS_OK) return SALTS_OK;
  deferred = (chttp_server_deferred_response){
      sizeof(deferred), SERVICE_UNAVAILABLE,     "text/plain; charset=utf-8", NULL, 0u,
      unavailable,      sizeof(unavailable) - 1u};
  (void)chttp_server_deferred_reply(&job->deferred, &deferred);
  flowie_control_http_job_destroy(job);
  return SALTS_OK;
}

flowie_control_http_app_t *flowie_control_http_app_create(void) {
  flowie_control_http_app_t *app =
      (flowie_control_http_app_t *)calloc(1u, sizeof(flowie_control_http_app_t));
  if (app) {
    app->worker_count = FLOWIE_CONTROL_HTTP_DEFAULT_WORKERS;
    app->worker_queue_capacity = FLOWIE_CONTROL_HTTP_DEFAULT_QUEUE_CAPACITY;
  }
  return app;
}

int flowie_control_http_app_configure(flowie_control_http_app_t *app,
                                      const flowie_control_http_limits_t *limits,
                                      size_t worker_count, size_t worker_queue_capacity) {
  if (!app || !limits || app->initialized || worker_count == 0u || worker_count > INT_MAX ||
      worker_queue_capacity == 0u || limits->max_header_name_length == 0u ||
      limits->max_header_value_length == 0u || limits->max_url_length == 0u ||
      limits->max_request_body_size == 0u || limits->max_headers_count == 0u ||
      limits->max_headers_count > INT_MAX || limits->max_header_value_length > SIZE_MAX - 4u ||
      limits->max_header_name_length > SIZE_MAX - limits->max_header_value_length - 4u ||
      limits->max_headers_count >
          SIZE_MAX / (limits->max_header_name_length + limits->max_header_value_length + 4u))
    return SALTS_EINVAL;
  app->limits = *limits;
  app->worker_count = worker_count;
  app->worker_queue_capacity = worker_queue_capacity;
  app->configured = 1;
  return SALTS_OK;
}

int flowie_control_http_app_bind_context(flowie_control_http_app_t *app, const char *path,
                                         void *context) {
  if (!app || !path || path[0] != '/' || !context || app->started) return SALTS_EINVAL;
  if (flowie_control_http_app_lookup_context(app, path)) return SALTS_EALREADY;
  if (app->context_count == FLOWIE_CONTROL_HTTP_CONTEXT_CAPACITY) return SALTS_ENOBUFS;
  app->contexts[app->context_count].path = flowie_control_http_duplicate(path);
  if (!app->contexts[app->context_count].path) return SALTS_ENOMEM;
  app->contexts[app->context_count].context = context;
  ++app->context_count;
  return SALTS_OK;
}

int flowie_control_http_app_unbind_context(flowie_control_http_app_t *app, const char *path,
                                           void *context) {
  if (!app || !path || !context || app->started) return SALTS_EINVAL;
  for (size_t index = 0u; index < app->context_count; ++index) {
    if (app->contexts[index].context == context && strcmp(app->contexts[index].path, path) == 0) {
      free(app->contexts[index].path);
      app->contexts[index] = app->contexts[app->context_count - 1u];
      app->contexts[app->context_count - 1u] = (flowie_control_http_context_binding_t){0};
      --app->context_count;
      return SALTS_OK;
    }
  }
  return SALTS_ENOENT;
}

void *flowie_control_http_app_lookup_context(flowie_control_http_app_t *app, const char *path) {
  if (!app || !path) return NULL;
  for (size_t index = 0u; index < app->context_count; ++index)
    if (strcmp(app->contexts[index].path, path) == 0) return app->contexts[index].context;
  return NULL;
}

static int flowie_control_http_app_route(flowie_control_http_app_t *app, chttp_method method,
                                         const char *path, flowie_control_http_handler_fn handler) {
  if (!app || !path || path[0] != '/' || !handler || app->initialized) return SALTS_EINVAL;
  for (size_t index = 0u; index < app->route_count; ++index)
    if (app->routes[index].method == method && strcmp(app->routes[index].path, path) == 0)
      return SALTS_EALREADY;
  if (app->route_count == FLOWIE_CONTROL_HTTP_ROUTE_CAPACITY) return SALTS_ENOBUFS;
  app->routes[app->route_count].app = app;
  app->routes[app->route_count].path = flowie_control_http_duplicate(path);
  if (!app->routes[app->route_count].path) return SALTS_ENOMEM;
  app->routes[app->route_count].method = method;
  app->routes[app->route_count].handler = handler;
  ++app->route_count;
  return SALTS_OK;
}

int flowie_control_http_app_get(flowie_control_http_app_t *app, const char *path,
                                flowie_control_http_handler_fn handler) {
  return flowie_control_http_app_route(app, CHTTP_METHOD_GET, path, handler);
}

int flowie_control_http_app_post(flowie_control_http_app_t *app, const char *path,
                                 flowie_control_http_handler_fn handler) {
  return flowie_control_http_app_route(app, CHTTP_METHOD_POST, path, handler);
}

static int flowie_control_http_app_unroute(flowie_control_http_app_t *app, chttp_method method,
                                           const char *path,
                                           flowie_control_http_handler_fn handler) {
  if (!app || !path || !handler || app->initialized) return SALTS_EINVAL;
  for (size_t index = 0u; index < app->route_count; ++index) {
    flowie_control_http_route_t *route = &app->routes[index];
    if (route->method == method && route->handler == handler && strcmp(route->path, path) == 0) {
      free(route->path);
      *route = app->routes[app->route_count - 1u];
      app->routes[app->route_count - 1u] = (flowie_control_http_route_t){0};
      --app->route_count;
      return SALTS_OK;
    }
  }
  return SALTS_ENOENT;
}

int flowie_control_http_app_unget(flowie_control_http_app_t *app, const char *path,
                                  flowie_control_http_handler_fn handler) {
  return flowie_control_http_app_unroute(app, CHTTP_METHOD_GET, path, handler);
}

int flowie_control_http_app_unpost(flowie_control_http_app_t *app, const char *path,
                                   flowie_control_http_handler_fn handler) {
  return flowie_control_http_app_unroute(app, CHTTP_METHOD_POST, path, handler);
}

int flowie_control_http_app_use(flowie_control_http_app_t *app,
                                flowie_control_http_middleware_fn middleware) {
  if (!app || !middleware || app->middleware || app->initialized) return SALTS_EINVAL;
  app->middleware = middleware;
  return SALTS_OK;
}

int flowie_control_http_tls_validate(const flowie_control_http_tls_config_t *tls) {
  cnet_tls_server server = {0};
  cnet_tls_server_config config = {0};
  int rc;
  if (!tls || tls->size < sizeof(*tls) || !tls->cert_file || !tls->cert_file[0] || !tls->key_file ||
      !tls->key_file[0])
    return SALTS_EINVAL;
  config.size = sizeof(config);
  config.cert_file = tls->cert_file;
  config.key_file = tls->key_file;
  config.key_password = tls->key_password;
  config.ca_file = tls->ca_file;
  config.client_auth =
      tls->client_auth_required ? CNET_TLS_CLIENT_AUTH_REQUIRED : CNET_TLS_CLIENT_AUTH_NONE;
  rc = cnet_tls_server_init(&server, &config);
  if (rc != SALTS_OK) return rc;
  return cnet_tls_server_destroy(&server);
}

int flowie_control_http_app_start_tls(flowie_control_http_app_t *app, const char *host,
                                      uint16_t port, const flowie_control_http_tls_config_t *tls) {
  chttp_server_config config = {0};
  cnet_tls_server_config tls_config = {0};
  salts_threadpool_config_t worker_config;
  int rc;
  if (!app || !host || !host[0] || !tls || tls->size < sizeof(*tls) || !tls->cert_file ||
      !tls->cert_file[0] || !tls->key_file || !tls->key_file[0] || app->started ||
      app->initialized || app->route_count == 0u)
    return SALTS_EINVAL;
  if (!app->configured) {
    app->limits = (flowie_control_http_limits_t){128u, 4096u, 2048u, 65536u, 64u};
  }
  worker_config.num_threads = (int)app->worker_count;
  worker_config.queue_capacity = app->worker_queue_capacity;
  app->workers = salts_threadpool_create_with_config(&worker_config);
  if (!app->workers) return SALTS_ENOMEM;
  tls_config.size = sizeof(tls_config);
  tls_config.cert_file = tls->cert_file;
  tls_config.key_file = tls->key_file;
  tls_config.key_password = tls->key_password;
  tls_config.ca_file = tls->ca_file;
  tls_config.client_auth =
      tls->client_auth_required ? CNET_TLS_CLIENT_AUTH_REQUIRED : CNET_TLS_CLIENT_AUTH_NONE;
  config.host = host;
  config.port = port;
  config.backlog = FLOWIE_CONTROL_HTTP_CONNECTION_CAPACITY;
  config.network.backend = flowie_control_http_native_backend();
  config.network.connection_capacity = FLOWIE_CONTROL_HTTP_CONNECTION_CAPACITY;
  config.network.command_capacity = FLOWIE_CONTROL_HTTP_COMMAND_CAPACITY;
  config.network.command_buffer_bytes = FLOWIE_CONTROL_HTTP_COMMAND_BUFFER_CAPACITY;
  config.network.request_capacity = FLOWIE_CONTROL_HTTP_REQUEST_CAPACITY;
  config.network.completion_batch_capacity = FLOWIE_CONTROL_HTTP_COMPLETION_BATCH_CAPACITY;
  config.network.event_capacity = FLOWIE_CONTROL_HTTP_EVENT_CAPACITY;
  if (app->limits.max_request_body_size > SIZE_MAX - FLOWIE_CONTROL_HTTP_IO_OVERHEAD_CAPACITY) {
    rc = SALTS_ERANGE;
    goto fail;
  }
  config.network.max_send_bytes = FLOWIE_CONTROL_HTTP_RESPONSE_BODY_CAPACITY +
                                  FLOWIE_CONTROL_HTTP_IO_OVERHEAD_CAPACITY +
                                  FLOWIE_CONTROL_HTTP_RESPONSE_WIRE_OVERHEAD;
  config.network.receive_buffer_bytes =
      app->limits.max_request_body_size + FLOWIE_CONTROL_HTTP_IO_OVERHEAD_CAPACITY;
  config.network.connect_timeout_ms = FLOWIE_CONTROL_HTTP_IO_TIMEOUT_MS;
  config.network.read_timeout_ms = FLOWIE_CONTROL_HTTP_IO_TIMEOUT_MS;
  config.network.write_timeout_ms = FLOWIE_CONTROL_HTTP_IO_TIMEOUT_MS;
  config.network.tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES;
  config.network.tls_handshake_timeout_ms = FLOWIE_CONTROL_HTTP_IO_TIMEOUT_MS;
  config.route_capacity = app->route_count;
  config.middleware_capacity = 1u;
  config.max_route_middleware_count = 1u;
  config.max_route_param_count = 1u;
  config.max_route_param_bytes = app->limits.max_url_length;
  config.max_target_bytes = app->limits.max_url_length;
  config.max_header_count = app->limits.max_headers_count;
  config.max_header_bytes =
      app->limits.max_headers_count *
      (app->limits.max_header_name_length + app->limits.max_header_value_length + 4u);
  config.max_request_body_bytes = app->limits.max_request_body_size;
  config.max_response_header_count = FLOWIE_CONTROL_HTTP_RESPONSE_HEADER_CAPACITY;
  config.max_response_header_bytes = FLOWIE_CONTROL_HTTP_IO_OVERHEAD_CAPACITY;
  config.max_response_body_bytes = FLOWIE_CONTROL_HTTP_RESPONSE_BODY_CAPACITY;
  config.poll_slice_ms = FLOWIE_CONTROL_HTTP_POLL_SLICE_MS;
  config.tls = &tls_config;
  config.max_buffered_response_body_bytes = FLOWIE_CONTROL_HTTP_RESPONSE_BODY_CAPACITY;
  rc = chttp_server_init(&app->server, &config);
  if (rc != SALTS_OK) goto fail;
  app->initialized = 1;
  for (size_t index = 0u; index < app->route_count; ++index) {
    rc = chttp_server_route(&app->server, app->routes[index].method, app->routes[index].path,
                            flowie_control_http_chttp_handler, &app->routes[index]);
    if (rc != SALTS_OK) goto fail;
  }
  rc = chttp_server_start(&app->server);
  if (rc != SALTS_OK) goto fail;
  app->started = 1;
  return SALTS_OK;

fail:
  if (app->initialized) {
    (void)chttp_server_destroy(&app->server);
    app->initialized = 0;
  }
  salts_threadpool_destroy(app->workers);
  app->workers = NULL;
  return rc;
}

int flowie_control_http_app_port(const flowie_control_http_app_t *app, uint16_t *port_out) {
  if (!app || !app->started) return SALTS_EINVAL;
  return chttp_server_port(&app->server, port_out);
}

int flowie_control_http_app_stop(flowie_control_http_app_t *app, uint32_t timeout_ms) {
  int rc;
  if (!app) return SALTS_EINVAL;
  if (!app->started) return SALTS_OK;
  rc = chttp_server_stop(&app->server,
                         timeout_ms ? timeout_ms : FLOWIE_CONTROL_HTTP_STOP_TIMEOUT_MS);
  if (rc != SALTS_OK) return rc;
  app->started = 0;
  salts_threadpool_shutdown(app->workers);
  rc = salts_threadpool_wait_status(app->workers);
  return rc;
}

void flowie_control_http_app_destroy(flowie_control_http_app_t *app) {
  if (!app) return;
  if (app->started) (void)flowie_control_http_app_stop(app, FLOWIE_CONTROL_HTTP_STOP_TIMEOUT_MS);
  if (app->initialized) (void)chttp_server_destroy(&app->server);
  salts_threadpool_destroy(app->workers);
  for (size_t index = 0u; index < app->route_count; ++index)
    free(app->routes[index].path);
  for (size_t index = 0u; index < app->context_count; ++index)
    free(app->contexts[index].path);
  memset(app, 0, sizeof(*app));
  free(app);
}
