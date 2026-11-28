#include "flowie_rulesforge_pipeline_internal.h"
#include "flowie_rulesforge_package_internal.h"
#include "flowie_workflow_https_sink_internal.h"

#include <core/clock.h>
#include <core/error.h>
#include <core/tlog.h>
#include <core/uuid.h>
#include <crypto/sha256.h>
#include <http_client/http.h>
#include <json_parser.h>
#include <threads/threads.h>
#include <uri_parser.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FLOWIE_RULESFORGE_MAX_SOURCES = 32, FLOWIE_RULESFORGE_TOPIC_MAX = 256,
       FLOWIE_RULESFORGE_EVENT_MAX = 16 * 1024, FLOWIE_RULESFORGE_EVENT_ID_MAX = 192,
       FLOWIE_RULESFORGE_MAX_BATCHES = 8, FLOWIE_RULESFORGE_OUTPUT_KEY_MAX = SALTS_UUID_STRING_SIZE + 32u };

#define FLOWIE_RULESFORGE_JOURNAL_RETRY_NS UINT64_C(250000000)

typedef enum flowie_rulesforge_json_mode_e {
  FLOWIE_RULESFORGE_JSON_MANIFEST,
  FLOWIE_RULESFORGE_JSON_EVENT,
  FLOWIE_RULESFORGE_JSON_RETRO_V3
} flowie_rulesforge_json_mode_t;

typedef struct flowie_rulesforge_source_s {
  char topic[FLOWIE_RULESFORGE_TOPIC_MAX + 1u];
  char entry_point[FLOWIE_RULESFORGE_NAME_MAX + 1u];
  char fact_type[FLOWIE_RULESFORGE_NAME_MAX + 1u];
  flowie_rulesforge_json_mode_t mode;
} flowie_rulesforge_source_t;

typedef struct flowie_rulesforge_sink_s {
  const char *name;
  char uri[320];
  char authority[300];
  char path[2049];
  chttp_tls_profile tls;
} flowie_rulesforge_sink_t;

typedef struct flowie_rulesforge_job_result_s {
  int64_t sequence;
  int result;
  char output_key[FLOWIE_RULESFORGE_OUTPUT_KEY_MAX];
  int output_result;
  unsigned int http_status;
} flowie_rulesforge_job_result_t;

struct flowie_rulesforge_pipeline_s {
  const flowie_server_release_config_t *config;
  flowie_control_release_store_t *store;
  flowie_rulesforge_package_t *package;
  chttp_client client;
  sync_thread_t thread;
  sync_mutex_t mutex;
  sync_cond_t changed;
  flowie_control_release_active_t active;
  int has_work;
  int recover;
  int backend_failed;
  int started;
  int initialized;
  int closing;
  int finished;
  int status;
  char run_id[SALTS_UUID_STRING_SIZE];
  char watermark_topic[FLOWIE_RULESFORGE_TOPIC_MAX + 1u];
  size_t source_count;
  size_t sink_count;
  flowie_rulesforge_source_t sources[FLOWIE_RULESFORGE_MAX_SOURCES];
  flowie_rulesforge_sink_t sinks[FLOWIE_SERVER_RULESFORGE_MAX_SINKS];
};

static int pipeline_thread_start(flowie_rulesforge_pipeline_t *pipeline);

typedef struct flowie_rulesforge_json_s {
  uint32_t seen[3];
  size_t depth;
  flowie_rulesforge_json_mode_t mode;
} flowie_rulesforge_json_t;

static int pipeline_json_enter(void *context) {
  flowie_rulesforge_json_t *state = context;
  if (state->mode == FLOWIE_RULESFORGE_JSON_MANIFEST && state->depth == sizeof(state->seen) / sizeof(state->seen[0])) return -1;
  if (state->depth < sizeof(state->seen) / sizeof(state->seen[0])) state->seen[state->depth] = 0;
  ++state->depth;
  return 0;
}

static int pipeline_json_leave(void *context) {
  --((flowie_rulesforge_json_t *)context)->depth;
  return 0;
}

