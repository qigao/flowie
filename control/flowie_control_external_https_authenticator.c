#include "flowie_control_external_https_authenticator_internal.h"

#include "base64_utils.h"
#include <chttp/chttp.h>
#include <json_parser.h>
#include <uri_parser.h>
#include "monocypher.h"
#include "salts_str.h"
#include <salts/error_codes.h>

#include <openssl/ssl.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  FLOWIE_CONTROL_EXTERNAL_HTTPS_HEADER_LIMIT = 16384u,
  FLOWIE_CONTROL_EXTERNAL_HTTPS_DESTROY_TIMEOUT_MS = 5000u
};

typedef struct flowie_control_external_https_tls_s {
  tstr ca_file;
  tstr client_cert_file;
  tstr client_key_file;
  tstr client_key_password_ref;
} flowie_control_external_https_tls_t;

typedef enum external_https_outcome_e {
  EXTERNAL_HTTPS_OUTCOME_LOCAL_FAILURE = 0,
  EXTERNAL_HTTPS_OUTCOME_SUCCEEDED,
  EXTERNAL_HTTPS_OUTCOME_DENIED,
  EXTERNAL_HTTPS_OUTCOME_LOCAL_OVERLOAD,
  EXTERNAL_HTTPS_OUTCOME_REMOTE_OVERLOAD,
  EXTERNAL_HTTPS_OUTCOME_REMOTE_SERVER_FAILURE,
  EXTERNAL_HTTPS_OUTCOME_TRANSPORT_FAILURE,
  EXTERNAL_HTTPS_OUTCOME_PROTOCOL_FAILURE
} external_https_outcome_t;

struct flowie_control_external_https_authenticator_s {
  flowie_control_external_authenticator_t interface;
  tstr url;
  tstr host;
  tstr method;
  tstr service_token_ref;
  uint16_t port;
  uint32_t timeout_ms;
  size_t max_response_size;
  uint32_t max_in_flight;
  atomic_uint in_flight;
  atomic_uint_fast64_t started_requests;
  atomic_uint_fast64_t succeeded;
  atomic_uint_fast64_t denied;
  atomic_uint_fast64_t local_overload;
  atomic_uint_fast64_t remote_overload;
  atomic_uint_fast64_t remote_server_failures;
  atomic_uint_fast64_t transport_failures;
  atomic_uint_fast64_t protocol_failures;
  atomic_uint_fast64_t local_failures;
  flowie_security_key_provider_t key_provider;
  flowie_control_external_https_tls_t tls;
};

static void external_https_counter_increment(atomic_uint_fast64_t *counter) {
  uint_fast64_t current;
  if (!counter) return;
  current = atomic_load_explicit(counter, memory_order_relaxed);
  while (current < UINT64_MAX &&
         !atomic_compare_exchange_weak_explicit(counter, &current, current + 1u,
                                                memory_order_relaxed, memory_order_relaxed)) {
  }
}

static void
external_https_record_outcome(flowie_control_external_https_authenticator_t *authenticator,
                              external_https_outcome_t outcome) {
  atomic_uint_fast64_t *counter;
  if (!authenticator) return;
  switch (outcome) {
  case EXTERNAL_HTTPS_OUTCOME_SUCCEEDED:
    counter = &authenticator->succeeded;
    break;
  case EXTERNAL_HTTPS_OUTCOME_DENIED:
    counter = &authenticator->denied;
    break;
  case EXTERNAL_HTTPS_OUTCOME_LOCAL_OVERLOAD:
    counter = &authenticator->local_overload;
    break;
  case EXTERNAL_HTTPS_OUTCOME_REMOTE_OVERLOAD:
    counter = &authenticator->remote_overload;
    break;
  case EXTERNAL_HTTPS_OUTCOME_REMOTE_SERVER_FAILURE:
    counter = &authenticator->remote_server_failures;
    break;
  case EXTERNAL_HTTPS_OUTCOME_TRANSPORT_FAILURE:
    counter = &authenticator->transport_failures;
    break;
  case EXTERNAL_HTTPS_OUTCOME_PROTOCOL_FAILURE:
    counter = &authenticator->protocol_failures;
    break;
  case EXTERNAL_HTTPS_OUTCOME_LOCAL_FAILURE:
  default:
    counter = &authenticator->local_failures;
    break;
  }
  external_https_counter_increment(counter);
}

static int external_https_text_valid(const char *value, size_t maximum, int required) {
  size_t size;
  if (!value) return !required;
  size = strnlen(value, maximum + 1u);
  if (size > maximum || (required && size == 0u)) return 0;
  for (size_t index = 0u; index < size; ++index) {
    unsigned char byte = (unsigned char)value[index];
    if (byte < 0x20u || byte == 0x7fu) return 0;
  }
  return 1;
}

static int external_https_fingerprint_valid(const char *value) {
  static const char prefix[] = "sha256:";
  size_t size;
  if (!value) return 1;
  size = strlen(value);
  if (size != FLOWIE_CONTROL_AUTH_CERT_SHA256_TEXT_SIZE ||
      memcmp(value, prefix, sizeof(prefix) - 1u) != 0)
    return 0;
  for (size_t i = sizeof(prefix) - 1u; i < size; ++i)
    if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) return 0;
  return 1;
}

