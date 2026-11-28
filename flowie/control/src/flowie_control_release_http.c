#include "flowie_control_release_http_internal.h"

#include "flowie_control_release_ui_assets_internal.h"
#include "flowie_control_validation_internal.h"

#include <chttp_app/application.h>
#include <core/error.h>
#include <core/tlog.h>
#include <crypto/sha256.h>
#include <json_parser.h>

#include <inttypes.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RELEASE_ROUTE "/domains/:domain/releases"
#define ACTIVE_ROUTE "/domains/:domain/active-release"
#define PACKAGE_UI_ROUTE "/domains/:domain/packages"
#define RELEASE_TYPE "application/zstd"
#define JSON_TYPE "application/json"

enum {
  DIGEST_HEX_LENGTH = FLOWIE_RELEASE_DIGEST_BYTES * 2,
  ACTIVATION_BODY_LIMIT = 256,
  HTTP_OVERHEAD_BYTES = 64 * 1024
};

struct flowie_control_release_http_s {
  int configured;
  chttp_application app;
  salts_configurator_provider_binding provider;
  salts_configurator_deployment deployment;
  flowie_control_release_http_config_t config;
  const flowie_control_release_mqtt_config_t *scope;
  char signer_certificate_sha256[DIGEST_HEX_LENGTH + 1];
};

cmeta_reflect_data(flowie_control_release_http_t, "flowie.control.ReleaseHttp",
                  cmeta_field(int, configured));
cmeta_component_empty(FlowieReleaseHttpComponent);

static cmeta_status flowie_release_http_component(void *user, const cmeta_data_desc *data,
    const void *config, const salts_configurator_dependency *dependencies, size_t count,
    cmeta_object_ref *out) {
  (void)data;
  (void)config;
  (void)dependencies;
  (void)count;
  return cmeta_object_borrow(out, user, cmeta_reflected_data(flowie_control_release_http_t), NULL);
}

static int flowie_release_http_nibble(char digit) {
  if (digit >= '0' && digit <= '9') return digit - '0';
  if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
  return -1;
}

static int flowie_release_http_decode(const char *hex, uint8_t *digest) {
  if (!hex || strlen(hex) != DIGEST_HEX_LENGTH) return SALTS_EINVAL;
  for (size_t i = 0; i < FLOWIE_RELEASE_DIGEST_BYTES; ++i) {
    const int high = flowie_release_http_nibble(hex[2u * i]);
    const int low = flowie_release_http_nibble(hex[2u * i + 1u]);
    if (high < 0 || low < 0) return SALTS_EINVAL;
    digest[i] = (uint8_t)((high << 4) | low);
  }
  return SALTS_OK;
}

static void flowie_release_http_encode(const uint8_t *digest, char hex[DIGEST_HEX_LENGTH + 1]) {
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < FLOWIE_RELEASE_DIGEST_BYTES; ++i) {
    hex[2u * i] = digits[digest[i] >> 4];
    hex[2u * i + 1u] = digits[digest[i] & 15u];
  }
  hex[DIGEST_HEX_LENGTH] = 0;
}

static void flowie_release_http_audit(const flowie_control_release_http_t *server,
    const char *actor, const char *operation, const char *digest, int status) {
  SALTS_LOG_INFOF(tlog_peek_default(), "Flowie.Release",
      "operation={} domain={} environment={} actor={} digest={} status={}",
      operation, server->scope->domain, server->scope->environment, actor, digest, status);
}

static int flowie_release_http_error(const chttp_invocation *invocation, unsigned status,
                                     const char *code) {
  char body[96];
  const int size = snprintf(body, sizeof(body), "{\"code\":\"%s\"}", code);
  return chttp_invocation_respond(invocation, status, JSON_TYPE, body, (size_t)size);
}

static int flowie_release_http_access_result(const chttp_invocation *invocation, int rc) {
  if (rc == SALTS_EAUTH) return flowie_release_http_error(invocation, 401, "release_credentials_required");
  if (rc == SALTS_EPERM) return flowie_release_http_error(invocation, 403, "release_access_denied");
  return flowie_release_http_error(invocation, 503, "release_authority_unavailable");
}

static int flowie_release_http_field(const chttp_server_request_view *request, const char *name,
    const char **value) {
  *value = NULL;
  for (size_t i = 0; i < request->header_count; ++i) {
    const unsigned char *a = (const unsigned char *)request->headers[i].name;
    const unsigned char *b = (const unsigned char *)name;
    while (*a && *b && tolower(*a) == tolower(*b)) { ++a; ++b; }
    if (*a || *b) continue;
    if (*value) return SALTS_EAUTH;
    *value = request->headers[i].value;
  }
  return SALTS_OK;
}

