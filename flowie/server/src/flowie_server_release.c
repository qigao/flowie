#include "flowie_server_release_internal.h"
#include "flowie_control_release_http_internal.h"
#include "flowie_rulesforge_pipeline_internal.h"
#include "flowie_server_http_security_internal.h"

#include <core/error.h>
#include <core/tlog.h>
#include <crypto/sm2.h>
#include <crypto/sm2_x509.h>
#include <http_client/http.h>
#include <uri_parser.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FLOWIE_RELEASE_CLIENT_METHOD "mtls"

struct flowie_server_release_s {
  const flowie_server_release_config_t *config;
  unsigned char anchor[SALTS_CRYPTO_SM2_TRUST_MAX_CERT_SIZE + 1u];
  unsigned char leaf[SALTS_CRYPTO_SM2_TRUST_MAX_CERT_SIZE + 1u];
  uint8_t signer_key[SALTS_CRYPTO_SM2_PUBLIC_KEY_SIZE];
  salts_crypto_sm2_chain_entry chain;
  salts_crypto_sm2_trust_policy trust;
  salts_crypto_sm2_cms_verify_config signer;
  flowie_security_auth_provider_t identity;
  flowie_server_http_security_t *http_auth;
  flowie_server_http_endpoint_t *management_authority;
  flowie_control_release_processing_t processing;
  flowie_rulesforge_pipeline_provider_t rulesforge;
  flowie_control_release_store_t *store;
  flowie_control_release_store_t *journal;
  flowie_control_release_mqtt_t *mqtt;
  flowie_control_release_http_t *http;
  char praktor_uri[320];
  char praktor_authority[320];
  char praktor_target[2049];
  chttp_tls_profile praktor_tls;
  chttp_client_config praktor_client_config;
  chttp_client praktor_client;
};

static int flowie_server_release_praktor_prepare(flowie_server_release_t *release) {
  const flowie_server_praktor_config_t *config = &release->config->praktor;
  uri_t uri = {0};
  if (!uri_parse(config->url, &uri) || !uri.valid || strcmp(uri.scheme, "https") != 0 ||
      !uri.host[0] || uri.userinfo[0] || uri.fragment[0] || uri.query[0] || uri.path[0] != '/')
    return SALTS_EINVAL;
  const int port = uri.port ? uri.port : 443;
  if (port < 1 || port > UINT16_MAX) return SALTS_EINVAL;
  const int authority = snprintf(release->praktor_authority, sizeof(release->praktor_authority),
      uri.host_type == URI_HOST_IPV6ADDR ? "[%s]:%d" : "%s:%d", uri.host, port);
  if (authority <= 0 || (size_t)authority >= sizeof(release->praktor_authority)) return SALTS_ERANGE;
  const int connection = snprintf(release->praktor_uri, sizeof(release->praktor_uri), "tls://%s",
      release->praktor_authority);
  if (connection <= 0 || (size_t)connection >= sizeof(release->praktor_uri)) return SALTS_ERANGE;
  const int target = snprintf(release->praktor_target, sizeof(release->praktor_target), "%s", uri.path);
  if (target <= 0 || (size_t)target >= sizeof(release->praktor_target)) return SALTS_ERANGE;
  const cnet_tls_client_config tls = {.size = sizeof(tls), .server_name = uri.host,
      .ca_file = config->ca_file, .cert_file = config->cert_file, .key_file = config->key_file};
  int rc = chttp_tls_profile_init(&release->praktor_tls, &tls);
  if (rc != SALTS_OK) return rc;
  release->praktor_client_config = (chttp_client_config){
      .network = {.backend =
#if defined(_WIN32)
          NATIVE_IO_BACKEND_IOCP,
#elif defined(__linux__)
          NATIVE_IO_BACKEND_EPOLL,
#else
          NATIVE_IO_BACKEND_KQUEUE,
#endif
          .connection_capacity = 1u, .command_capacity = 8u, .request_capacity = 4u,
          .event_capacity = 8u, .completion_batch_capacity = 4u,
          .max_send_bytes = FLOWIE_RELEASE_MAX_EXPANDED_BYTES + 8192u,
          .receive_buffer_bytes = 4096u, .connect_timeout_ms = config->timeout_ms,
          .read_timeout_ms = config->timeout_ms, .write_timeout_ms = config->timeout_ms,
          .tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES,
          .tls_handshake_timeout_ms = config->timeout_ms},
      .request_capacity = 1u, .max_start_line_bytes = 1024u, .max_header_count = 16u,
      .max_header_bytes = 4096u, .max_request_body_bytes = FLOWIE_RELEASE_MAX_EXPANDED_BYTES,
      .max_response_body_bytes = 4096u, .max_informational_responses = 4u};
  return SALTS_OK;
}