static int external_https_optional_path_valid(const char *value) {
  if (!value) return 1;
  return external_https_text_valid(value, FLOWIE_CONTROL_EXTERNAL_HTTPS_TLS_PATH_MAX, 1);
}

static int external_https_ascii_prefix(const char *value, size_t value_size, const char *prefix) {
  size_t prefix_size = strlen(prefix);
  if (!value || value_size < prefix_size) return 0;
  for (size_t index = 0u; index < prefix_size; ++index)
    if (tolower((unsigned char)value[index]) != tolower((unsigned char)prefix[index])) return 0;
  return 1;
}

static int external_https_json_fields_exact(const json_value_t *object, const char *const *allowed,
                                            size_t allowed_count) {
  if (!object || json_type(object) != JSON_OBJECT ||
      json_object_size(object) != allowed_count)
    return SALTS_EPROTO;
  for (size_t field_index = 0u; field_index < json_object_size(object); ++field_index) {
    const char *field = json_object_key(object, field_index);
    size_t matches = 0u;
    for (size_t allowed_index = 0u; allowed_index < allowed_count; ++allowed_index)
      if (field && strcmp(field, allowed[allowed_index]) == 0) ++matches;
    if (matches != 1u) return SALTS_EPROTO;
    for (size_t previous = 0u; previous < field_index; ++previous) {
      const char *previous_field = json_object_key(object, previous);
      if (previous_field && field && strcmp(previous_field, field) == 0) return SALTS_EPROTO;
    }
  }
  return SALTS_OK;
}

static int external_https_json_u64(const json_value_t *value, uint64_t *out) {
  const char *text;
  char buffer[32];
  char *end = NULL;
  size_t size = 0u;
  unsigned long long parsed;
  if (!value || json_type(value) != JSON_NUMBER || !out) return SALTS_EPROTO;
  text = json_number_text(value, &size);
  if (!text || size == 0u || size >= sizeof(buffer)) return SALTS_EPROTO;
  memcpy(buffer, text, size);
  buffer[size] = '\0';
  if (buffer[0] == '-' || buffer[0] == '+' || (size > 1u && buffer[0] == '0')) return SALTS_EPROTO;
  errno = 0;
  parsed = strtoull(buffer, &end, 10);
  if (errno == ERANGE || !end || *end != '\0') return SALTS_EPROTO;
  *out = (uint64_t)parsed;
  return SALTS_OK;
}

static int external_https_copy_json_string(const json_value_t *object, const char *field, char *out,
                                           size_t capacity) {
  json_value_t *value;
  const char *text;
  size_t size;
  if (!object || !field || !out || capacity == 0u) return SALTS_EPROTO;
  value = json_object_get(object, field);
  if (!value || json_type(value) != JSON_STRING) return SALTS_EPROTO;
  text = json_string(value);
  size = json_string_len(value);
  if (!text || size == 0u || size >= capacity || memchr(text, '\0', size)) return SALTS_EPROTO;
  memcpy(out, text, size);
  out[size] = '\0';
  return external_https_text_valid(out, capacity - 1u, 1) ? SALTS_OK : SALTS_EPROTO;
}

static int external_https_copy_groups(const json_value_t *object,
                                      flowie_control_external_auth_assertion_t *assertion) {
  json_value_t *groups = json_object_get(object, "groups");
  size_t count;
  if (!groups || json_type(groups) != JSON_ARRAY || !assertion) return SALTS_EPROTO;
  count = json_array_size(groups);
  if (count > FLOWIE_SECURITY_MAX_GROUPS) return SALTS_EPROTO;
  for (size_t index = 0u; index < count; ++index) {
    json_value_t *entry = json_array_get(groups, index);
    const char *text;
    size_t size;
    if (!entry || json_type(entry) != JSON_STRING) return SALTS_EPROTO;
    text = json_string(entry);
    size = json_string_len(entry);
    if (!text || size == 0u || size > FLOWIE_SECURITY_ID_MAX || memchr(text, '\0', size))
      return SALTS_EPROTO;
    memcpy(assertion->external_groups[index], text, size);
    assertion->external_groups[index][size] = '\0';
    if (!external_https_text_valid(assertion->external_groups[index], FLOWIE_SECURITY_ID_MAX, 1))
      return SALTS_EPROTO;
    for (size_t previous = 0u; previous < index; ++previous)
      if (strcmp(assertion->external_groups[previous], assertion->external_groups[index]) == 0)
        return SALTS_EPROTO;
  }
  assertion->external_group_count = (uint32_t)count;
  return SALTS_OK;
}