static int flowie_release_http_authorize(const flowie_control_release_http_t *server,
    const chttp_invocation *invocation, const char *action, char *actor) {
  char unused[FLOWIE_RELEASE_ACCESS_ACTOR_MAX + 1u] = {0};
  if (!actor) actor = unused;
  actor[0] = '\0';
  if (!server->config.authorize) {
    strcpy(actor, server->config.operator_id);
    return SALTS_OK;
  }
  const chttp_server_request_view *request = chttp_invocation_request(invocation);
  flowie_release_access_request_t access = {.domain = server->scope->domain, .action = action};
  if (flowie_release_http_field(request, "Authorization", &access.authorization) != SALTS_OK ||
      !access.authorization || strncmp(access.authorization, "Bearer ", 7u) != 0 ||
      !access.authorization[7] || strlen(access.authorization) > FLOWIE_RELEASE_ACCESS_TOKEN_MAX + 7u ||
      flowie_release_http_field(request, "X-Flowie-Service-Domain", &access.service_domain) != SALTS_OK ||
      flowie_release_http_field(request, "X-Flowie-Service-Id", &access.service_id) != SALTS_OK ||
      !!access.service_id != !!access.service_domain)
    return SALTS_EAUTH;
  const int rc = server->config.authorize(server->config.authorize_context, &access, actor);
  if (rc != SALTS_OK) return rc;
  return flowie_control_text_valid(actor, FLOWIE_RELEASE_ACCESS_ACTOR_MAX) ? SALTS_OK : SALTS_EPROTO;
}

#define FLOWIE_REQUIRE_RELEASE_ACCESS(action, actor) \
  do { \
    const int access_rc = flowie_release_http_authorize(server, invocation, action, actor); \
    if (access_rc != SALTS_OK) return flowie_release_http_access_result(invocation, access_rc); \
  } while (0)

static int flowie_release_http_result(const chttp_invocation *invocation, int rc) {
  switch (rc) {
  case SALTS_ENOENT: return flowie_release_http_error(invocation, 404, "release_not_found");
  case SALTS_EPERM: return flowie_release_http_error(invocation, 422, "release_trust_rejected");
  case SALTS_ENOTSUP: return flowie_release_http_error(invocation, 422, "rulesforge_runtime_required");
  case SALTS_EINVAL:
  case SALTS_EPROTONOSUPPORT:
  case SALTS_EPROTO: return flowie_release_http_error(invocation, 422, "release_content_rejected");
  case SALTS_ENOSPC: return flowie_release_http_error(invocation, 413, "release_limit_exceeded");
  case SALTS_EALREADY: return flowie_release_http_error(invocation, 409, "idempotency_conflict");
  case SALTS_ECANCELED: return flowie_release_http_error(invocation, 409, "release_base_conflict");
  case SALTS_EAUTH: return flowie_release_http_error(invocation, 409, "validation_required");
  case SALTS_EBUSY:
  case SALTS_ESHUTDOWN:
  case SALTS_ENOMEM:
  case SALTS_ENOBUFS: return flowie_release_http_error(invocation, 503, "release_unavailable");
  default: return flowie_release_http_error(invocation, 500, "release_internal_error");
  }
}

static int flowie_release_http_same(const char *value, const char *expected) {
  return value && strcmp(value, expected) == 0;
}

static int flowie_release_http_read_verified(const flowie_control_release_mqtt_config_t *scope,
    const uint8_t *digest, time_t now, flowie_release_archive_t **out) {
  void *bytes = NULL;
  size_t size = 0;
  int rc = flowie_control_release_store_read(scope->store, scope->environment, scope->domain,
      digest, &bytes, &size);
  if (rc != SALTS_OK) return rc;
  salts_crypto_sm2_trust_policy trust = *scope->signer->trust;
  salts_crypto_sm2_cms_verify_config signer = *scope->signer;
  trust.validation_time = (int64_t)now;
  signer.trust = &trust;
  rc = flowie_control_release_archive_verify(&signer, scope->domain, scope->environment,
      bytes, size, out);
  free(bytes);
  return rc;
}

static int flowie_release_ui_respond(const chttp_invocation *invocation, const char *type,
    const void *body, size_t size) {
  int rc = chttp_invocation_set_header(invocation, "Cache-Control", "private, no-store");
  if (rc == SALTS_OK)
    rc = chttp_invocation_set_header(invocation, "X-Content-Type-Options", "nosniff");
  return rc == SALTS_OK ? chttp_invocation_respond(invocation, 200, type, body, size) : rc;
}

CHTTP_ACTION(flowie_release_http_ui);
int flowie_release_http_ui(void *self, const chttp_invocation *invocation) {
  (void)self;
  const int rc = chttp_invocation_set_header(invocation, "Content-Security-Policy",
      "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; "
      "base-uri 'none'; frame-ancestors 'none'; form-action 'none'");
  if (rc != SALTS_OK) return rc;
  size_t size = 0;
  const char *body = flowie_release_ui_html(&size);
  return flowie_release_ui_respond(invocation, "text/html; charset=utf-8", body, size);
}

CHTTP_ACTION(flowie_release_http_ui_css);
int flowie_release_http_ui_css(void *self, const chttp_invocation *invocation) {
  (void)self;
  size_t size = 0;
  const char *body = flowie_release_ui_css(&size);
  return flowie_release_ui_respond(invocation, "text/css; charset=utf-8", body, size);
}

CHTTP_ACTION(flowie_release_http_ui_js);
int flowie_release_http_ui_js(void *self, const chttp_invocation *invocation) {
  (void)self;
  size_t size = 0;
  const char *body = flowie_release_ui_js(&size);
  return flowie_release_ui_respond(invocation, "text/javascript; charset=utf-8", body, size);
}