static int flowie_server_release_praktor_submit(void *context, const char *domain,
    const char *digest, const char *key, const char *type, const void *body, size_t size) {
  flowie_server_release_t *release = context;
  /* The release App has one management worker. Initialize, submit and drain a
   * bounded client on that thread because CHttp clients are thread-affine. */
  if (release->praktor_client.impl) return SALTS_EBUSY;
  int rc = chttp_client_init(&release->praktor_client, &release->praktor_client_config);
  if (rc != SALTS_OK) return rc;
  const chttp_header headers[] = {{"Content-Type", type},
      {"Flowie-Domain", domain}, {"Flowie-Release-Digest", digest}, {"Idempotency-Key", key}};
  const chttp_options request = {.connection_uri = release->praktor_uri,
      .authority = release->praktor_authority, .target = release->praktor_target,
      .headers = headers, .header_count = sizeof(headers) / sizeof(headers[0]),
      .body = body, .body_size = size, .timeout_ms = release->config->praktor.timeout_ms,
      .tls = &release->praktor_tls, .protocol = CHTTP_HTTP_1_1};
  chttp_response response = {0};
  chttp_error error = {0};
  rc = chttp_post(&release->praktor_client, &request, &response, &error);
  if (rc == SALTS_OK) {
    if (response.status_code == 429u || response.status_code == 503u) rc = SALTS_EBUSY;
    else if (response.status_code == 409u) rc = SALTS_EALREADY;
    else if (response.status_code != 202u) rc = SALTS_EPROTO;
  }
  chttp_response_destroy(&response);
  const int close_rc = chttp_client_destroy(&release->praktor_client, 5000u);
  return rc == SALTS_OK ? close_rc : rc;
}

static int flowie_server_release_read_cert(const char *path, unsigned char *buffer,
                                          salts_crypto_sm2_cert_span *out) {
  FILE *file = fopen(path, "rb");
  if (!file) return errno == ENOENT ? SALTS_ENOENT : SALTS_EIO;
  const size_t size = fread(buffer, 1, SALTS_CRYPTO_SM2_TRUST_MAX_CERT_SIZE + 1u, file);
  const int read_error = ferror(file);
  const int close_error = fclose(file);
  if (read_error || close_error) return SALTS_EIO;
  if (size == 0u || size > SALTS_CRYPTO_SM2_TRUST_MAX_CERT_SIZE) return SALTS_EPROTO;
  *out = (salts_crypto_sm2_cert_span){buffer, size};
  return SALTS_OK;
}

static int flowie_server_release_load_signer(flowie_server_release_t *release) {
  const flowie_server_release_config_t *config = release->config;
  int rc = flowie_server_release_read_cert(config->signer_anchor_der, release->anchor,
                                          &release->trust.anchor);
  if (rc != SALTS_OK) return rc;
  rc = flowie_server_release_read_cert(config->signer_leaf_der, release->leaf,
                                      &release->chain.certificate);
  if (rc != SALTS_OK) return rc;
  const time_t now = time(NULL);
  if (now == (time_t)-1) return SALTS_EIO;
  if (salts_crypto_sm2_x509_public_key(release->leaf, release->chain.certificate.size,
                                       release->signer_key) != SALTS_CRYPTO_OK) return SALTS_EPROTO;
  release->chain.issuer_identity = config->issuer_identity;
  release->chain.issuer_identity_size = strlen(config->issuer_identity);
  release->trust.expected_leaf_public_key = release->signer_key;
  release->trust.validation_time = (int64_t)now;
  release->trust.purpose = SALTS_CRYPTO_SM2_TRUST_CODE_SIGNING;
  release->trust.revocation = SALTS_CRYPTO_SM2_REVOCATION_NOT_CHECKED;
  uint8_t verified[SALTS_CRYPTO_SM2_PUBLIC_KEY_SIZE];
  if (salts_crypto_sm2_validate_pinned_chain(&release->chain, 1u, &release->trust, verified) !=
      SALTS_CRYPTO_OK) return SALTS_EPERM;
  release->signer = (salts_crypto_sm2_cms_verify_config){.format = SALTS_CRYPTO_SM2_CMS_RFC5652,
      .detached = true, .signer_identity = config->signer_identity,
      .signer_identity_size = strlen(config->signer_identity), .chain = &release->chain,
      .chain_count = 1u, .trust = &release->trust};
  return SALTS_OK;
}