int flowie_control_external_https_decode_response(
    const char *body, size_t body_size, const char *method,
    flowie_control_external_auth_assertion_t *assertion_out) {
  static const char *const denied_fields[] = {"version", "authenticated"};
  static const char *const outer_fields[] = {"version", "authenticated", "assertion"};
  static const char *const assertion_fields[] = {
      "issuer",     "domain_id", "subject",         "subject_type",    "auth_method", "issued_at",
      "expires_at", "revision",  "assurance_level", "account_enabled", "groups"};
  flowie_control_external_auth_assertion_t assertion = FLOWIE_CONTROL_EXTERNAL_AUTH_ASSERTION_INIT;
  json_value_t *document = NULL;
  json_value_t *authenticated;
  json_value_t *assertion_object;
  json_value_t *account_enabled;
  uint64_t version = 0u;
  uint64_t assurance = 0u;
  int rc = SALTS_EPROTO;
  if (assertion_out && assertion_out->size >= sizeof(*assertion_out)) *assertion_out = assertion;
  if (!body || body_size == 0u || body_size > FLOWIE_CONTROL_EXTERNAL_HTTPS_MAX_RESPONSE_SIZE ||
      !external_https_text_valid(method, FLOWIE_SECURITY_TYPE_MAX, 1) || !assertion_out ||
      assertion_out->size < sizeof(*assertion_out))
    return SALTS_EINVAL;
  document = json_parse(body, body_size);
  if (!document)
    return SALTS_EPROTO;
  authenticated = json_object_get(document, "authenticated");
  if (!authenticated || json_type(authenticated) != JSON_BOOL ||
      external_https_json_u64(json_object_get(document, "version"), &version) != SALTS_OK ||
      version != FLOWIE_CONTROL_EXTERNAL_HTTPS_PROTOCOL_VERSION)
    goto done;
  if (!json_bool(authenticated)) {
    if (external_https_json_fields_exact(
            document, denied_fields, sizeof(denied_fields) / sizeof(denied_fields[0])) != SALTS_OK)
      goto done;
    rc = SALTS_EPERM;
    goto done;
  }
  if (external_https_json_fields_exact(document, outer_fields,
                                       sizeof(outer_fields) / sizeof(outer_fields[0])) != SALTS_OK)
    goto done;
  assertion_object = json_object_get(document, "assertion");
  if (external_https_json_fields_exact(assertion_object, assertion_fields,
                                       sizeof(assertion_fields) / sizeof(assertion_fields[0])) !=
      SALTS_OK)
    goto done;
  if (external_https_copy_json_string(assertion_object, "issuer", assertion.issuer,
                                      sizeof(assertion.issuer)) != SALTS_OK ||
      external_https_copy_json_string(assertion_object, "domain_id", assertion.domain_id,
                                      sizeof(assertion.domain_id)) != SALTS_OK ||
      external_https_copy_json_string(assertion_object, "subject", assertion.subject,
                                      sizeof(assertion.subject)) != SALTS_OK ||
      external_https_copy_json_string(assertion_object, "subject_type", assertion.subject_type,
                                      sizeof(assertion.subject_type)) != SALTS_OK ||
      external_https_copy_json_string(assertion_object, "auth_method", assertion.auth_method,
                                      sizeof(assertion.auth_method)) != SALTS_OK ||
      strcmp(assertion.auth_method, method) != 0 ||
      external_https_json_u64(json_object_get(assertion_object, "issued_at"),
                              &assertion.issued_at) != SALTS_OK ||
      external_https_json_u64(json_object_get(assertion_object, "expires_at"),
                              &assertion.expires_at) != SALTS_OK ||
      external_https_json_u64(json_object_get(assertion_object, "revision"),
                              &assertion.revision) != SALTS_OK ||
      external_https_json_u64(json_object_get(assertion_object, "assurance_level"),
                              &assurance) != SALTS_OK ||
      assurance < FLOWIE_CONTROL_EXTERNAL_ASSURANCE_SINGLE_FACTOR ||
      assurance > FLOWIE_CONTROL_EXTERNAL_ASSURANCE_HARDWARE_BOUND || assertion.issued_at == 0u ||
      assertion.expires_at <= assertion.issued_at || assertion.revision == 0u ||
      external_https_copy_groups(assertion_object, &assertion) != SALTS_OK)
    goto done;
  account_enabled = json_object_get(assertion_object, "account_enabled");
  if (!account_enabled || json_type(account_enabled) != JSON_BOOL) goto done;
  assertion.assurance_level = (uint32_t)assurance;
  assertion.account_enabled = json_bool(account_enabled) ? 1 : 0;
  *assertion_out = assertion;
  rc = SALTS_OK;

done:
  if (rc != SALTS_OK)
    *assertion_out =
        (flowie_control_external_auth_assertion_t)FLOWIE_CONTROL_EXTERNAL_AUTH_ASSERTION_INIT;
  json_free(document);
  return rc;
}

static int external_https_json_add(json_value_t *object, const char *field, json_value_t *value) {
  if (value && json_object_add_checked(object, field, value)) return SALTS_OK;
  if (value) {
    json_value_t *owned = (json_value_t *)value;
    json_free(owned);
  }
  return SALTS_ENOMEM;
}

static int external_https_request_valid(const flowie_control_external_auth_request_t *request) {
  return request && request->size >= sizeof(*request) &&
         external_https_text_valid(request->domain_id, FLOWIE_SECURITY_ID_MAX, 0) &&
         external_https_text_valid(request->presented_identity, FLOWIE_SECURITY_ID_MAX, 1) &&
         external_https_text_valid(request->method, FLOWIE_SECURITY_TYPE_MAX, 1) &&
         request->secret && request->secret_size > 0u &&
         request->secret_size <= FLOWIE_CONTROL_EXTERNAL_HTTPS_MAX_SECRET_SIZE &&
         external_https_text_valid(request->protocol, FLOWIE_SECURITY_TYPE_MAX, 1) &&
         external_https_text_valid(request->remote_address, FLOWIE_CONTROL_AUTH_REMOTE_ADDRESS_MAX,
                                   1) &&
         external_https_fingerprint_valid(request->peer_certificate_sha256);
}