static int pipeline_json_key(void *context, const char *key, size_t size) {
  static const char *const manifest_keys[] = {
      "sources", "watermark_topic", "sinks", "topic", "entry_point", "fact_type", "format"};
  static const char *const event_keys[] = {"id", "time_ms", "data"};
  static const char *const retro_keys[] = {
      "version", "kind", "name", "id", "time", "data", "trace_id", "expires_at", "reply_to", "error"};
  flowie_rulesforge_json_t *state = context;
  if (state->mode != FLOWIE_RULESFORGE_JSON_MANIFEST && state->depth != 1u) return 0;
  const char *const *keys = manifest_keys;
  size_t count = sizeof(manifest_keys) / sizeof(manifest_keys[0]);
  if (state->mode == FLOWIE_RULESFORGE_JSON_EVENT) {
    keys = event_keys;
    count = sizeof(event_keys) / sizeof(event_keys[0]);
  } else if (state->mode == FLOWIE_RULESFORGE_JSON_RETRO_V3) {
    keys = retro_keys;
    count = sizeof(retro_keys) / sizeof(retro_keys[0]);
  }
  for (size_t i = 0; i < count; ++i) {
    if (strlen(keys[i]) != size || memcmp(key, keys[i], size)) continue;
    uint32_t *seen = &state->seen[state->depth - 1u];
    const uint32_t bit = UINT32_C(1) << i;
    if (*seen & bit) return -1;
    *seen |= bit;
    return 0;
  }
  return -1;
}

static json_value_t *pipeline_json_parse(const void *bytes, size_t size, flowie_rulesforge_json_mode_t mode) {
  static const json_sax_handler_raw_t callbacks = {.on_object_start = pipeline_json_enter,
      .on_object_end = pipeline_json_leave, .on_array_start = pipeline_json_enter,
      .on_array_end = pipeline_json_leave, .on_object_key = pipeline_json_key};
  flowie_rulesforge_json_t state = {.mode = mode};
  if (!size || size > FLOWIE_RULESFORGE_EVENT_MAX || memchr(bytes, 0, size) ||
      json_parse_sax_raw(bytes, size, &callbacks, &state)) return NULL;
  return json_parse(bytes, size);
}

static int pipeline_string(const json_value_t *value, char *out, size_t capacity) {
  if (!value || json_type(value) != JSON_STRING) return SALTS_EPROTO;
  const char *text = json_string(value);
  const size_t size = json_string_len(value);
  if (!size || size >= capacity || memchr(text, 0, size)) return SALTS_EPROTO;
  memcpy(out, text, size);
  out[size] = '\0';
  return SALTS_OK;
}

static int pipeline_token(const char *text) {
  if (!text[0]) return 0;
  for (; *text; ++text) {
    const unsigned char c = (unsigned char)*text;
    if ((c < 'a' || c > 'z') && (c < 'A' || c > 'Z') && (c < '0' || c > '9') &&
        c != '-' && c != '_' && c != '.') return 0;
  }
  return 1;
}

static int pipeline_topic(const char *topic, const char *domain) {
  const size_t size = strlen(topic), prefix = strlen(domain);
  return size > prefix + 1u && topic[prefix] == '/' && !memcmp(topic, domain, prefix) &&
      flowie_mqtt_topic_name_validate((flowie_mqtt_span_t){(const uint8_t *)topic, size});
}

static int pipeline_sink_open(flowie_rulesforge_sink_t *sink, const flowie_server_rulesforge_sink_t *config) {
  uri_t uri = {0};
  if (!uri_parse(config->url, &uri) || !uri.valid || strcmp(uri.scheme, "https") || !uri.host[0] ||
      uri.userinfo[0] || uri.fragment[0] || uri.query[0] || uri.path[0] != '/') return SALTS_EINVAL;
  const int port = uri.port ? uri.port : 443;
  if (port < 1 || port > UINT16_MAX) return SALTS_EINVAL;
  const int authority = snprintf(sink->authority, sizeof(sink->authority),
      uri.host_type == URI_HOST_IPV6ADDR ? "[%s]:%d" : "%s:%d", uri.host, port);
  if (authority <= 0 || (size_t)authority >= sizeof(sink->authority)) return SALTS_ERANGE;
  const int written = snprintf(sink->uri, sizeof(sink->uri), "tls://%s", sink->authority);
  if (written <= 0 || (size_t)written >= sizeof(sink->uri) || strlen(uri.path) >= sizeof(sink->path))
    return SALTS_ERANGE;
  strcpy(sink->path, uri.path);
  sink->name = config->name;
  const cnet_tls_client_config tls = {.size = sizeof(tls), .server_name = uri.host,
      .ca_file = config->ca_file, .cert_file = config->cert_file, .key_file = config->key_file};
  return chttp_tls_profile_init(&sink->tls, &tls);
}