static int flowie_server_release_authenticate(void *context, const flowie_security_auth_request_t *request,
                                             flowie_security_principal_t *principal) {
  const flowie_server_release_t *release = context;
  const flowie_server_release_config_t *config = release->config;
  if (request->size < offsetof(flowie_security_auth_request_t, transport_peer_address) ||
      !request->peer_certificate_sha256 || !request->peer_certificate_sha256[0] ||
      !request->identity || strcmp(config->operator_sha256, request->peer_certificate_sha256) == 0)
    return SALTS_EPERM;
  if (release->http_auth)
    return flowie_security_authenticate(
        flowie_server_http_security_auth_provider(release->http_auth), request, principal);
  if (strcmp(request->method, FLOWIE_RELEASE_CLIENT_METHOD) != 0 || request->secret_size != 0u)
    return SALTS_EPERM;
  /* The native MQTT transport forwards CNet's verified lowercase hex digest. */
  const char *pin = request->peer_certificate_sha256;
  for (size_t i = 0; i < config->client_count; ++i) {
    const flowie_server_release_client_t *client = &config->clients[i];
    if (strcmp(client->sha256, pin) != 0 || strcmp(client->principal, request->identity) != 0) continue;
    *principal = (flowie_security_principal_t)FLOWIE_SECURITY_PRINCIPAL_INIT;
    memcpy(principal->principal_id, client->principal, strlen(client->principal) + 1u);
    memcpy(principal->domain_id, config->domain, strlen(config->domain) + 1u);
    memcpy(principal->principal_type, "device", sizeof("device"));
    memcpy(principal->auth_method, FLOWIE_RELEASE_CLIENT_METHOD, sizeof(FLOWIE_RELEASE_CLIENT_METHOD));
    principal->scope = FLOWIE_SECURITY_SCOPE_SELF;
    /* The MQTT release owner stamps the active ACL version before publication. */
    principal->policy_version = 1u;
    return SALTS_OK;
  }
  return SALTS_EPERM;
}

static int flowie_server_release_check_broker_tls(const flowie_server_release_config_t *config) {
  const cnet_tls_server_config tls = {.size = sizeof(tls),
      .cert_file = getenv("SALTS_TLS_CERT_FILE"), .key_file = getenv("SALTS_TLS_KEY_FILE"),
      .ca_file = config->mqtt_client_ca_file, .client_auth = CNET_TLS_CLIENT_AUTH_REQUIRED};
  cnet_tls_server server = {0};
  const int rc = cnet_tls_server_init(&server, &tls);
  return rc == SALTS_OK ? cnet_tls_server_destroy(&server) : rc;
}