int flowie_control_external_https_encode_request(
    const flowie_control_external_auth_request_t *request, char **body_out, size_t *body_size_out) {
  json_value_t *document = NULL;
  char *secret_base64 = NULL;
  int rc = SALTS_ENOMEM;
  if (body_out) *body_out = NULL;
  if (body_size_out) *body_size_out = 0u;
  if (!external_https_request_valid(request) || !body_out || !body_size_out) return SALTS_EINVAL;
  if (tn_base64_encode(request->secret, request->secret_size, &secret_base64) != 0 ||
      !secret_base64)
    return SALTS_ENOMEM;
  document = (json_value_t *)json_create_object();
  if (!document) goto done;
  if (external_https_json_add(
          document, "version",
          json_create_uint64(FLOWIE_CONTROL_EXTERNAL_HTTPS_PROTOCOL_VERSION)) != SALTS_OK ||
      external_https_json_add(document, "domain", json_create_string(request->domain_id)) !=
          SALTS_OK ||
      external_https_json_add(document, "identity",
                              json_create_string(request->presented_identity)) != SALTS_OK ||
      external_https_json_add(document, "method", json_create_string(request->method)) !=
          SALTS_OK ||
      external_https_json_add(document, "secret_base64", json_create_string(secret_base64)) !=
          SALTS_OK ||
      external_https_json_add(document, "protocol", json_create_string(request->protocol)) !=
          SALTS_OK ||
      external_https_json_add(document, "remote_address",
                              json_create_string(request->remote_address)) != SALTS_OK ||
      external_https_json_add(document, "peer_certificate_sha256",
                              json_create_string(request->peer_certificate_sha256
                                                           ? request->peer_certificate_sha256
                                                           : "")) != SALTS_OK)
    goto done;
  *body_out = json_serialize(document, body_size_out);
  if (!*body_out) goto done;
  rc = SALTS_OK;

done:
  if (secret_base64) {
    crypto_wipe(secret_base64, strlen(secret_base64));
    free(secret_base64);
  }
  json_free(document);
  return rc;
}

static int external_https_content_type_json(const chttp_response *response) {
  const char *content_type = chttp_response_header(response, "Content-Type");
  size_t json_size = sizeof("application/json") - 1u;
  return content_type && external_https_ascii_prefix(content_type, strlen(content_type),
                                                     "application/json") &&
         (content_type[json_size] == '\0' || content_type[json_size] == ';');
}

static int external_https_endpoint_text(char *connection_uri, size_t connection_capacity,
                                        char *authority, size_t authority_capacity,
                                        const char *host, uint16_t port) {
  const int ipv6_literal = host && strchr(host, ':') != NULL;
  int connection_size;
  int authority_size;
  if (!connection_uri || connection_capacity == 0u || !authority || authority_capacity == 0u ||
      !host || !host[0])
    return SALTS_EINVAL;
  connection_size = snprintf(connection_uri, connection_capacity,
                             ipv6_literal ? "tls://[%s]:%u" : "tls://%s:%u", host,
                             (unsigned int)port);
  if (port == 443u)
    authority_size = snprintf(authority, authority_capacity, ipv6_literal ? "[%s]" : "%s", host);
  else
    authority_size = snprintf(authority, authority_capacity,
                              ipv6_literal ? "[%s]:%u" : "%s:%u", host,
                              (unsigned int)port);
  if (connection_size <= 0 || (size_t)connection_size >= connection_capacity ||
      authority_size <= 0 || (size_t)authority_size >= authority_capacity)
    return SALTS_ERANGE;
  return SALTS_OK;
}

static int external_https_validate_url(const char *url, tstr *host_out, uint16_t *port_out) {
  uri_t uri = {0};
  const char *scheme;
  const char *host;
  const char *path;
  int port;
  int rc = SALTS_EINVAL;
  if (!url || !url[0] || !host_out || !port_out || !uri_parse(url, &uri))
    return SALTS_EINVAL;
  scheme = uri.scheme;
  host = uri.host;
  path = uri.path;
  port = uri.port;
  if (!uri.valid || strcmp(scheme, "https") != 0 || !host[0] || path[0] != '/' || !path[1] ||
      uri.userinfo[0] || uri.query[0] || uri.fragment[0] || port < 0 || port > UINT16_MAX)
    goto done;
  *host_out = tstr_dup(host);
  if (!*host_out) {
    rc = SALTS_ENOMEM;
    goto done;
  }
  *port_out = port == 0 ? 443u : (uint16_t)port;
  rc = SALTS_OK;

done:
  return rc;
}

static int external_https_tls_config_valid(const flowie_control_external_https_tls_config_t *tls) {
  int has_cert;
  int has_key;
  if (!tls) return 1;
  has_cert = tls->client_cert_file != NULL;
  has_key = tls->client_key_file != NULL;
  return has_cert == has_key && (!tls->client_key_password_ref || has_key) &&
         external_https_optional_path_valid(tls->ca_file) &&
         external_https_optional_path_valid(tls->client_cert_file) &&
         external_https_optional_path_valid(tls->client_key_file) &&
         external_https_optional_path_valid(tls->client_key_password_ref);
}