CHTTP_ACTION(flowie_release_http_ui_context);
int flowie_release_http_ui_context(void *self, const chttp_invocation *invocation) {
  const flowie_control_release_http_t *server = self;
  char actor[FLOWIE_RELEASE_ACCESS_ACTOR_MAX + 1u];
  FLOWIE_REQUIRE_RELEASE_ACCESS(FLOWIE_RELEASE_ACCESS_READ, actor);
  json_value_t *document = json_create_object();
  if (!document) return flowie_release_http_result(invocation, SALTS_ENOMEM);
  const char *const keys[] = {"domain", "environment", "operator_id", "operator_certificate_sha256",
                              "signer_reference", "signer_certificate_sha256"};
  const char *const fields[] = {server->scope->domain, server->scope->environment,
      actor, chttp_invocation_request(invocation)->peer_certificate_sha256,
      server->config.signer_reference, server->signer_certificate_sha256};
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
    json_value_t *value = json_create_string(fields[i]);
    if (value && json_object_add_checked(document, keys[i], value)) continue;
    if (value) json_free(value);
    json_free(document);
    return flowie_release_http_result(invocation, SALTS_ENOMEM);
  }
  size_t size = 0;
  char *body = json_serialize(document, &size);
  json_free(document);
  if (!body) return flowie_release_http_result(invocation, SALTS_ENOMEM);
  int rc = chttp_invocation_set_header(invocation, "Flowie-Release-Signer",
                                       server->config.signer_reference);
  if (rc == SALTS_OK) rc = flowie_release_ui_respond(invocation, JSON_TYPE, body, size);
  json_serialize_free(body);
  return rc;
}

CHTTP_ACTION(flowie_release_http_preview_acl);
int flowie_release_http_preview_acl(void *self, const chttp_invocation *invocation) {
  const flowie_control_release_http_t *server = self;
  FLOWIE_REQUIRE_RELEASE_ACCESS(FLOWIE_RELEASE_ACCESS_READ, NULL);
  const char *hex = chttp_server_request_param(chttp_invocation_request(invocation), "digest");
  uint8_t digest[FLOWIE_RELEASE_DIGEST_BYTES];
  if (flowie_release_http_decode(hex, digest) != SALTS_OK)
    return flowie_release_http_error(invocation, 400, "invalid_digest");
  const time_t now = time(NULL);
  if (now == (time_t)-1) return flowie_release_http_error(invocation, 503, "clock_unavailable");
  flowie_release_archive_t *archive = NULL;
  int rc = flowie_release_http_read_verified(server->scope, digest, now, &archive);
  if (rc != SALTS_OK) return flowie_release_http_result(invocation, rc);

  const flowie_release_manifest_t *manifest = flowie_control_release_archive_manifest(archive);
  const char *path = manifest->files[manifest->acl_index].path;
  const void *body = NULL;
  size_t size = 0;
  rc = flowie_control_release_archive_file(archive, path, &body, &size);
  if (rc != SALTS_OK || !size || size > FLOWIE_CONTROL_ACL_DOCUMENT_MAX || memchr(body, 0, size)) {
    flowie_control_release_archive_destroy(archive);
    return flowie_release_http_error(invocation, 422, "invalid_acl_text");
  }
  rc = chttp_invocation_set_header(invocation, "Flowie-ACL-Path", path);
  if (rc == SALTS_OK && manifest->name[0])
    rc = chttp_invocation_set_header(invocation, "Flowie-Package-Name", manifest->name);
  if (rc == SALTS_OK)
    rc = flowie_release_ui_respond(invocation, "text/plain; charset=utf-8", body, size);
  flowie_control_release_archive_destroy(archive);
  return rc;
}

CHTTP_ADMISSION_ACTION(flowie_release_http_admit);
int flowie_release_http_admit(void *self, const chttp_server_request_view *request,
                               chttp_server_admission_result *result) {
  const flowie_control_release_http_t *server = self;
  *result = (chttp_server_admission_result){0};
  if (!request->peer_certificate_sha256 ||
      (!server->config.authorize && !flowie_release_http_same(request->peer_certificate_sha256,
                                      server->config.operator_certificate_sha256))) {
    result->status_code = 403;
    return SALTS_OK;
  }
  const char *domain = chttp_server_request_param(request, "domain");
  if ((!domain && strcmp(request->path, "/healthz") != 0) ||
      (domain && strcmp(domain, server->scope->domain) != 0)) {
    result->status_code = 404;
    return SALTS_OK;
  }
  if (server->config.authorize && request->method != CHTTP_METHOD_GET) {
    const char *bearer;
    if (flowie_release_http_field(request, "Authorization", &bearer) != SALTS_OK ||
        !bearer || strncmp(bearer, "Bearer ", 7u) != 0 || !bearer[7]) {
      result->status_code = 401;
      return SALTS_OK;
    }
  }
  const char *encoding = chttp_server_request_header(request, "Content-Encoding");
  if (encoding && strcmp(encoding, "identity") != 0) {
    result->status_code = 415;
    return SALTS_OK;
  }
  if (request->method == CHTTP_METHOD_POST && domain && !chttp_server_request_param(request, "digest")) {
    if (!flowie_release_http_same(chttp_server_request_header(request, "Content-Type"), RELEASE_TYPE))
      result->status_code = 415;
    else if (!flowie_release_http_same(chttp_server_request_header(request, "Flowie-Release-Signer"),
                                       server->config.signer_reference))
      result->status_code = 403;
  } else if (request->method == CHTTP_METHOD_PUT &&
             !flowie_release_http_same(chttp_server_request_header(request, "Content-Type"), JSON_TYPE)) {
    result->status_code = 415;
  }
  return SALTS_OK;
}