int flowie_server_release_prepare(const flowie_server_release_config_t *config,
    const orm_config_t *database, int check_only, flowie_server_release_t **out,
    const char **operation) {
  *out = NULL;
  *operation = "release-signer-load";
  flowie_server_release_t *release = calloc(1, sizeof(*release));
  if (!release) return SALTS_ENOMEM;
  *out = release;
  release->config = config;
  int rc = flowie_server_release_load_signer(release);
  if (rc != SALTS_OK) return rc;
  if (check_only) {
    *operation = "release-mqtt-tls-check";
    rc = flowie_server_release_check_broker_tls(config);
    if (rc != SALTS_OK) return rc;
  }
  if (config->auth.url[0]) {
    *operation = "release-auth-prepare";
    rc = flowie_server_http_security_create(&config->auth, NULL, &release->http_auth);
    if (rc == SALTS_OK)
      rc = flowie_server_http_security_check_auth_tls(release->http_auth);
    if (rc != SALTS_OK) return rc;
  }
  *operation = "release-store-open";
  const flowie_control_release_store_limits_t limits = {.domain_count = config->package_count,
      .domain_bytes = config->package_bytes, .total_count = config->package_count,
      .total_bytes = config->package_bytes};
  rc = flowie_control_release_store_open(database, &limits, &release->store);
  if (rc != SALTS_OK) return rc;
  if (config->rulesforge.timeout_ms) {
    *operation = "release-journal-configure";
    orm_config_t journal;
    orm_config(&journal);
    const orm_option_t options[] = {{orm_view("filename"), orm_view(config->rulesforge.journal_file)},
        {orm_view("open_mode"), orm_view("read_write_create")},
        {orm_view("busy_timeout_ms"), orm_view("1000")}};
    journal.driver = orm_view("sqlite");
    journal.options = options;
    journal.option_count = (uint32_t)(sizeof(options) / sizeof(options[0]));
    rc = flowie_control_release_journal_open(&journal, &release->journal);
    if (rc == SALTS_OK)
      rc = flowie_control_release_store_check_journal(release->journal, release->store, database);
    if (rc != SALTS_OK) return rc;
  }

  *operation = "release-mqtt-restore";
  release->identity = (flowie_security_auth_provider_t){sizeof(release->identity),
      release, flowie_server_release_authenticate};
  release->rulesforge.config = config;
  release->rulesforge.store = release->journal;
  release->processing = (flowie_control_release_processing_t){&release->rulesforge,
      flowie_rulesforge_pipeline_prepare, flowie_rulesforge_pipeline_close,
      flowie_rulesforge_pipeline_publish, flowie_rulesforge_pipeline_activate};
  const flowie_control_release_mqtt_config_t mqtt = {.store = release->store,
      .environment = config->environment, .domain = config->domain, .realm_name = "domain-release",
      .resource_uid = "domain-release", .owner_name = "flowie-server",
      .auth_method = release->http_auth ? config->auth.method : FLOWIE_RELEASE_CLIENT_METHOD,
      .identity_provider = &release->identity,
      .signer = &release->signer, .processing = config->rulesforge.timeout_ms ? &release->processing : NULL,
      .journal = release->journal};
  rc = flowie_control_release_mqtt_open(&mqtt, &release->mqtt);
  if (rc != SALTS_OK) return rc;
  if (config->praktor.url[0]) {
    *operation = "release-praktor-prepare";
    rc = flowie_server_release_praktor_prepare(release);
    if (rc != SALTS_OK) return rc;
  }

  *operation = "release-https-prepare";
  if (config->management_authority.url[0]) {
    rc = flowie_server_release_authority_create(&config->management_authority,
        &release->management_authority);
    if (rc != SALTS_OK) return rc;
  }
  const cnet_tls_server_config tls = {.size = sizeof(tls), .cert_file = config->management_cert_file,
      .key_file = config->management_key_file, .ca_file = config->management_client_ca_file,
      .client_auth = CNET_TLS_CLIENT_AUTH_REQUIRED};
  const flowie_control_release_http_config_t http = {.host = config->management_host,
      .port = config->management_port, .tls = &tls, .mqtt = release->mqtt,
      .operator_id = config->operator_id, .operator_certificate_sha256 = config->operator_sha256,
      .signer_reference = config->signer_reference,
      .validation_policy_revision = config->validation_policy_revision,
      .submit_praktor = config->praktor.url[0] ? flowie_server_release_praktor_submit : NULL,
      .praktor_context = release,
      .authorize = release->management_authority ? flowie_server_release_authorize : NULL,
      .authorize_context = release->management_authority};
  return flowie_control_release_http_prepare(&http, &release->http);
}

int flowie_server_release_start(flowie_server_release_t *release) {
  const int rc = flowie_control_release_http_start(release->http);
  if (rc == SALTS_OK) flowie_control_release_mqtt_resume(release->mqtt);
  return rc;
}