static void external_https_tls_cleanup(flowie_control_external_https_tls_t *tls) {
  if (!tls) return;
  tstr_freep(&tls->ca_file);
  tstr_freep(&tls->client_cert_file);
  tstr_freep(&tls->client_key_file);
  tstr_freep(&tls->client_key_password_ref);
}

static int external_https_tls_init(flowie_control_external_https_tls_t *tls,
                                   const flowie_control_external_https_tls_config_t *config) {
  flowie_control_external_https_tls_t next = {0};
  if (!tls || !external_https_tls_config_valid(config)) return SALTS_EINVAL;
  if (!config) {
    *tls = next;
    return SALTS_OK;
  }
  next.ca_file = config->ca_file ? tstr_dup(config->ca_file) : NULL;
  next.client_cert_file = config->client_cert_file ? tstr_dup(config->client_cert_file) : NULL;
  next.client_key_file = config->client_key_file ? tstr_dup(config->client_key_file) : NULL;
  next.client_key_password_ref =
      config->client_key_password_ref ? tstr_dup(config->client_key_password_ref) : NULL;
  if ((config->ca_file && !next.ca_file) || (config->client_cert_file && !next.client_cert_file) ||
      (config->client_key_file && !next.client_key_file) ||
      (config->client_key_password_ref && !next.client_key_password_ref)) {
    external_https_tls_cleanup(&next);
    return SALTS_ENOMEM;
  }
  *tls = next;
  return SALTS_OK;
}

static int external_https_secret_valid(const flowie_security_secret_lease_t *lease) {
  return lease && lease->bytes && lease->byte_count > 0u &&
         lease->byte_count <= FLOWIE_CONTROL_EXTERNAL_HTTPS_TOKEN_MAX &&
         !memchr(lease->bytes, '\0', lease->byte_count) &&
         !memchr(lease->bytes, '\r', lease->byte_count) &&
         !memchr(lease->bytes, '\n', lease->byte_count);
}

static int external_https_tls_apply(const flowie_control_external_https_tls_t *tls,
                                    const flowie_security_key_provider_t *key_provider,
                                    const char *server_name,
                                    chttp_tls_profile *profile) {
  flowie_security_secret_lease_t lease = FLOWIE_SECURITY_SECRET_LEASE_INIT;
  cnet_tls_client_config config = {0};
  char *password = NULL;
  int rc = SALTS_OK;
  if (!tls || !key_provider || !server_name || !server_name[0] || !profile) return SALTS_EINVAL;
  if (tls->client_key_password_ref) {
    rc = flowie_security_secret_acquire(key_provider, tls->client_key_password_ref, &lease);
    if (rc != SALTS_OK) goto done;
    if (!external_https_secret_valid(&lease)) {
      rc = SALTS_EPERM;
      goto done;
    }
    password = (char *)malloc(lease.byte_count + 1u);
    if (!password) {
      rc = SALTS_ENOMEM;
      goto done;
    }
    memcpy(password, lease.bytes, lease.byte_count);
    password[lease.byte_count] = '\0';
  }
  config = (cnet_tls_client_config){.size = sizeof(config),
                                    .ca_file = tls->ca_file,
                                    .cert_file = tls->client_cert_file,
                                    .key_file = tls->client_key_file,
                                    .key_password = password,
                                    .server_name = server_name};
  rc = chttp_tls_profile_init(profile, &config);

done:
  if (password) {
    crypto_wipe(password, lease.byte_count);
    free(password);
  }
  flowie_security_secret_release(key_provider, &lease);
  return rc;
}

static native_io_backend_kind external_https_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static chttp_client_config external_https_client_config(
    const flowie_control_external_https_authenticator_t *authenticator) {
  size_t max_request = FLOWIE_CONTROL_EXTERNAL_HTTPS_MAX_SECRET_SIZE * 2u + 8192u;
  return (chttp_client_config){
      .network = {.backend = external_https_backend(),
                  .connection_capacity = 1u,
                  .command_capacity = 8u,
                  .request_capacity = 4u,
                  .completion_batch_capacity = 4u,
                  .event_capacity = 8u,
                  .max_send_bytes = max_request + FLOWIE_CONTROL_EXTERNAL_HTTPS_HEADER_LIMIT,
                  .receive_buffer_bytes = 65536u,
                  .connect_timeout_ms = authenticator->timeout_ms,
                  .read_timeout_ms = authenticator->timeout_ms,
                  .write_timeout_ms = authenticator->timeout_ms,
                  .tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES,
                  .tls_handshake_timeout_ms = authenticator->timeout_ms},
      .request_capacity = 1u,
      .max_start_line_bytes = 2048u,
      .max_header_count = 16u,
      .max_header_bytes = FLOWIE_CONTROL_EXTERNAL_HTTPS_HEADER_LIMIT,
      .max_request_body_bytes = max_request,
      .max_response_body_bytes = authenticator->max_response_size,
      .max_informational_responses = 4u};
}