CHTTP_ACTION(flowie_release_http_upload);
int flowie_release_http_upload(void *self, const chttp_invocation *invocation) {
  const flowie_control_release_http_t *server = self;
  char actor[FLOWIE_RELEASE_ACCESS_ACTOR_MAX + 1u];
  FLOWIE_REQUIRE_RELEASE_ACCESS(FLOWIE_RELEASE_ACCESS_UPLOAD, actor);
  const flowie_control_release_mqtt_config_t *scope = server->scope;
  const chttp_server_request_view *request = chttp_invocation_request(invocation);
  if (!request->body_size) return flowie_release_http_error(invocation, 400, "missing_release_body");
  const time_t now = time(NULL);
  if (now == (time_t)-1) return flowie_release_http_error(invocation, 503, "clock_unavailable");
  salts_crypto_sm2_trust_policy trust = *scope->signer->trust;
  salts_crypto_sm2_cms_verify_config signer = *scope->signer;
  trust.validation_time = (int64_t)now;
  signer.trust = &trust;
  flowie_control_release_receipt_t receipt;
  const int rc = flowie_control_release_store_admit(scope->store, scope->environment, scope->domain,
      server->config.signer_reference, &signer, request->body, request->body_size, &receipt);
  char digest[DIGEST_HEX_LENGTH + 1] = "-";
  if (rc == SALTS_OK) flowie_release_http_encode(receipt.digest, digest);
  flowie_release_http_audit(server, actor, FLOWIE_RELEASE_ACCESS_UPLOAD, digest, rc);
  if (rc == SALTS_ENOENT) return flowie_release_http_error(invocation, 422, "package_name_required");
  if (rc != SALTS_OK) return flowie_release_http_result(invocation, rc);
  char body[DIGEST_HEX_LENGTH + FLOWIE_RELEASE_MAX_NAME + 32],
      location[FLOWIE_SECURITY_ID_MAX + 96];
  const int size = receipt.name[0]
      ? snprintf(body, sizeof(body), "{\"digest\":\"%s\",\"name\":\"%s\"}", digest, receipt.name)
      : snprintf(body, sizeof(body), "{\"digest\":\"%s\"}", digest);
  snprintf(location, sizeof(location), "/domains/%s/releases/%s", scope->domain, digest);
  const int header = chttp_invocation_set_header(invocation, "Location", location);
  return header == SALTS_OK
      ? chttp_invocation_respond(invocation, receipt.created ? 201 : 200, JSON_TYPE, body, (size_t)size)
      : header;
}

CHTTP_ACTION(flowie_release_http_validate);
int flowie_release_http_validate(void *self, const chttp_invocation *invocation) {
  const flowie_control_release_http_t *server = self;
  char actor[FLOWIE_RELEASE_ACCESS_ACTOR_MAX + 1u];
  FLOWIE_REQUIRE_RELEASE_ACCESS(FLOWIE_RELEASE_ACCESS_VALIDATE, actor);
  const chttp_server_request_view *request = chttp_invocation_request(invocation);
  const char *hex = chttp_server_request_param(request, "digest");
  uint8_t digest[FLOWIE_RELEASE_DIGEST_BYTES];
  if (request->body_size || flowie_release_http_decode(hex, digest) != SALTS_OK)
    return flowie_release_http_error(invocation, 400, "invalid_validation_request");
  uint64_t generation = 0;
  int changed = 0;
  const int rc = flowie_control_release_mqtt_validate(server->config.mqtt, digest,
      server->config.validation_policy_revision, &generation, &changed);
  flowie_release_http_audit(server, actor, FLOWIE_RELEASE_ACCESS_VALIDATE, hex, rc);
  if (rc != SALTS_OK) return flowie_release_http_result(invocation, rc);
  char body[192];
  const int size = snprintf(body, sizeof(body),
      "{\"digest\":\"%s\",\"base_generation\":%" PRIu64 ",\"acl_changed\":%s,\"routes_changed\":false}",
      hex, generation, changed ? "true" : "false");
  return chttp_invocation_respond(invocation, 200, JSON_TYPE, body, (size_t)size);
}

static int flowie_release_http_active_reply(const chttp_invocation *invocation,
    const flowie_control_release_active_t *active) {
  char digest[DIGEST_HEX_LENGTH + 1], etag[32], body[160];
  flowie_release_http_encode(active->digest, digest);
  snprintf(etag, sizeof(etag), "\"release-%" PRIu64 "\"", active->generation);
  const int size = snprintf(body, sizeof(body),
      "{\"digest\":\"%s\",\"generation\":%" PRIu64 ",\"acl_version\":%" PRIu64 "}",
      digest, active->generation, active->acl_version);
  const int rc = chttp_invocation_set_header(invocation, "ETag", etag);
  return rc == SALTS_OK ? chttp_invocation_respond(invocation, 200, JSON_TYPE, body, (size_t)size) : rc;
}