static int pipeline_sources(flowie_rulesforge_pipeline_t *pipeline, const json_value_t *root) {
  const json_value_t *sources = json_object_get(root, "sources");
  pipeline->source_count = json_array_size(sources);
  if (!sources || json_type(sources) != JSON_ARRAY || !pipeline->source_count ||
      pipeline->source_count > FLOWIE_RULESFORGE_MAX_SOURCES) return SALTS_EPROTO;
  for (size_t i = 0; i < pipeline->source_count; ++i) {
    const json_value_t *value = json_array_get(sources, i);
    flowie_rulesforge_source_t *source = &pipeline->sources[i];
    const json_value_t *format = json_object_get(value, "format");
    source->mode = FLOWIE_RULESFORGE_JSON_EVENT;
    if (format) {
      char name[16];
      if (pipeline_string(format, name, sizeof(name)) || strcmp(name, "retro_v3")) return SALTS_EPROTO;
      source->mode = FLOWIE_RULESFORGE_JSON_RETRO_V3;
    }
    if (json_type(value) != JSON_OBJECT || json_object_size(value) != (format ? 4u : 3u) ||
        pipeline_string(json_object_get(value, "topic"), source->topic, sizeof(source->topic)) ||
        pipeline_string(json_object_get(value, "entry_point"), source->entry_point, sizeof(source->entry_point)) ||
        pipeline_string(json_object_get(value, "fact_type"), source->fact_type, sizeof(source->fact_type)) ||
        !pipeline_topic(source->topic, pipeline->config->domain) ||
        !pipeline_token(source->entry_point) || !pipeline_token(source->fact_type) ||
        !strcmp(source->topic, pipeline->watermark_topic)) return SALTS_EPROTO;
    for (size_t j = 0; j < i; ++j) {
      if (!strcmp(source->topic, pipeline->sources[j].topic)) return SALTS_EPROTO;
    }
  }
  return SALTS_OK;
}

static int pipeline_sinks(flowie_rulesforge_pipeline_t *pipeline, const json_value_t *root) {
  const json_value_t *sinks = json_object_get(root, "sinks");
  const flowie_server_rulesforge_config_t *config = &pipeline->config->rulesforge;
  const size_t count = json_array_size(sinks);
  if (!sinks || json_type(sinks) != JSON_ARRAY || count > FLOWIE_SERVER_RULESFORGE_MAX_SINKS)
    return SALTS_EPROTO;
  for (size_t i = 0; i < count; ++i) {
    char name[FLOWIE_SERVER_RULESFORGE_SINK_NAME + 1u];
    if (pipeline_string(json_array_get(sinks, i), name, sizeof(name)) || !pipeline_token(name))
      return SALTS_EPROTO;
    for (size_t j = 0; j < i; ++j) {
      if (!strcmp(name, pipeline->sinks[j].name)) return SALTS_EPROTO;
    }
    size_t selected = 0;
    while (selected < config->sink_count && strcmp(name, config->sinks[selected].name)) ++selected;
    if (selected == config->sink_count) return SALTS_EPROTO;
    ++pipeline->sink_count;
    const int rc = pipeline_sink_open(&pipeline->sinks[i], &config->sinks[selected]);
    if (rc != SALTS_OK) return rc;
  }
  return SALTS_OK;
}

static int pipeline_close(flowie_rulesforge_pipeline_t **owner) {
  flowie_rulesforge_pipeline_t *pipeline = *owner;
  if (!pipeline) return SALTS_OK;
  if (pipeline->started) {
    sync_mutex_lock(&pipeline->mutex);
    if (!pipeline->finished) {
      pipeline->closing = 1;
      sync_cond_signal(&pipeline->changed);
      while (pipeline->closing) sync_cond_wait(&pipeline->changed, &pipeline->mutex);
    }
    const int status = pipeline->status;
    sync_mutex_unlock(&pipeline->mutex);
    if (status != SALTS_OK) return status;
    const int joined = sync_thread_join(&pipeline->thread);
    if (joined != SALTS_OK) return joined;
    sync_cond_destroy(&pipeline->changed);
    sync_mutex_destroy(&pipeline->mutex);
  }
  int rc = flowie_rulesforge_package_close(&pipeline->package);
  for (size_t i = 0; i < pipeline->sink_count; ++i) {
    if (!pipeline->sinks[i].tls.impl) continue;
    const int closed = chttp_tls_profile_destroy(&pipeline->sinks[i].tls);
    if (rc == SALTS_OK) rc = closed;
  }
  free(pipeline);
  *owner = NULL;
  return rc;
}