static int external_https_tls_files_validate(const flowie_control_external_https_tls_t *tls,
                                             const flowie_security_key_provider_t *key_provider) {
  flowie_security_secret_lease_t lease = FLOWIE_SECURITY_SECRET_LEASE_INIT;
  SSL_CTX *context = NULL;
  char *password = NULL;
  int rc = SALTS_EIO;
  if (!tls || !key_provider) return SALTS_EINVAL;
  if (!tls->ca_file && !tls->client_cert_file) return SALTS_OK;
  context = SSL_CTX_new(TLS_client_method());
  if (!context) goto done;
  if (tls->ca_file && SSL_CTX_load_verify_locations(context, tls->ca_file, NULL) != 1) goto done;
  if (!tls->client_cert_file) {
    rc = SALTS_OK;
    goto done;
  }
  if (tls->client_key_password_ref) {
    rc = flowie_security_secret_acquire(key_provider, tls->client_key_password_ref, &lease);
    if (rc != SALTS_OK) goto done;
    if (!external_https_secret_valid(&lease)) {
      rc = SALTS_EPERM;
      goto done;
    }
    password = (char *)malloc(lease.byte_count + 1u);
    if (!password) {
      rc = SALTS_ENOMEM;
      goto done;
    }
    memcpy(password, lease.bytes, lease.byte_count);
    password[lease.byte_count] = '\0';
    SSL_CTX_set_default_passwd_cb_userdata(context, password);
  }
  rc = SALTS_EIO;
  if (SSL_CTX_use_certificate_chain_file(context, tls->client_cert_file) != 1 ||
      SSL_CTX_use_PrivateKey_file(context, tls->client_key_file, SSL_FILETYPE_PEM) != 1 ||
      SSL_CTX_check_private_key(context) != 1)
    goto done;
  rc = SALTS_OK;

done:
  SSL_CTX_free(context);
  if (password) {
    crypto_wipe(password, lease.byte_count);
    free(password);
  }
  flowie_security_secret_release(key_provider, &lease);
  return rc;
}

static int external_https_verify(void *ctx, const flowie_control_external_auth_request_t *request,
                                 flowie_control_external_auth_assertion_t *assertion_out) {
  flowie_control_external_https_authenticator_t *authenticator =
      (flowie_control_external_https_authenticator_t *)ctx;
  flowie_security_secret_lease_t lease = FLOWIE_SECURITY_SECRET_LEASE_INIT;
  chttp_client client = {0};
  chttp_tls_profile tls_profile = {0};
  chttp_response response = {0};
  chttp_error error = {0};
  chttp_client_config client_config;
  chttp_options options;
  uri_t uri = {0};
  char *authorization = NULL;
  char *body = NULL;
  size_t body_size = 0u;
  size_t token_size = 0u;
  chttp_header headers[3];
  char connection_uri[320];
  char authority[320];
  unsigned int in_flight;
  external_https_outcome_t outcome = EXTERNAL_HTTPS_OUTCOME_LOCAL_FAILURE;
  int admitted = 0;
  int rc = SALTS_EIO;
  if (assertion_out && assertion_out->size >= sizeof(*assertion_out))
    *assertion_out =
        (flowie_control_external_auth_assertion_t)FLOWIE_CONTROL_EXTERNAL_AUTH_ASSERTION_INIT;
  if (!authenticator || !external_https_request_valid(request) || !assertion_out ||
      assertion_out->size < sizeof(*assertion_out) ||
      strcmp(request->method, authenticator->method) != 0)
    return SALTS_EINVAL;
  external_https_counter_increment(&authenticator->started_requests);
  in_flight = atomic_load_explicit(&authenticator->in_flight, memory_order_relaxed);
  do {
    if (in_flight >= authenticator->max_in_flight) {
      external_https_record_outcome(authenticator, EXTERNAL_HTTPS_OUTCOME_LOCAL_OVERLOAD);
      return SALTS_EBUSY;
    }
  } while (!atomic_compare_exchange_weak_explicit(&authenticator->in_flight, &in_flight,
                                                  in_flight + 1u, memory_order_acq_rel,
                                                  memory_order_relaxed));
  admitted = 1;
  rc = flowie_security_secret_acquire(&authenticator->key_provider,
                                      authenticator->service_token_ref, &lease);
  if (rc != SALTS_OK) goto done;
  token_size = lease.byte_count;
  if (!external_https_secret_valid(&lease)) {
    rc = SALTS_EPERM;
    goto done;
  }
  authorization = (char *)malloc(sizeof("Bearer ") + token_size);
  if (!authorization) {
    rc = SALTS_ENOMEM;
    goto done;
  }
  memcpy(authorization, "Bearer ", sizeof("Bearer ") - 1u);
  memcpy(authorization + sizeof("Bearer ") - 1u, lease.bytes, token_size);
  authorization[sizeof("Bearer ") - 1u + token_size] = '\0';
  rc = flowie_control_external_https_encode_request(request, &body, &body_size);
  if (rc != SALTS_OK) goto done;
  if (!uri_parse(authenticator->url, &uri) || !uri.valid) {
    rc = SALTS_EINVAL;
    goto done;
  }
  rc = external_https_endpoint_text(connection_uri, sizeof(connection_uri), authority,
                                    sizeof(authority), authenticator->host, authenticator->port);
  if (rc != SALTS_OK) goto done;
  rc = external_https_tls_apply(&authenticator->tls, &authenticator->key_provider,
                                authenticator->host, &tls_profile);
  if (rc != SALTS_OK) goto done;
  client_config = external_https_client_config(authenticator);
  rc = chttp_client_init(&client, &client_config);
  if (rc != SALTS_OK) goto done;
  headers[0] = (chttp_header){"Content-Type", "application/json"};
  headers[1] = (chttp_header){"Accept", "application/json"};
  headers[2] = (chttp_header){"Authorization", authorization};
  options = (chttp_options){.connection_uri = connection_uri,
                            .authority = authority,
                            .target = uri.path,
                            .headers = headers,
                            .header_count = 3u,
                            .body = body,
                            .body_size = body_size,
                            .timeout_ms = authenticator->timeout_ms,
                            .tls = &tls_profile,
                            .protocol = CHTTP_HTTP_1_1};
  rc = chttp_post(&client, &options, &response, &error);
  if (rc != SALTS_OK) {
    outcome = EXTERNAL_HTTPS_OUTCOME_TRANSPORT_FAILURE;
    rc = SALTS_EIO;
    goto done;
  }
  if (response.status_code == 401u || response.status_code == 403u) {
    outcome = EXTERNAL_HTTPS_OUTCOME_DENIED;
    rc = SALTS_EPERM;
    goto done;
  }
  if (response.status_code == 429u) {
    outcome = EXTERNAL_HTTPS_OUTCOME_REMOTE_OVERLOAD;
    rc = SALTS_EBUSY;
    goto done;
  }
  if (response.status_code >= 500u && response.status_code <= 599u) {
    outcome = EXTERNAL_HTTPS_OUTCOME_REMOTE_SERVER_FAILURE;
    rc = SALTS_EIO;
    goto done;
  }
  if (response.status_code != 200u || !external_https_content_type_json(&response)) {
    outcome = EXTERNAL_HTTPS_OUTCOME_PROTOCOL_FAILURE;
    rc = SALTS_EIO;
    goto done;
  }
  if (!response.body || response.body_size == 0u) {
    outcome = EXTERNAL_HTTPS_OUTCOME_PROTOCOL_FAILURE;
    rc = SALTS_EPROTO;
    goto done;
  }
  rc = flowie_control_external_https_decode_response((const char *)response.body,
                                                     response.body_size,
                                                     authenticator->method, assertion_out);
  if (rc == SALTS_OK) outcome = EXTERNAL_HTTPS_OUTCOME_SUCCEEDED;
  else if (rc == SALTS_EPERM) outcome = EXTERNAL_HTTPS_OUTCOME_DENIED;
  else outcome = EXTERNAL_HTTPS_OUTCOME_PROTOCOL_FAILURE;

done:
  chttp_response_destroy(&response);
  if (client.impl != NULL)
    (void)chttp_client_destroy(&client, FLOWIE_CONTROL_EXTERNAL_HTTPS_DESTROY_TIMEOUT_MS);
  (void)chttp_tls_profile_destroy(&tls_profile);
  if (body) {
    crypto_wipe(body, body_size);
    json_serialize_free(body);
  }
  if (authorization) {
    crypto_wipe(authorization, sizeof("Bearer ") + token_size);
    free(authorization);
  }
  flowie_security_secret_release(&authenticator->key_provider, &lease);
  if (admitted)
    (void)atomic_fetch_sub_explicit(&authenticator->in_flight, 1u, memory_order_release);
  external_https_record_outcome(authenticator, outcome);
  return rc;
}