/* Only an emitted strong generation tag or the explicit initial wildcard is valid. */
static unsigned flowie_release_http_precondition(const chttp_server_request_view *request,
                                                  uint64_t *generation) {
  const char *match = chttp_server_request_header(request, "If-Match");
  const char *none = chttp_server_request_header(request, "If-None-Match");
  if (!match && !none) return 428;
  *generation = 0;
  if (none) return !match && strcmp(none, "*") == 0 ? 0 : 400;
  static const char prefix[] = "\"release-";
  if (strncmp(match, prefix, sizeof(prefix) - 1u) != 0) return 400;
  const char *digit = match + sizeof(prefix) - 1u;
  if (*digit < '1' || *digit > '9') return 400;
  for (; *digit >= '0' && *digit <= '9'; ++digit) {
    const uint64_t value = (unsigned int)(*digit - '0');
    if (*generation > (INT64_MAX - value) / 10u) return 400;
    *generation = *generation * 10u + value;
  }
  return strcmp(digit, "\"") == 0 ? 0 : 400;
}

/* Activation accepts exactly one literal digest member and JSON whitespace. */
static const char *flowie_release_http_space(const char *input, const char *end) {
  while (input != end && (*input == ' ' || *input == '\n' || *input == '\r' || *input == '\t')) ++input;
  return input;
}

static int flowie_release_http_activation_digest(const chttp_server_request_view *request,
                                                  uint8_t *digest) {
  if (!request->body_size || request->body_size > ACTIVATION_BODY_LIMIT) return SALTS_EINVAL;
  const char *input = request->body;
  const char *end = input + request->body_size;
  input = flowie_release_http_space(input, end);
  if (input == end || *input++ != '{') return SALTS_EINVAL;
  input = flowie_release_http_space(input, end);
  static const char field[] = "\"digest\"";
  if ((size_t)(end - input) < sizeof(field) - 1u ||
      memcmp(input, field, sizeof(field) - 1u) != 0) return SALTS_EINVAL;
  input = flowie_release_http_space(input + sizeof(field) - 1u, end);
  if (input == end || *input++ != ':') return SALTS_EINVAL;
  input = flowie_release_http_space(input, end);
  if (end - input < DIGEST_HEX_LENGTH + 2 || *input++ != '"' || input[DIGEST_HEX_LENGTH] != '"')
    return SALTS_EINVAL;
  char hex[DIGEST_HEX_LENGTH + 1];
  memcpy(hex, input, DIGEST_HEX_LENGTH);
  hex[DIGEST_HEX_LENGTH] = 0;
  input = flowie_release_http_space(input + DIGEST_HEX_LENGTH + 1, end);
  if (input == end || *input++ != '}' || flowie_release_http_space(input, end) != end) return SALTS_EINVAL;
  return flowie_release_http_decode(hex, digest);
}

CHTTP_ACTION(flowie_release_http_activate);
int flowie_release_http_activate(void *self, const chttp_invocation *invocation) {
  const flowie_control_release_http_t *server = self;
  char actor[FLOWIE_RELEASE_ACCESS_ACTOR_MAX + 1u];
  FLOWIE_REQUIRE_RELEASE_ACCESS(FLOWIE_RELEASE_ACCESS_ACTIVATE, actor);
  const flowie_control_release_mqtt_config_t *scope = server->scope;
  const chttp_server_request_view *request = chttp_invocation_request(invocation);
  uint64_t generation = 0;
  const unsigned condition = flowie_release_http_precondition(request, &generation);
  if (condition) return flowie_release_http_error(invocation, condition,
      condition == 428 ? "precondition_required" : "invalid_precondition");
  const char *key = chttp_server_request_header(request, "Idempotency-Key");
  uint8_t digest[FLOWIE_RELEASE_DIGEST_BYTES];
  if (!flowie_control_text_valid(key, FLOWIE_SECURITY_ID_MAX) ||
      flowie_release_http_activation_digest(request, digest) != SALTS_OK)
    return flowie_release_http_error(invocation, 400, "invalid_activation_request");
  int replayed = 0;
  flowie_control_release_active_t active;
  int rc = flowie_control_release_mqtt_activate(server->config.mqtt, digest, actor,
      key, generation, server->config.validation_policy_revision, &active, &replayed);
  char hex[DIGEST_HEX_LENGTH + 1];
  flowie_release_http_encode(digest, hex);
  flowie_release_http_audit(server, actor, FLOWIE_RELEASE_ACCESS_ACTIVATE, hex, rc);
  if (rc == SALTS_ECANCELED) {
    flowie_control_release_active_t current;
    const int current_rc = flowie_control_release_store_current(scope->store,
        scope->environment, scope->domain, &current);
    if (current_rc != SALTS_OK) return flowie_release_http_result(invocation, current_rc);
    if (current.generation != generation)
      return flowie_release_http_error(invocation, 412, "stale_precondition");
  }
  if (rc != SALTS_OK) return flowie_release_http_result(invocation, rc);
  flowie_control_release_active_t current;
  int ready = 0;
  const int status = flowie_control_release_mqtt_status(server->config.mqtt, &current, &ready);
  rc = chttp_invocation_set_header(invocation, "Flowie-Runtime-Ready",
      status == SALTS_OK ? (ready && current.generation == active.generation ? "true" : "false") : "unknown");
  if (rc == SALTS_OK && replayed)
    rc = chttp_invocation_set_header(invocation, "Idempotency-Replayed", "true");
  return rc == SALTS_OK ? flowie_release_http_active_reply(invocation, &active) : rc;
}

