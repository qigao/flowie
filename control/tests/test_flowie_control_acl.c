#include "flowie_control_acl_internal.h"
#include "flowie.h"

#include "tinytest.h"
#include "turbo_error.h"

#include <string.h>

spec("Flowie Control user ACL grammar") {
  it("parses and canonically formats one user parent with bounded topic entries") {
    static const char input[] =
        "user 4fc55867-cdb8-4458-9ba7-afca8e4e2867 allow {\n"
        "write topic booth/groups/china/east/operators/devices/%u/{event,heartbeat,process}\n"
        "read topic booth/groups/china/east/operators/devices/%c/{command,payment}\n"
        "deny readwrite topic booth/groups/china/east/operators/devices/%u/private\n"
        "}";
    static const char canonical[] =
        "user 4fc55867-cdb8-4458-9ba7-afca8e4e2867 allow {\n"
        "  write topic booth/groups/china/east/operators/devices/%u/{event,heartbeat,process}\n"
        "  read topic booth/groups/china/east/operators/devices/%c/{command,payment}\n"
        "  deny readwrite topic booth/groups/china/east/operators/devices/%u/private\n"
        "}";
    flowie_control_acl_document_t document = FLOWIE_CONTROL_ACL_DOCUMENT_INIT;
    char formatted[FLOWIE_CONTROL_ACL_DOCUMENT_MAX + 1u];
    size_t formatted_size = 0u;

    check_equal(flowie_control_acl_parse(input, sizeof(input) - 1u, &document), TURBO_OK);
    check_equal(document.subject, "4fc55867-cdb8-4458-9ba7-afca8e4e2867");
    check_equal(document.connection_effect, FLOWIE_SECURITY_ALLOW);
    check_equal(document.entry_count, 3u);
    check_equal(document.entries[0].action_mask, FLOWIE_SECURITY_ACTION_PUBLISH);
    check_equal(document.entries[0].group_count, 3u);
    check_equal(document.entries[0].alternative_count, 3u);
    check_true(document.entries[0].uses_username);
    check_false(document.entries[0].uses_client_id);
    check_equal(document.entries[1].action_mask, FLOWIE_SECURITY_ACTION_SUBSCRIBE);
    check_true(document.entries[1].uses_client_id);
    check_equal(document.entries[2].effect, FLOWIE_SECURITY_DENY);
    check_equal(document.entries[2].action_mask,
                  FLOWIE_SECURITY_ACTION_PUBLISH |
                      FLOWIE_SECURITY_ACTION_SUBSCRIBE);
    check_equal(flowie_control_acl_format(&document, formatted, sizeof(formatted),
                                           &formatted_size),
                 TURBO_OK);
    check_equal(formatted_size, sizeof(canonical) - 1u);
    check_equal(formatted, canonical);
  }

  it("compiles connection state substitutions and alternatives into bounded internal rules") {
    static const char input[] =
        "user device-1 allow {"
        "write topic root-a/groups/region/operators/devices/%u/{event,heartbeat} "
        "read topic root-a/groups/region/operators/devices/%c/command"
        "}";
    flowie_control_acl_document_t document = FLOWIE_CONTROL_ACL_DOCUMENT_INIT;
    flowie_security_rule_t rules[4] = {FLOWIE_SECURITY_RULE_INIT,
                                           FLOWIE_SECURITY_RULE_INIT,
                                           FLOWIE_SECURITY_RULE_INIT,
                                           FLOWIE_SECURITY_RULE_INIT};
    flowie_security_matcher_t matcher = FLOWIE_SECURITY_MATCHER_INIT;
    flowie_security_realm_config_t realm_config = FLOWIE_SECURITY_REALM_CONFIG_INIT;
    flowie_security_principal_t principal = FLOWIE_SECURITY_PRINCIPAL_INIT;
    flowie_security_request_t request = FLOWIE_SECURITY_REQUEST_INIT;
    flowie_security_decision_t decision = FLOWIE_SECURITY_DECISION_INIT;
    flowie_mqtt_security_context_t mqtt = FLOWIE_MQTT_SECURITY_CONTEXT_INIT;
    flowie_security_realm_t *realm = NULL;
    size_t count = 0u;

    check_equal(flowie_control_acl_parse(input, sizeof(input) - 1u, &document), TURBO_OK);
    check_equal(flowie_control_acl_compile(&document, "root-a", rules, 4u, &count), TURBO_OK);
    check_equal(count, 4u);
    check_equal(rules[0].action_mask, FLOWIE_SECURITY_ACTION_CONNECT);
    check_equal(rules[0].resource_type, FLOWIE_SECURITY_RESOURCE_GENERIC);
    check_equal(rules[0].match_kind, FLOWIE_SECURITY_MATCH_PREFIX);
    check_equal(rules[0].pattern, "");
    check_equal(rules[1].pattern,
                 "root-a/groups/region/operators/devices/%u/event");
    check_equal(rules[2].pattern,
                 "root-a/groups/region/operators/devices/%u/heartbeat");
    check_equal(rules[3].pattern,
                 "root-a/groups/region/operators/devices/%c/command");
    check_equal(flowie_mqtt_security_matcher_init(&matcher), TURBO_OK);
    realm_config.policy_version = 1u;
    realm_config.rules = rules;
    realm_config.rule_count = count;
    realm_config.matcher = matcher;
    check_equal(flowie_security_realm_create(&realm_config, &realm), TURBO_OK);
    memcpy(principal.principal_id, "device-1", sizeof("device-1"));
    memcpy(principal.principal_type, "device", sizeof("device"));
    memcpy(principal.domain_id, "root-a", sizeof("root-a"));
    memcpy(principal.auth_method, "password", sizeof("password"));
    principal.scope = FLOWIE_SECURITY_SCOPE_DOMAIN;
    principal.policy_version = 1u;
    request.principal = &principal;
    request.domain_id = "root-a";
    request.action = FLOWIE_SECURITY_ACTION_CONNECT;
    request.resource_type = FLOWIE_SECURITY_RESOURCE_GENERIC;
    request.resource = "root-a";
    check_equal(flowie_security_realm_evaluate(realm, &request, 1u, &decision), TURBO_OK);
    check_equal(decision.effect, FLOWIE_SECURITY_ALLOW);

    mqtt.username = (flowie_mqtt_span_t){(const uint8_t *)"device-1", 8u};
    request.action = FLOWIE_SECURITY_ACTION_PUBLISH;
    request.resource_type = FLOWIE_SECURITY_RESOURCE_MQTT_TOPIC;
    request.resource = "root-a/groups/region/operators/devices/device-1/event";
    request.protocol_context = &mqtt;
    decision = (flowie_security_decision_t)FLOWIE_SECURITY_DECISION_INIT;
    check_equal(flowie_security_realm_evaluate(realm, &request, 1u, &decision), TURBO_OK);
    check_equal(decision.effect, FLOWIE_SECURITY_ALLOW);
    flowie_security_realm_destroy(realm);
  }

  it("rejects malformed trees duplicate alternatives denied blocks and capacity overflow") {
    static const char bad_tree[] =
        "user device-1 allow { read topic root-a/operators/device-1/event }";
    static const char duplicate_alternative[] =
        "user device-1 allow { read topic root-a/groups/operators/devices/%u/{event,event} }";
    static const char denied_block[] =
        "user device-1 deny { read topic root-a/groups/operators/devices/%u/event }";
    flowie_control_acl_document_t document = FLOWIE_CONTROL_ACL_DOCUMENT_INIT;

    check_equal(flowie_control_acl_parse(bad_tree, sizeof(bad_tree) - 1u, &document),
                 TURBO_EPROTO);
    check_equal(flowie_control_acl_parse(duplicate_alternative,
                                          sizeof(duplicate_alternative) - 1u, &document),
                 TURBO_EPROTO);
    check_equal(flowie_control_acl_parse(denied_block, sizeof(denied_block) - 1u, &document),
                 TURBO_EPROTO);
  }
}