int flowie_control_external_https_authenticator_create(
    const flowie_control_external_https_authenticator_config_t *config,
    flowie_control_external_https_authenticator_t **out) {
  flowie_control_external_https_authenticator_t *authenticator = NULL;
  flowie_security_secret_lease_t lease = FLOWIE_SECURITY_SECRET_LEASE_INIT;
  int rc;
  if (out) *out = NULL;
  if (!config || config->size < sizeof(*config) || !out ||
      !external_https_text_valid(config->url, FLOWIE_CONTROL_EXTERNAL_HTTPS_URL_MAX, 1) ||
      !external_https_text_valid(config->method, FLOWIE_SECURITY_TYPE_MAX, 1) ||
      !external_https_text_valid(config->service_token_ref,
                                 FLOWIE_CONTROL_EXTERNAL_HTTPS_TLS_PATH_MAX, 1) ||
      config->timeout_ms == 0u ||
      config->timeout_ms > FLOWIE_CONTROL_EXTERNAL_HTTPS_MAX_TIMEOUT_MS ||
      config->max_response_size == 0u ||
      config->max_response_size > FLOWIE_CONTROL_EXTERNAL_HTTPS_MAX_RESPONSE_SIZE ||
      config->max_in_flight == 0u ||
      config->max_in_flight > FLOWIE_CONTROL_EXTERNAL_HTTPS_MAX_IN_FLIGHT ||
      config->key_provider.size < sizeof(config->key_provider) || !config->key_provider.acquire ||
      !config->key_provider.release || !external_https_tls_config_valid(&config->tls))
    return SALTS_EINVAL;
  authenticator =
      (flowie_control_external_https_authenticator_t *)calloc(1u, sizeof(*authenticator));
  if (!authenticator) return SALTS_ENOMEM;
  authenticator->url = config->url ? tstr_dup(config->url) : NULL;
  authenticator->method = tstr_dup(config->method);
  authenticator->service_token_ref = tstr_dup(config->service_token_ref);
  authenticator->timeout_ms = config->timeout_ms;
  authenticator->max_response_size = config->max_response_size;
  authenticator->max_in_flight = config->max_in_flight;
  atomic_init(&authenticator->in_flight, 0u);
  atomic_init(&authenticator->started_requests, 0u);
  atomic_init(&authenticator->succeeded, 0u);
  atomic_init(&authenticator->denied, 0u);
  atomic_init(&authenticator->local_overload, 0u);
  atomic_init(&authenticator->remote_overload, 0u);
  atomic_init(&authenticator->remote_server_failures, 0u);
  atomic_init(&authenticator->transport_failures, 0u);
  atomic_init(&authenticator->protocol_failures, 0u);
  atomic_init(&authenticator->local_failures, 0u);
  authenticator->key_provider = config->key_provider;
  if (!authenticator->url || !authenticator->method || !authenticator->service_token_ref) {
    rc = SALTS_ENOMEM;
    goto fail;
  }
  rc = external_https_tls_init(&authenticator->tls, &config->tls);
  if (rc != SALTS_OK) goto fail;
  rc = external_https_validate_url(authenticator->url, &authenticator->host, &authenticator->port);
  if (rc != SALTS_OK) goto fail;
  rc = flowie_security_secret_acquire(&authenticator->key_provider,
                                      authenticator->service_token_ref, &lease);
  if (rc != SALTS_OK) goto fail;
  if (!external_https_secret_valid(&lease)) {
    rc = SALTS_EPERM;
    goto fail;
  }
  flowie_security_secret_release(&authenticator->key_provider, &lease);
  rc = external_https_tls_files_validate(&authenticator->tls, &authenticator->key_provider);
  if (rc != SALTS_OK) goto fail;
  authenticator->interface =
      (flowie_control_external_authenticator_t)FLOWIE_CONTROL_EXTERNAL_AUTHENTICATOR_INIT;
  authenticator->interface.capabilities = FLOWIE_CONTROL_EXTERNAL_AUTH_REQUIRED_CAPABILITIES |
                                          FLOWIE_CONTROL_EXTERNAL_AUTH_GROUP_CLAIMS;
  authenticator->interface.ctx = authenticator;
  authenticator->interface.method = authenticator->method;
  authenticator->interface.verify = external_https_verify;
  *out = authenticator;
  return SALTS_OK;

fail:
  flowie_security_secret_release(&authenticator->key_provider, &lease);
  flowie_control_external_https_authenticator_destroy(authenticator);
  return rc;
}