CHTTP_ACTION(flowie_release_http_current);
int flowie_release_http_current(void *self, const chttp_invocation *invocation) {
  const flowie_control_release_http_t *server = self;
  FLOWIE_REQUIRE_RELEASE_ACCESS(FLOWIE_RELEASE_ACCESS_READ, NULL);
  const flowie_control_release_mqtt_config_t *scope = server->scope;
  flowie_control_release_active_t active;
  const int rc = flowie_control_release_store_current(scope->store, scope->environment, scope->domain, &active);
  if (rc != SALTS_OK) return flowie_release_http_result(invocation, rc);
  if (!active.generation) return flowie_release_http_error(invocation, 404, "no_active_release");
  return flowie_release_http_active_reply(invocation, &active);
}

CHTTP_ACTION(flowie_release_http_download);
int flowie_release_http_download(void *self, const chttp_invocation *invocation) {
  const flowie_control_release_http_t *server = self;
  FLOWIE_REQUIRE_RELEASE_ACCESS(FLOWIE_RELEASE_ACCESS_READ, NULL);
  const flowie_control_release_mqtt_config_t *scope = server->scope;
  const char *hex = chttp_server_request_param(chttp_invocation_request(invocation), "digest");
  uint8_t digest[FLOWIE_RELEASE_DIGEST_BYTES];
  if (flowie_release_http_decode(hex, digest) != SALTS_OK)
    return flowie_release_http_error(invocation, 400, "invalid_digest");
  void *bytes = NULL;
  size_t size = 0;
  int rc = flowie_control_release_store_read(scope->store, scope->environment, scope->domain,
      digest, &bytes, &size);
  if (rc != SALTS_OK) return flowie_release_http_result(invocation, rc);
  char etag[DIGEST_HEX_LENGTH + 3];
  snprintf(etag, sizeof(etag), "\"%s\"", hex);
  rc = chttp_invocation_set_header(invocation, "ETag", etag);
  if (rc == SALTS_OK) rc = chttp_invocation_set_header(invocation, "Cache-Control", "private, no-store");
  if (rc == SALTS_OK) rc = chttp_invocation_respond(invocation, 200, RELEASE_TYPE, bytes, size);
  free(bytes);
  return rc;
}

CHTTP_ACTION(flowie_release_http_list);
int flowie_release_http_list(void *self, const chttp_invocation *invocation) {
  const flowie_control_release_http_t *server = self;
  FLOWIE_REQUIRE_RELEASE_ACCESS(FLOWIE_RELEASE_ACCESS_READ, NULL);
  const flowie_control_release_mqtt_config_t *scope = server->scope;
  const char *query = strchr(chttp_invocation_request(invocation)->target, '?');
  uint8_t cursor[FLOWIE_RELEASE_DIGEST_BYTES];
  if (query && (strncmp(query, "?after=", 7) != 0 ||
                flowie_release_http_decode(query + 7, cursor) != SALTS_OK))
    return flowie_release_http_error(invocation, 400, "invalid_release_cursor");
  flowie_control_release_entry_t entries[FLOWIE_RELEASE_PAGE_CAPACITY];
  size_t count = 0;
  int more = 0;
  const int rc = flowie_control_release_store_list(scope->store, scope->environment, scope->domain,
      query ? cursor : NULL, entries, &count, &more);
  if (rc != SALTS_OK) return flowie_release_http_result(invocation, rc);
  char body[FLOWIE_RELEASE_PAGE_CAPACITY * 160 + 128], hex[DIGEST_HEX_LENGTH + 1];
  size_t size = (size_t)snprintf(body, sizeof(body), "{\"releases\":[");
  for (size_t i = 0; i < count; ++i) {
    flowie_release_http_encode(entries[i].digest, hex);
    size += (size_t)snprintf(body + size, sizeof(body) - size,
        "%s{\"digest\":\"%s\",\"bytes\":%" PRId64 ",\"admitted_at\":%" PRId64 "}",
        i ? "," : "", hex, entries[i].byte_count, entries[i].admitted_at);
  }
  size += more ? (size_t)snprintf(body + size, sizeof(body) - size, "],\"next_after\":\"%s\"}", hex)
               : (size_t)snprintf(body + size, sizeof(body) - size, "],\"next_after\":null}");
  return chttp_invocation_respond(invocation, 200, JSON_TYPE, body, size);
}

