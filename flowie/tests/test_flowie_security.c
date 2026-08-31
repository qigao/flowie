#include "flowie_security.h"

#include "tinytest.h"
#include "turbo_error.h"

#include <stdio.h>

typedef struct flowie_authorization_fixture_s {
  flowie_security_effect_t effect;
  size_t calls;
} flowie_authorization_fixture_t;

static flowie_security_principal_t flowie_test_principal(uint64_t policy_version) {
  flowie_security_principal_t principal = FLOWIE_SECURITY_PRINCIPAL_INIT;
  (void)snprintf(principal.principal_id, sizeof(principal.principal_id), "%s", "device-1");
  (void)snprintf(principal.principal_type, sizeof(principal.principal_type), "%s", "device");
  (void)snprintf(principal.domain_id, sizeof(principal.domain_id), "%s", "booth");
  (void)snprintf(principal.auth_method, sizeof(principal.auth_method), "%s", "password");
  principal.scope = FLOWIE_SECURITY_SCOPE_SELF;
  principal.policy_version = policy_version;
  return principal;
}

static int flowie_test_authorize(void *ctx, const flowie_security_request_t *request,
                                 uint64_t now_epoch_seconds,
                                 flowie_security_decision_t *decision_out) {
  flowie_authorization_fixture_t *fixture = (flowie_authorization_fixture_t *)ctx;
  (void)now_epoch_seconds;
  if (!fixture || !request || !request->principal || !decision_out ||
      decision_out->size < sizeof(*decision_out))
    return TURBO_EINVAL;
  fixture->calls += 1u;
  decision_out->effect = fixture->effect;
  decision_out->reason = fixture->effect == FLOWIE_SECURITY_ALLOW
                             ? FLOWIE_SECURITY_REASON_ALLOW_RULE
                             : FLOWIE_SECURITY_REASON_DENY_RULE;
  decision_out->policy_version = request->principal->policy_version;
  return TURBO_OK;
}

static int flowie_test_remote_authorize(flowie_security_effect_t effect,
                                        flowie_security_decision_t *decision_out) {
  flowie_authorization_fixture_t fixture = {effect, 0u};
  flowie_security_authorization_provider_t provider = {
      sizeof(provider), &fixture, flowie_test_authorize};
  flowie_security_realm_config_t config = FLOWIE_SECURITY_REALM_CONFIG_INIT;
  flowie_security_request_t request = FLOWIE_SECURITY_REQUEST_INIT;
  flowie_security_principal_t principal = flowie_test_principal(7u);
  flowie_security_realm_t *realm = NULL;
  int rc;

  config.policy_source = "acl.remote";
  rc = flowie_security_realm_create(&config, &realm);
  if (rc != TURBO_OK) return rc;
  rc = flowie_security_realm_bind_authorization_provider(realm, &provider);
  if (rc == TURBO_OK) {
    request.principal = &principal;
    request.domain_id = principal.domain_id;
    request.action = FLOWIE_SECURITY_ACTION_PUBLISH;
    request.resource_type = FLOWIE_SECURITY_RESOURCE_MQTT_TOPIC;
    request.resource = "booth/devices/device-1/event";
    rc = flowie_security_realm_authorize(realm, &request, 1u, decision_out);
  }
  check_equal(fixture.calls, (size_t)1);
  flowie_security_realm_destroy(realm);
  return rc;
}

spec("Flowie security authorization boundary") {
  it("maps a remote deny decision to permission denied") {
    flowie_security_decision_t decision = FLOWIE_SECURITY_DECISION_INIT;

    check_equal(flowie_test_remote_authorize(FLOWIE_SECURITY_DENY, &decision), TURBO_EPERM);
    check_equal(decision.effect, FLOWIE_SECURITY_DENY);
    check_equal(decision.reason, FLOWIE_SECURITY_REASON_DENY_RULE);
    check_equal(decision.policy_version, UINT64_C(7));
  }

  it("preserves a remote allow decision") {
    flowie_security_decision_t decision = FLOWIE_SECURITY_DECISION_INIT;

    check_equal(flowie_test_remote_authorize(FLOWIE_SECURITY_ALLOW, &decision), TURBO_OK);
    check_equal(decision.effect, FLOWIE_SECURITY_ALLOW);
    check_equal(decision.reason, FLOWIE_SECURITY_REASON_ALLOW_RULE);
  }

  it("maps a local default deny decision to permission denied") {
    flowie_security_realm_config_t config = FLOWIE_SECURITY_REALM_CONFIG_INIT;
    flowie_security_request_t request = FLOWIE_SECURITY_REQUEST_INIT;
    flowie_security_decision_t decision = FLOWIE_SECURITY_DECISION_INIT;
    flowie_security_principal_t principal = flowie_test_principal(11u);
    flowie_security_realm_t *realm = NULL;

    config.policy_version = 11u;
    check_equal(flowie_security_realm_create(&config, &realm), TURBO_OK);
    request.principal = &principal;
    request.domain_id = principal.domain_id;
    request.action = FLOWIE_SECURITY_ACTION_PUBLISH;
    request.resource_type = FLOWIE_SECURITY_RESOURCE_MQTT_TOPIC;
    request.resource = "booth/devices/device-1/event";
    check_equal(flowie_security_realm_authorize(realm, &request, 1u, &decision), TURBO_EPERM);
    check_equal(decision.effect, FLOWIE_SECURITY_DENY);
    check_equal(decision.reason, FLOWIE_SECURITY_REASON_DEFAULT_DENY);
    flowie_security_realm_destroy(realm);
  }
}