void flowie_control_external_https_authenticator_destroy(
    flowie_control_external_https_authenticator_t *authenticator) {
  if (!authenticator) return;
  tstr_freep(&authenticator->url);
  tstr_freep(&authenticator->host);
  tstr_freep(&authenticator->method);
  tstr_freep(&authenticator->service_token_ref);
  external_https_tls_cleanup(&authenticator->tls);
  crypto_wipe(&authenticator->key_provider, sizeof(authenticator->key_provider));
  crypto_wipe(&authenticator->interface, sizeof(authenticator->interface));
  free(authenticator);
}

const flowie_control_external_authenticator_t *
flowie_control_external_https_authenticator_interface(
    const flowie_control_external_https_authenticator_t *authenticator) {
  return authenticator ? &authenticator->interface : NULL;
}

int flowie_control_external_https_authenticator_get_stats(
    const flowie_control_external_https_authenticator_t *authenticator,
    flowie_control_external_https_authenticator_stats_t *stats_out) {
  flowie_control_external_https_authenticator_stats_t stats =
      FLOWIE_CONTROL_EXTERNAL_HTTPS_AUTHENTICATOR_STATS_INIT;
  if (stats_out && stats_out->size >= sizeof(*stats_out)) *stats_out = stats;
  if (!authenticator || !stats_out || stats_out->size < sizeof(*stats_out)) return SALTS_EINVAL;
  stats.started_requests =
      (uint64_t)atomic_load_explicit(&authenticator->started_requests, memory_order_relaxed);
  stats.in_flight = (uint64_t)atomic_load_explicit(&authenticator->in_flight, memory_order_relaxed);
  stats.succeeded = (uint64_t)atomic_load_explicit(&authenticator->succeeded, memory_order_relaxed);
  stats.denied = (uint64_t)atomic_load_explicit(&authenticator->denied, memory_order_relaxed);
  stats.local_overload =
      (uint64_t)atomic_load_explicit(&authenticator->local_overload, memory_order_relaxed);
  stats.remote_overload =
      (uint64_t)atomic_load_explicit(&authenticator->remote_overload, memory_order_relaxed);
  stats.remote_server_failures =
      (uint64_t)atomic_load_explicit(&authenticator->remote_server_failures, memory_order_relaxed);
  stats.transport_failures =
      (uint64_t)atomic_load_explicit(&authenticator->transport_failures, memory_order_relaxed);
  stats.protocol_failures =
      (uint64_t)atomic_load_explicit(&authenticator->protocol_failures, memory_order_relaxed);
  stats.local_failures =
      (uint64_t)atomic_load_explicit(&authenticator->local_failures, memory_order_relaxed);
  *stats_out = stats;
  return SALTS_OK;
}