int flowie_rulesforge_pipeline_reap(flowie_rulesforge_pipeline_provider_t *provider) {
  int rc = SALTS_OK;
  for (size_t i = 0; i < sizeof(provider->retired) / sizeof(provider->retired[0]); ++i) {
    const int closed = pipeline_close(&provider->retired[i]);
    if (rc == SALTS_OK) rc = closed;
  }
  return rc;
}

int flowie_rulesforge_pipeline_close(void *context, void *handle) {
  flowie_rulesforge_pipeline_provider_t *provider = context;
  flowie_rulesforge_pipeline_t *pipeline = handle;
  const int rc = pipeline_close(&pipeline);
  if (pipeline) {
    /* Prepare refuses another candidate while retirement is incomplete. */
    size_t index = provider->retired[0] ? 1u : 0u;
    provider->retired[index] = pipeline;
  }
  return rc;
}

int flowie_rulesforge_pipeline_prepare(void *context, const flowie_release_archive_t *archive, void **out) {
  *out = NULL;
  flowie_rulesforge_pipeline_provider_t *provider = context;
  const flowie_server_release_config_t *deployment = provider->config;
  int rc = flowie_rulesforge_pipeline_reap(provider);
  if (rc != SALTS_OK) return rc;
  const void *bytes = NULL;
  size_t size = 0;
  rc = flowie_control_release_archive_file(archive, FLOWIE_RULESFORGE_FLOW_PATH, &bytes, &size);
  if (rc != SALTS_OK) return rc == SALTS_ENOENT ? SALTS_EPROTO : rc;
  json_value_t *root = pipeline_json_parse(bytes, size, FLOWIE_RULESFORGE_JSON_MANIFEST);
  if (!root) return SALTS_EPROTO;
  flowie_rulesforge_pipeline_t *pipeline = calloc(1u, sizeof(*pipeline));
  if (!pipeline) {
    json_free(root);
    return SALTS_ENOMEM;
  }
  *out = pipeline; /* The release owner closes partial preparation. */
  pipeline->config = deployment;
  pipeline->store = provider->store;
  const flowie_server_rulesforge_config_t *config = &deployment->rulesforge;
  rc = SALTS_EPROTO;
  if (json_type(root) == JSON_OBJECT && json_object_size(root) == 3u &&
      pipeline_string(json_object_get(root, "watermark_topic"), pipeline->watermark_topic,
          sizeof(pipeline->watermark_topic)) == SALTS_OK &&
      pipeline_topic(pipeline->watermark_topic, deployment->domain)) {
    rc = pipeline_sources(pipeline, root);
    if (rc == SALTS_OK) rc = pipeline_sinks(pipeline, root);
  }
  json_free(root);
  unsigned char id[SALTS_UUID_SIZE];
  if (rc == SALTS_OK) rc = uuid_v4_generate(id);
  if (rc == SALTS_OK) rc = uuid_format(id, pipeline->run_id, sizeof(pipeline->run_id));
  const flowie_rulesforge_package_config_t package = {config->executable, config->provider,
      config->staging_root, config->timeout_ms};
  if (rc == SALTS_OK) rc = flowie_rulesforge_package_prepare(&package, archive, &pipeline->package);
  if (rc == SALTS_OK) rc = pipeline_thread_start(pipeline);
  return rc;
}

static int pipeline_timestamp(const json_value_t *value, int64_t *out) {
  size_t size = 0;
  const char *text = value ? json_number_text(value, &size) : NULL;
  if (!text || !size) return SALTS_EINVAL;
  int64_t time = 0;
  for (size_t i = 0; i < size; ++i) {
    const int digit = text[i] - '0';
    if (digit < 0 || digit > 9 || time > (INT64_MAX - digit) / 10) return SALTS_EINVAL;
    time = time * 10 + digit;
  }
  *out = time;
  return SALTS_OK;
}

static void pipeline_hex(const uint8_t *bytes, size_t size, char *out) {
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < size; ++i) {
    out[2u * i] = digits[bytes[i] >> 4u];
    out[2u * i + 1u] = digits[bytes[i] & 15u];
  }
  out[2u * size] = '\0';
}

static int pipeline_fact_string(json_value_t *object, const char *key, const char *text) {
  json_value_t *value = json_create_string(text);
  if (value && json_object_add_checked(object, key, value)) return SALTS_OK;
  if (value) json_free(value);
  return SALTS_ENOMEM;
}