CHTTP_ACTION(flowie_release_http_submit_praktor);
int flowie_release_http_submit_praktor(void *self, const chttp_invocation *invocation) {
  const flowie_control_release_http_t *server = self;
  FLOWIE_REQUIRE_RELEASE_ACCESS(FLOWIE_RELEASE_ACCESS_PRAKTOR, NULL);
  const flowie_control_release_mqtt_config_t *scope = server->scope;
  const chttp_server_request_view *request = chttp_invocation_request(invocation);
  const char *hex = chttp_server_request_param(request, "digest");
  const char *key = chttp_server_request_header(request, "Idempotency-Key");
  uint8_t digest[FLOWIE_RELEASE_DIGEST_BYTES];
  if (request->body_size || flowie_release_http_decode(hex, digest) != SALTS_OK ||
      !flowie_control_text_valid(key, 128u) ||
      key[strspn(key, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.")] != 0)
    return flowie_release_http_error(invocation, 400, "invalid_praktor_submission");
  const time_t now = time(NULL);
  if (now == (time_t)-1) return flowie_release_http_error(invocation, 503, "clock_unavailable");
  flowie_release_archive_t *archive = NULL;
  int rc = flowie_release_http_read_verified(scope, digest, now, &archive);
  if (rc != SALTS_OK) return flowie_release_http_result(invocation, rc);
  const flowie_release_manifest_t *manifest = flowie_control_release_archive_manifest(archive);
  if (manifest->praktor_index == manifest->file_count) {
    flowie_control_release_archive_destroy(archive);
    return flowie_release_http_error(invocation, 404, "praktor_instructions_not_found");
  }
  const void *yaml = NULL;
  size_t size = 0;
  rc = flowie_control_release_archive_file(archive, manifest->files[manifest->praktor_index].path,
      &yaml, &size);
  if (rc != SALTS_OK || memchr(yaml, 0, size)) {
    flowie_control_release_archive_destroy(archive);
    return flowie_release_http_error(invocation, 422, "invalid_praktor_text");
  }
  const int bundled = manifest->file_count > 2u; /* ACL and Praktor are always distinct. */
  const void *body = bundled ? flowie_control_release_archive_tar(archive, &size) : yaml;
  rc = server->config.submit_praktor(server->config.praktor_context, scope->domain, hex, key,
      bundled ? "application/x-tar" : "application/yaml", body, size);
  flowie_control_release_archive_destroy(archive);
  if (rc == SALTS_ETIMEDOUT)
    return flowie_release_http_error(invocation, 504, "praktor_outcome_unknown");
  if (rc == SALTS_EBUSY || rc == SALTS_ENOBUFS)
    return flowie_release_http_error(invocation, 503, "praktor_busy");
  if (rc == SALTS_EALREADY)
    return flowie_release_http_error(invocation, 409, "praktor_submission_conflict");
  if (rc != SALTS_OK)
    return flowie_release_http_error(invocation, 502, "praktor_acceptance_unconfirmed");
  static const char accepted[] = "{\"accepted\":true}";
  return chttp_invocation_respond(invocation, 202, JSON_TYPE, accepted, sizeof(accepted) - 1u);
}

CHTTP_ACTION(flowie_release_http_health);
int flowie_release_http_health(void *self, const chttp_invocation *invocation) {
  (void)self;
  static const char body[] = "{\"alive\":true}";
  return chttp_invocation_respond(invocation, 200, JSON_TYPE, body, sizeof(body) - 1u);
}

CHTTP_ACTION(flowie_release_http_status);
int flowie_release_http_status(void *self, const chttp_invocation *invocation) {
  const flowie_control_release_http_t *server = self;
  FLOWIE_REQUIRE_RELEASE_ACCESS(FLOWIE_RELEASE_ACCESS_READ, NULL);
  flowie_control_release_active_t committed;
  int ready = 0;
  const int rc = flowie_control_release_mqtt_status(server->config.mqtt, &committed, &ready);
  if (rc != SALTS_OK) return flowie_release_http_result(invocation, rc);
  char body[224], observed[24] = "null";
  if (ready) snprintf(observed, sizeof(observed), "%" PRIu64, committed.generation);
  const int size = snprintf(body, sizeof(body),
      "{\"expected_generation\":%" PRIu64 ",\"observed_generation\":%s,\"ready\":%s,"
      "\"queue_depth\":null,\"connectivity\":null,\"delivery\":null}",
      committed.generation, observed, ready ? "true" : "false");
  return chttp_invocation_respond(invocation, 200, JSON_TYPE, body, (size_t)size);
}

static int flowie_release_http_segment(const char *value) {
  if (!flowie_control_text_valid(value, FLOWIE_SECURITY_ID_MAX)) return 0;
  return value[strspn(value, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")] == 0;
}

int flowie_control_release_http_prepare(const flowie_control_release_http_config_t *config,
    flowie_control_release_http_t **out) {
  if (!out) return SALTS_EINVAL;
  *out = NULL;
  uint8_t pin[FLOWIE_RELEASE_DIGEST_BYTES];
  if (!config || !config->host || !config->tls || config->tls->client_auth != CNET_TLS_CLIENT_AUTH_REQUIRED ||
      !config->mqtt || (!config->authorize &&
      (flowie_release_http_decode(config->operator_certificate_sha256, pin) != SALTS_OK ||
       !flowie_control_text_valid(config->operator_id, FLOWIE_SECURITY_ID_MAX))) ||
      !flowie_control_text_valid(config->signer_reference, FLOWIE_SECURITY_ID_MAX) ||
      !flowie_control_text_valid(config->validation_policy_revision, FLOWIE_SECURITY_ID_MAX))
    return SALTS_EINVAL;
  const flowie_control_release_mqtt_config_t *scope = flowie_control_release_mqtt_config(config->mqtt);
  if (!flowie_release_http_segment(scope->domain)) return SALTS_EINVAL;
  const salts_crypto_sm2_cms_verify_config *signer = scope->signer;
  if (!signer->chain || !signer->chain_count) return SALTS_EINVAL;
  const salts_crypto_sm2_cert_span *leaf = &signer->chain[0].certificate;
  if (!leaf->der || !leaf->size) return SALTS_EINVAL;
  uint8_t fingerprint[FLOWIE_RELEASE_DIGEST_BYTES];
  if (salts_crypto_sha256(leaf->der, leaf->size, fingerprint) != SALTS_CRYPTO_OK)
    return SALTS_EIO;
  flowie_control_release_http_t *server = calloc(1, sizeof(*server));
  if (!server) return SALTS_ENOMEM;
  server->config = *config;
  server->scope = scope;
  flowie_release_http_encode(fingerprint, server->signer_certificate_sha256);
  const cmeta_data_desc *receiver = cmeta_reflected_data(flowie_control_release_http_t);
  server->provider = (salts_configurator_provider_binding){sizeof(server->provider),
      SALTS_CONFIGURATOR_PROVIDER_BINDING_ABI_VERSION, cmeta_component_meta(FlowieReleaseHttpComponent),
      server, NULL, flowie_release_http_component, NULL, NULL};
  server->deployment = (salts_configurator_deployment){&server->provider, NULL, NULL};
  const chttp_action actions[] = {
      CHTTP_POST(RELEASE_ROUTE, flowie_release_http_upload),
      CHTTP_POST(RELEASE_ROUTE "/:digest/validate", flowie_release_http_validate),
      CHTTP_GET(RELEASE_ROUTE, flowie_release_http_list),
      CHTTP_GET(RELEASE_ROUTE "/:digest", flowie_release_http_download),
      CHTTP_GET(RELEASE_ROUTE "/:digest/acl", flowie_release_http_preview_acl),
      CHTTP_GET(PACKAGE_UI_ROUTE, flowie_release_http_ui),
      CHTTP_GET(PACKAGE_UI_ROUTE "/app.css", flowie_release_http_ui_css),
      CHTTP_GET(PACKAGE_UI_ROUTE "/app.js", flowie_release_http_ui_js),
      CHTTP_GET(PACKAGE_UI_ROUTE "/context", flowie_release_http_ui_context),
      CHTTP_MAPPING(CHTTP_METHOD_PUT, ACTIVE_ROUTE, flowie_release_http_activate),
      CHTTP_GET(ACTIVE_ROUTE, flowie_release_http_current),
      CHTTP_GET("/domains/:domain/status", flowie_release_http_status),
      CHTTP_GET("/healthz", flowie_release_http_health),
      CHTTP_POST(RELEASE_ROUTE "/:digest/praktor", flowie_release_http_submit_praktor),
  };
  const chttp_controller controller = {.component_id = "FlowieReleaseHttpComponent", .receiver = receiver,
      .actions = actions, .action_count = sizeof(actions) / sizeof(actions[0]) - !config->submit_praktor};
  const chttp_admission admission = {"FlowieReleaseHttpComponent", receiver,
      FunctionAbi(flowie_release_http_admit), flowie_release_http_admit};
  chttp_application_config app = chttp_application_config_default();
  app.components = &server->deployment;
  app.component_count = 1;
  app.dependency_capacity = 1;
  app.controllers = &controller;
  app.controller_count = 1;
  app.admission = &admission;
  app.http.host = config->host;
  app.http.port = config->port;
  app.http.tls = config->tls;
  app.http.enable_http2 = 0;
  app.http.h2_stream_capacity = 0;
  app.http.h2_input_buffer_bytes = 0;
  app.http.h2_output_buffer_bytes = 0;
  app.http.h2_hpack_dynamic_table_bytes = 0;
  app.http.h2_max_settings_count = 0;
  app.http.max_request_body_bytes = FLOWIE_RELEASE_MAX_COMPRESSED_BYTES;
  app.http.max_response_body_bytes = FLOWIE_RELEASE_MAX_COMPRESSED_BYTES;
  app.http.max_buffered_response_body_bytes = FLOWIE_RELEASE_MAX_COMPRESSED_BYTES;
  app.http.network.max_send_bytes = FLOWIE_RELEASE_MAX_COMPRESSED_BYTES + HTTP_OVERHEAD_BYTES;
  app.http.network.tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES;
  app.http.network.tls_handshake_timeout_ms = app.http.network.connect_timeout_ms;
  app.http.buffer_capacity_bytes = 4u * app.http.network.max_send_bytes;
  app.workers.workers = 1u;
  app.workers.reply_bytes = app.http.network.max_send_bytes;
  app.workers.retained_bytes = 4u * app.workers.reply_bytes;
  int rc = chttp_application_init(&server->app, &app);
  if (rc != SALTS_OK) {
    const int close_rc = flowie_control_release_http_close(&server);
    *out = server;
    return close_rc == SALTS_OK ? rc : close_rc;
  }
  *out = server;
  return SALTS_OK;
}

int flowie_control_release_http_start(flowie_control_release_http_t *server) {
  return server ? chttp_application_start(&server->app) : SALTS_EINVAL;
}

int flowie_control_release_http_open(const flowie_control_release_http_config_t *config,
    flowie_control_release_http_t **out) {
  int rc = flowie_control_release_http_prepare(config, out);
  if (rc != SALTS_OK) return rc;
  rc = flowie_control_release_http_start(*out);
  if (rc != SALTS_OK) {
    const int close_rc = flowie_control_release_http_close(out);
    if (close_rc != SALTS_OK) return close_rc;
  }
  return rc;
}

int flowie_control_release_http_port(const flowie_control_release_http_t *server, uint16_t *port) {
  return server ? chttp_application_port(&server->app, port) : SALTS_EINVAL;
}

int flowie_control_release_http_close(flowie_control_release_http_t **owner) {
  if (!owner) return SALTS_EINVAL;
  flowie_control_release_http_t *server = *owner;
  if (!server) return SALTS_OK;
  const int rc = chttp_application_close(&server->app, 0);
  if (!server->app.impl) {
    free(server);
    *owner = NULL;
  }
  return rc;
}