int flowie_server_release_reconcile(flowie_server_release_t *release,
    flowie_protocol_repository_t *repository) {
  if (!release->journal) return SALTS_OK;
  return flowie_control_release_admissions_reconcile(release->journal,
      release->config->environment, release->config->domain, repository);
}

int flowie_server_release_stop(flowie_server_release_t *release, int *settled) {
  *settled = 1;
  if (!release) return SALTS_OK;
  int rc = flowie_control_release_http_close(&release->http);
  if (!release->http && release->praktor_client.impl) {
    const int client_rc = chttp_client_destroy(&release->praktor_client, 5000u);
    if (rc == SALTS_OK) rc = client_rc;
  }
  *settled = !release->http && !release->praktor_client.impl;
  return rc;
}

int flowie_server_release_destroy(flowie_server_release_t **owner) {
  flowie_server_release_t *release = *owner;
  if (!release) return SALTS_OK;
  if (release->praktor_tls.impl)
    (void)chttp_tls_profile_destroy(&release->praktor_tls);
  flowie_control_release_mqtt_close(release->mqtt);
  release->mqtt = NULL;
  const int rc = flowie_rulesforge_pipeline_reap(&release->rulesforge);
  if (release->rulesforge.retired[0] || release->rulesforge.retired[1]) return rc;
  flowie_server_http_security_destroy(release->http_auth);
  free(release->management_authority);
  flowie_control_release_store_close(release->store);
  flowie_control_release_store_close(release->journal);
  free(release);
  *owner = NULL;
  return rc;
}

int flowie_server_release_port(const flowie_server_release_t *release, uint16_t *port) {
  return release ? flowie_control_release_http_port(release->http, port) : SALTS_ENOENT;
}

const flowie_endpoint_security_binding_t *flowie_server_release_binding(
    const flowie_server_release_t *release) {
  return flowie_control_release_mqtt_binding(release->mqtt);
}

static void flowie_server_release_qos2_committed(void *context, const uint8_t *exchange) {
  flowie_server_release_t *release = context;
  const int rc = flowie_control_release_admission_forget(release->journal,
      release->config->environment, release->config->domain, exchange);
  if (rc != SALTS_OK)
    SALTS_LOG_ERRORF(tlog_peek_default(), "Flowie.RulesForge",
        "release admission retirement domain={} status={} ({})",
        release->config->domain, rc, error_code_string(rc));
}

int flowie_server_release_publish(flowie_server_release_t *release, const flowie_message_t *message,
    int *handled) {
  *handled = 0;
  if (message->type != FLOWIE_MQTT_PACKET_PUBLISH ||
      (message->flags & FLOWIE_MQTT_MESSAGE_BROKER_WILL)) return SALTS_OK;
  flowie_mqtt_parse_options_t options = FLOWIE_MQTT_PARSE_OPTIONS_INIT;
  if (flowie_mqtt_message_flags_version(message->flags, &options.version) != SALTS_OK)
    return SALTS_EPROTO;
  flowie_mqtt_packet_view_t packet = FLOWIE_MQTT_PACKET_VIEW_INIT;
  flowie_mqtt_publish_view_t publish = FLOWIE_MQTT_PUBLISH_VIEW_INIT;
  size_t consumed = 0;
  if (flowie_mqtt_packet_parse((const uint8_t *)message->payload.data, message->payload.len,
          &options, &packet, &consumed, NULL) != FLOWIE_MQTT_PARSE_OK ||
      consumed != message->payload.len || flowie_mqtt_publish_parse(&packet, &publish) != FLOWIE_MQTT_PARSE_OK)
    return SALTS_EPROTO;
  flowie_direct_publish_context_t *ingress = message->transport_context;
  const uint8_t *exchange = ingress && ingress->qos2_exchange[6] ? ingress->qos2_exchange : NULL;
  const int rc = flowie_control_release_mqtt_publish(release->mqtt,
      ingress ? ingress->principal : NULL, &publish, exchange, handled);
  if (rc == SALTS_OK && *handled && exchange && release->journal) {
    ingress->on_qos2_committed = flowie_server_release_qos2_committed;
    ingress->commit_context = release;
  }
  return rc;
}