static int pipeline_retro_fact(const json_value_t *root, int64_t *time, const char *id,
    json_value_t **fact) {
  int64_t version;
  char kind[16], name[FLOWIE_RULESFORGE_NAME_MAX + 1u];
  if (pipeline_timestamp(json_object_get(root, "version"), &version) || version != 3 ||
      pipeline_timestamp(json_object_get(root, "time"), time) || *time > INT64_MAX / 1000 ||
      pipeline_string(json_object_get(root, "kind"), kind, sizeof(kind)) || !pipeline_token(kind))
    return SALTS_EINVAL;
  const json_value_t *name_value = json_object_get(root, "name");
  if (!name_value && !strcmp(kind, "ack")) name[0] = '\0';
  else if (pipeline_string(name_value, name, sizeof(name)) || !pipeline_token(name)) return SALTS_EINVAL;
  if (strspn(id, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._:-") != strlen(id))
    return SALTS_EINVAL;
  char *body = json_serialize(root, NULL);
  if (!body) return SALTS_ENOMEM;
  *time *= 1000;
  *fact = json_create_object();
  const int rc = !*fact || pipeline_fact_string(*fact, "kind", kind) ||
      pipeline_fact_string(*fact, "name", name) || pipeline_fact_string(*fact, "body", body) ?
      SALTS_ENOMEM : SALTS_OK;
  json_serialize_free(body);
  return rc;
}

static int pipeline_begin(flowie_rulesforge_worker_t *worker, const flowie_rulesforge_source_t *source,
    const flowie_release_job_t *job,
    flowie_rulesforge_batch_t *batch) {
  const flowie_rulesforge_json_mode_t mode = source ? source->mode : FLOWIE_RULESFORGE_JSON_EVENT;
  json_value_t *root = pipeline_json_parse(job->payload, job->size, mode);
  if (!root) return SALTS_EINVAL;
  int64_t time;
  int rc = SALTS_EINVAL;
  json_value_t *fact = NULL;
  if (json_type(root) != JSON_OBJECT) goto done;
  if (mode == FLOWIE_RULESFORGE_JSON_EVENT) {
    if (json_object_size(root) != (source ? 3u : 1u) ||
        pipeline_timestamp(json_object_get(root, "time_ms"), &time) != SALTS_OK) goto done;
    if (!source) {
      rc = flowie_rulesforge_worker_watermark(worker, time, batch);
      goto done;
    }
  }
  char event_id[FLOWIE_RULESFORGE_EVENT_ID_MAX + 1u];
  const json_value_t *data = json_object_get(root, "data");
  const size_t id_capacity = mode == FLOWIE_RULESFORGE_JSON_RETRO_V3 ? sizeof(event_id) : 65u;
  if (pipeline_string(json_object_get(root, "id"), event_id, id_capacity) ||
      !data || json_type(data) != JSON_OBJECT) goto done;
  if (mode == FLOWIE_RULESFORGE_JSON_RETRO_V3) {
    rc = pipeline_retro_fact(root, &time, event_id, &fact);
    if (rc != SALTS_OK) goto done;
    data = fact;
  } else if (!pipeline_token(event_id)) {
    goto done;
  }
  const size_t principal_size = strlen(job->principal) + 1u;
  char source_id[FLOWIE_SECURITY_ID_MAX + sizeof(event_id) + 1u];
  memcpy(source_id, job->principal, principal_size);
  memcpy(source_id + principal_size, event_id, strlen(event_id));
  uint8_t digest[SALTS_CRYPTO_SHA256_DIGEST_SIZE];
  if (salts_crypto_sha256(source_id, principal_size + strlen(event_id), digest) != SALTS_CRYPTO_OK) {
    rc = SALTS_EIO;
    goto done;
  }
  char id[sizeof(digest) * 2u + 1u];
  pipeline_hex(digest, sizeof(digest), id);
  size_t size = 0;
  char *json = json_serialize(data, &size);
  rc = json ? flowie_rulesforge_worker_push(worker, time, id, source->entry_point, source->fact_type,
      json, size, batch) : SALTS_ENOMEM;
  if (json) json_serialize_free(json);
done:
  if (fact) json_free(fact);
  json_free(root);
  return rc;
}

static flowie_rulesforge_sink_t *pipeline_sink(flowie_rulesforge_pipeline_t *pipeline,
    const flowie_rulesforge_output_t *output) {
  for (size_t i = 0; i < pipeline->sink_count; ++i) {
    const char *name = pipeline->sinks[i].name;
    if (strlen(name) == output->sink_size && !memcmp(name, output->sink, output->sink_size))
      return &pipeline->sinks[i];
  }
  return NULL;
}

static int pipeline_client(chttp_client *client, uint32_t timeout) {
  const chttp_client_config config = {.network = {.backend =
#if defined(_WIN32)
      NATIVE_IO_BACKEND_IOCP,
#elif defined(__linux__)
      NATIVE_IO_BACKEND_EPOLL,
#else
      NATIVE_IO_BACKEND_KQUEUE,
#endif
      .connection_capacity = FLOWIE_SERVER_RULESFORGE_MAX_SINKS, .command_capacity = 32u,
      .request_capacity = 8u, .event_capacity = 32u, .completion_batch_capacity = 8u,
      .max_send_bytes = FLOWIE_RULESFORGE_BODY_MAX + 4096u, .receive_buffer_bytes = 4096u,
      .connect_timeout_ms = timeout, .read_timeout_ms = timeout, .write_timeout_ms = timeout,
      .tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES, .tls_handshake_timeout_ms = timeout},
      .request_capacity = 1u, .max_start_line_bytes = 2048u, .max_header_count = 16u,
      .max_header_bytes = 4096u, .max_request_body_bytes = FLOWIE_RULESFORGE_BODY_MAX,
      .max_response_body_bytes = 4096u, .max_informational_responses = 4u};
  return chttp_client_init(client, &config);
}

static int pipeline_deliver(flowie_rulesforge_pipeline_t *pipeline,
    const flowie_release_journal_scope_t *scope, const flowie_release_job_t *job,
    const flowie_rulesforge_batch_t *batch,
    uint64_t deadline, flowie_rulesforge_job_result_t *result, int *rejection) {
  flowie_rulesforge_sink_t *sinks[FLOWIE_RULESFORGE_OUTPUT_MAX];
  for (size_t i = 0; i < batch->output_count; ++i) {
    sinks[i] = pipeline_sink(pipeline, &batch->outputs[i]);
    if (!sinks[i]) return SALTS_EPERM;
  }
  if (!batch->output_count) return SALTS_OK;
  char digest[2u * FLOWIE_RELEASE_DIGEST_BYTES + 1u], generation[24];
  pipeline_hex(pipeline->active.digest, sizeof(pipeline->active.digest), digest);
  snprintf(generation, sizeof(generation), "%" PRIu64, scope->generation);
  for (size_t i = 0; i < batch->output_count; ++i) {
    const uint64_t now = monotonic_ms();
    if (now >= deadline) return SALTS_ETIMEDOUT;
    char key[FLOWIE_RULESFORGE_OUTPUT_KEY_MAX];
    snprintf(key, sizeof(key), "%s-%" PRIu64 "-%u", pipeline->run_id, batch->id, (unsigned)i);
    const chttp_header headers[] = {{"Content-Type", "application/octet-stream"},
        {"Flowie-Domain", pipeline->config->domain}, {"Flowie-Release-Digest", digest},
        {"Flowie-Generation", generation}, {"Flowie-Sink", sinks[i]->name}, {"Idempotency-Key", key}};
    unsigned int http_status = 0;
    flowie_workflow_https_sink_t sink = {.client = &pipeline->client, .http_status = &http_status, .request = {
        .connection_uri = sinks[i]->uri, .authority = sinks[i]->authority, .target = sinks[i]->path,
        .headers = headers, .header_count = sizeof(headers) / sizeof(headers[0]),
        .timeout_ms = (uint32_t)(deadline - now), .tls = &sinks[i]->tls, .protocol = CHTTP_HTTP_1_1}};
    const flowie_workflow_event_t event = {.payload = batch->outputs[i].body,
        .payload_size = batch->outputs[i].body_size};
    int rc = flowie_control_release_record_output(pipeline->store, scope, job->sequence, key,
        sinks[i]->name, event.payload, event.payload_size);
    if (rc != SALTS_OK) return rc;
    const uint64_t ready = monotonic_ms();
    rc = SALTS_ETIMEDOUT;
    if (ready < deadline) {
      sink.request.timeout_ms = (uint32_t)(deadline - ready);
      rc = flowie_workflow_https_deliver(&sink, &event);
    }
    const int recorded = flowie_control_release_finish_output(pipeline->store, key, rc, http_status);
    if (recorded != SALTS_OK) {
      memcpy(result->output_key, key, strlen(key) + 1u);
      result->output_result = rc;
      result->http_status = http_status;
      return recorded;
    }
    if (rc == SALTS_EPROTO && http_status >= 300u) {
      *rejection = rc;
      continue;
    }
    if (rc != SALTS_OK) return rc;
  }
  return SALTS_OK;
}

static int pipeline_topic_equal(flowie_mqtt_span_t topic, const char *name) {
  return strlen(name) == topic.size && !memcmp(topic.data, name, topic.size);
}

static const flowie_rulesforge_source_t *pipeline_source(flowie_rulesforge_pipeline_t *pipeline,
    flowie_mqtt_span_t topic) {
  for (size_t i = 0; i < pipeline->source_count; ++i) {
    if (pipeline_topic_equal(topic, pipeline->sources[i].topic)) return &pipeline->sources[i];
  }
  return NULL;
}

static int pipeline_run(flowie_rulesforge_pipeline_t *pipeline,
    const flowie_release_journal_scope_t *scope, const flowie_release_job_t *job,
    flowie_rulesforge_job_result_t *result) {
  if (pipeline->backend_failed) return SALTS_ESHUTDOWN;
  const flowie_rulesforge_source_t *source = pipeline_source(pipeline,
      (flowie_mqtt_span_t){(const uint8_t *)job->topic, strlen(job->topic)});
  if (!source && strcmp(job->topic, pipeline->watermark_topic)) return SALTS_EPROTO;
  flowie_rulesforge_worker_t *worker = flowie_rulesforge_package_worker(pipeline->package);
  const uint32_t timeout = pipeline->config->rulesforge.timeout_ms;
  const uint64_t deadline = monotonic_ms() + timeout;
  flowie_rulesforge_batch_t batch = {0};
  int rc = pipeline_begin(worker, source, job, &batch);
  if (rc == SALTS_EINVAL || rc == SALTS_ENOSPC) return rc;

  int rejection = SALTS_OK;
  for (unsigned int count = 1; rc == SALTS_OK; ++count) {
    rc = pipeline_deliver(pipeline, scope, job, &batch, deadline, result, &rejection);
    if (rc != SALTS_OK) break;
    if (monotonic_ms() >= deadline) { rc = SALTS_ETIMEDOUT; break; }
    rc = flowie_rulesforge_worker_ack(worker, batch.id);
    if (rc != SALTS_OK || !batch.drain_required) break;
    if (count == FLOWIE_RULESFORGE_MAX_BATCHES) { rc = SALTS_ENOSPC; break; }
    if (monotonic_ms() >= deadline) { rc = SALTS_ETIMEDOUT; break; }
    rc = flowie_rulesforge_worker_drain(worker, &batch);
    if (rc != SALTS_OK) break;
  }
  if (rc != SALTS_OK) pipeline->backend_failed = 1;
  return rc == SALTS_OK ? rejection : rc;
}

static int pipeline_record_result(flowie_rulesforge_pipeline_t *pipeline,
    const flowie_release_journal_scope_t *scope, flowie_rulesforge_job_result_t *result) {
  if (result->output_key[0]) {
    const int rc = flowie_control_release_finish_output(pipeline->store, result->output_key,
        result->output_result, result->http_status);
    if (rc != SALTS_OK) return rc;
    result->output_key[0] = 0;
  }
  const int rc = flowie_control_release_finish(pipeline->store, scope, result->sequence, result->result);
  if (rc == SALTS_OK) result->sequence = 0;
  return rc;
}

static void pipeline_thread(void *context) {
  flowie_rulesforge_pipeline_t *pipeline = context;
  const uint32_t timeout = pipeline->config->rulesforge.timeout_ms;
  int status = pipeline_client(&pipeline->client, timeout);
  sync_mutex_lock(&pipeline->mutex);
  pipeline->status = status;
  pipeline->initialized = 1;
  sync_cond_signal(&pipeline->changed);
  flowie_rulesforge_job_result_t result = {0};
  int last_journal_error = SALTS_OK;
  while (!pipeline->finished) {
    while ((!pipeline->active.generation || !pipeline->has_work) && !pipeline->closing)
      sync_cond_wait(&pipeline->changed, &pipeline->mutex);
    const int closing = pipeline->closing;
    const flowie_control_release_active_t active = pipeline->active;
    pipeline->has_work = 0;
    sync_mutex_unlock(&pipeline->mutex);
    if (closing) {
      status = chttp_client_destroy(&pipeline->client, timeout);
    } else {
      const flowie_release_journal_scope_t scope = {pipeline->config->environment,
          pipeline->config->domain, active.generation, active.digest};
      status = pipeline->recover ? flowie_control_release_interrupt(pipeline->store, &scope) : SALTS_OK;
      if (status == SALTS_OK) {
        pipeline->recover = 0;
        if (!result.sequence) {
          flowie_release_job_t job;
          status = flowie_control_release_claim(pipeline->store, &scope, &job);
          if (status == SALTS_OK) {
            result.sequence = job.sequence;
            result.result = pipeline_run(pipeline, &scope, &job, &result);
          }
        }
        if (status == SALTS_OK) status = pipeline_record_result(pipeline, &scope, &result);
      }
      if (status != SALTS_OK && status != SALTS_ENOENT && status != last_journal_error) {
        SALTS_LOG_ERRORF(tlog_peek_default(), "Flowie.RulesForge",
            "journal domain={} generation={} status={} ({})", scope.domain,
            (unsigned long long)scope.generation, status, error_code_string(status));
      }
      last_journal_error = status;
    }
    sync_mutex_lock(&pipeline->mutex);
    pipeline->status = status;
    if (closing) {
      pipeline->closing = 0;
      pipeline->finished = status == SALTS_OK;
    } else if (status == SALTS_OK || status == SALTS_EBUSY || status == SALTS_EIO) {
      pipeline->has_work = 1;
      if (status != SALTS_OK) {
        const uint64_t retry_at = monotonic_ms() + FLOWIE_RULESFORGE_JOURNAL_RETRY_NS / UINT64_C(1000000);
        uint64_t now;
        while (!pipeline->closing && (now = monotonic_ms()) < retry_at)
          sync_cond_timedwait(&pipeline->changed, &pipeline->mutex, (retry_at - now) * UINT64_C(1000000));
      }
    }
    if (closing) sync_cond_signal(&pipeline->changed);
  }
  sync_mutex_unlock(&pipeline->mutex);
}

static int pipeline_thread_start(flowie_rulesforge_pipeline_t *pipeline) {
  sync_mutex_init(&pipeline->mutex);
  sync_cond_init(&pipeline->changed);
  int rc = sync_thread_create(&pipeline->thread, pipeline_thread, pipeline);
  if (rc != SALTS_OK) {
    sync_cond_destroy(&pipeline->changed);
    sync_mutex_destroy(&pipeline->mutex);
    return rc;
  }
  pipeline->started = 1;
  sync_mutex_lock(&pipeline->mutex);
  while (!pipeline->initialized) sync_cond_wait(&pipeline->changed, &pipeline->mutex);
  rc = pipeline->status;
  sync_mutex_unlock(&pipeline->mutex);
  return rc;
}

int flowie_rulesforge_pipeline_publish(void *handle, const flowie_control_release_active_t *active,
    const flowie_security_principal_t *principal, const flowie_mqtt_publish_view_t *publish,
    const uint8_t *exchange, int *handled) {
  flowie_rulesforge_pipeline_t *pipeline = handle;
  const flowie_rulesforge_source_t *source = pipeline_source(pipeline, publish->topic);
  if (!source && !pipeline_topic_equal(publish->topic, pipeline->watermark_topic)) return SALTS_OK;
  *handled = 1;
  if (publish->retain) return SALTS_EINVAL;
  const flowie_release_journal_scope_t scope = {pipeline->config->environment,
      pipeline->config->domain, active->generation, active->digest};
  const int rc = flowie_control_release_enqueue(pipeline->store, &scope, principal->principal_id,
      source ? source->topic : pipeline->watermark_topic, publish->payload.data, publish->payload.size, exchange);
  if (rc != SALTS_OK) return rc;
  sync_mutex_lock(&pipeline->mutex);
  pipeline->has_work = 1;
  sync_cond_signal(&pipeline->changed);
  sync_mutex_unlock(&pipeline->mutex);
  return rc;
}

void flowie_rulesforge_pipeline_activate(void *handle, const flowie_control_release_active_t *active,
    int recovered) {
  flowie_rulesforge_pipeline_t *pipeline = handle;
  sync_mutex_lock(&pipeline->mutex);
  if (!pipeline->active.generation) {
    pipeline->active = *active;
    pipeline->recover = recovered;
    pipeline->has_work = 1;
    sync_cond_signal(&pipeline->changed);
  }
  sync_mutex_unlock(&pipeline->mutex);
}
