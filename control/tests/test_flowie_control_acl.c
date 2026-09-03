#include "flowie_control_acl_internal.h"
#include "flowie.h"

#include "tinytest.h"
#include "salts_error.h"

#include <string.h>

spec("Flowie Control subject-scoped ACL grammar") {
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

    check_equal(flowie_control_acl_parse(input, sizeof(input) - 1u, &document), SALTS_OK);
    check_equal(document.subject_kind, FLOWIE_SECURITY_SUBJECT_PRINCIPAL);
    check_equal(document.subject, "4fc55867-cdb8-4458-9ba7-afca8e4e2867");
    check_equal(document.connection_effect, FLOWIE_SECURITY_ALLOW);
    check_equal(document.entry_count, 3u);
    check_equal(document.entries[0].action_mask, FLOWIE_SECURITY_ACTION_PUBLISH);
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
                 SALTS_OK);
    check_equal(formatted_size, sizeof(canonical) - 1u);
    check_equal(formatted, canonical);
  }

  it("parses formats and compiles role and group subjects with generic MQTT topics") {
    static const char role_input[] =
        "role publisher allow {"
        "write topic root-a/telemetry/%u/+ "
        "read topic root-a/commands/# "
        "deny write topic root-a/events/{created,updated}"
        "}";
    static const char role_canonical[] =
        "role publisher allow {\n"
        "  write topic root-a/telemetry/%u/+\n"
        "  read topic root-a/commands/#\n"
        "  deny write topic root-a/events/{created,updated}\n"
        "}";
    static const char group_input[] = "group operators allow";
    flowie_control_acl_document_t document = FLOWIE_CONTROL_ACL_DOCUMENT_INIT;
    flowie_security_rule_t rules[5] = {
        FLOWIE_SECURITY_RULE_INIT, FLOWIE_SECURITY_RULE_INIT, FLOWIE_SECURITY_RULE_INIT,
        FLOWIE_SECURITY_RULE_INIT, FLOWIE_SECURITY_RULE_INIT};
    char formatted[FLOWIE_CONTROL_ACL_DOCUMENT_MAX + 1u];
    size_t formatted_size = 0u;
    size_t rule_count = 0u;

    check_equal(flowie_control_acl_parse(role_input, sizeof(role_input) - 1u, &document),
                SALTS_OK);
    check_equal(document.subject_kind, FLOWIE_SECURITY_SUBJECT_ROLE);
    check_equal(document.entry_count, 3u);
    check_true(document.entries[0].uses_username);
    check_false(document.entries[0].uses_client_id);
    check_equal(document.entries[2].alternative_count, 2u);
    check_equal(flowie_control_acl_format(&document, formatted, sizeof(formatted),
                                          &formatted_size),
                SALTS_OK);
    check_equal(formatted_size, sizeof(role_canonical) - 1u);
    check_equal(formatted, role_canonical);
    check_equal(flowie_control_acl_compile(&document, "root-a", rules, 5u, &rule_count),
                SALTS_OK);
    check_equal(rule_count, 5u);
    for (size_t index = 0u; index < rule_count; ++index) {
      check_equal(rules[index].subject_kind, FLOWIE_SECURITY_SUBJECT_ROLE);
      check_equal(rules[index].subject, "publisher");
    }
    check_equal(rules[1].pattern, "root-a/telemetry/%u/+");
    check_equal(rules[2].pattern, "root-a/commands/#");
    check_equal(rules[3].pattern, "root-a/events/created");
    check_equal(rules[4].pattern, "root-a/events/updated");

    document = (flowie_control_acl_document_t)FLOWIE_CONTROL_ACL_DOCUMENT_INIT;
    formatted_size = 0u;
    check_equal(flowie_control_acl_parse(group_input, sizeof(group_input) - 1u, &document),
                SALTS_OK);
    check_equal(document.subject_kind, FLOWIE_SECURITY_SUBJECT_GROUP);
    check_equal(flowie_control_acl_format(&document, formatted, sizeof(formatted),
                                          &formatted_size),
                SALTS_OK);
    check_equal(formatted, group_input);
    rule_count = 0u;
    check_equal(flowie_control_acl_compile(&document, "root-a", rules, 5u, &rule_count),
                SALTS_OK);
    check_equal(rule_count, 1u);
    check_equal(rules[0].subject_kind, FLOWIE_SECURITY_SUBJECT_GROUP);
  }

  it("accepts placeholders in any complete tail segment and dollar in static segments") {
    static const char input[] =
        "user device-1 allow {"
        "readwrite topic root-a/$bridge/%capture/devices/%u/state "
        "read topic root-a/$bridge/%controller/command"
        "}";
    flowie_control_acl_document_t document = FLOWIE_CONTROL_ACL_DOCUMENT_INIT;

    check_equal(flowie_control_acl_parse(input, sizeof(input) - 1u, &document), SALTS_OK);
    check_true(document.entries[0].uses_username);
    check_true(document.entries[0].uses_client_id);
    check_equal(document.entries[0].topic, "root-a/$bridge/%capture/devices/%u/state");
    check_true(document.entries[1].uses_client_id);
    check_equal(document.entries[1].topic, "root-a/$bridge/%controller/command");
  }

  it("compiles connection state substitutions and alternatives into bounded internal rules") {
    static const char input[] =
        "user device-1 allow {"
        "write topic root-a/groups/region/operators/devices/%u/{event,heartbeat} "
        "read topic root-a/groups/region/operators/devices/%capture/command"
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

    check_equal(flowie_control_acl_parse(input, sizeof(input) - 1u, &document), SALTS_OK);
    check_equal(flowie_control_acl_compile(&document, "root-a", rules, 4u, &count), SALTS_OK);
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
                 "root-a/groups/region/operators/devices/%capture/command");
    check_equal(flowie_mqtt_security_matcher_init(&matcher), SALTS_OK);
    realm_config.policy_version = 1u;
    realm_config.rules = rules;
    realm_config.rule_count = count;
    realm_config.matcher = matcher;
    check_equal(flowie_security_realm_create(&realm_config, &realm), SALTS_OK);
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
    check_equal(flowie_security_realm_evaluate(realm, &request, 1u, &decision), SALTS_OK);
    check_equal(decision.effect, FLOWIE_SECURITY_ALLOW);

    mqtt.username = (flowie_mqtt_span_t){(const uint8_t *)"device-1", 8u};
    request.action = FLOWIE_SECURITY_ACTION_PUBLISH;
    request.resource_type = FLOWIE_SECURITY_RESOURCE_MQTT_TOPIC;
    request.resource = "root-a/groups/region/operators/devices/device-1/event";
    request.protocol_context = &mqtt;
    decision = (flowie_security_decision_t)FLOWIE_SECURITY_DECISION_INIT;
    check_equal(flowie_security_realm_evaluate(realm, &request, 1u, &decision), SALTS_OK);
    check_equal(decision.effect, FLOWIE_SECURITY_ALLOW);

    mqtt.client_id =
        (flowie_mqtt_span_t){(const uint8_t *)"7f4c12-capture", sizeof("7f4c12-capture") - 1u};
    request.action = FLOWIE_SECURITY_ACTION_SUBSCRIBE;
    request.resource = "root-a/groups/region/operators/devices/7f4c12/command";
    decision = (flowie_security_decision_t)FLOWIE_SECURITY_DECISION_INIT;
    check_equal(flowie_security_realm_evaluate(realm, &request, 1u, &decision), SALTS_OK);
    check_equal(decision.effect, FLOWIE_SECURITY_ALLOW);

    mqtt.client_id =
        (flowie_mqtt_span_t){(const uint8_t *)"7f4c12-controller",
                             sizeof("7f4c12-controller") - 1u};
    decision = (flowie_security_decision_t)FLOWIE_SECURITY_DECISION_INIT;
    check_equal(flowie_security_realm_evaluate(realm, &request, 1u, &decision), SALTS_OK);
    check_equal(decision.effect, FLOWIE_SECURITY_DENY);

    mqtt.client_id =
        (flowie_mqtt_span_t){(const uint8_t *)"other-capture", sizeof("other-capture") - 1u};
    decision = (flowie_security_decision_t)FLOWIE_SECURITY_DECISION_INIT;
    check_equal(flowie_security_realm_evaluate(realm, &request, 1u, &decision), SALTS_OK);
    check_equal(decision.effect, FLOWIE_SECURITY_DENY);
    flowie_security_realm_destroy(realm);
  }

  it("rejects malformed generic topics unknown subjects duplicate alternatives and denied blocks") {
    static const char unknown_subject[] = "service device-1 allow";
    static const char empty_segment[] =
        "user device-1 allow { read topic root-a//event }";
    static const char wildcard_domain[] =
        "user device-1 allow { read topic +/event }";
    static const char partial_wildcard[] =
        "user device-1 allow { read topic root-a/device+ }";
    static const char partial_placeholder[] =
        "user device-1 allow { read topic root-a/device-%capture }";
    static const char non_terminal_hash[] =
        "user device-1 allow { read topic root-a/#/event }";
    static const char non_terminal_alternatives[] =
        "user device-1 allow { read topic root-a/{east,west}/event }";
    static const char missing_tail[] =
        "user device-1 allow { read topic root-a }";
    static const char duplicate_alternative[] =
        "user device-1 allow { read topic root-a/events/{event,event} }";
    static const char excess_alternatives[] =
        "user device-1 allow { read topic root-a/events/{a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p,q} }";
    static const char denied_block[] =
        "user device-1 deny { read topic root-a/events/event }";
    flowie_control_acl_document_t document = FLOWIE_CONTROL_ACL_DOCUMENT_INIT;

    check_equal(flowie_control_acl_parse(unknown_subject, sizeof(unknown_subject) - 1u,
                                         &document),
                SALTS_EPROTO);
    check_equal(flowie_control_acl_parse(empty_segment, sizeof(empty_segment) - 1u, &document),
                SALTS_EPROTO);
    check_equal(flowie_control_acl_parse(wildcard_domain, sizeof(wildcard_domain) - 1u,
                                         &document),
                SALTS_EPROTO);
    check_equal(flowie_control_acl_parse(partial_wildcard, sizeof(partial_wildcard) - 1u,
                                         &document),
                SALTS_EPROTO);
    check_equal(flowie_control_acl_parse(partial_placeholder,
                                         sizeof(partial_placeholder) - 1u, &document),
                SALTS_EPROTO);
    check_equal(flowie_control_acl_parse(non_terminal_hash,
                                         sizeof(non_terminal_hash) - 1u, &document),
                SALTS_EPROTO);
    check_equal(flowie_control_acl_parse(non_terminal_alternatives,
                                         sizeof(non_terminal_alternatives) - 1u, &document),
                SALTS_EPROTO);
    check_equal(flowie_control_acl_parse(missing_tail, sizeof(missing_tail) - 1u, &document),
                 SALTS_EPROTO);
    check_equal(flowie_control_acl_parse(duplicate_alternative,
                                          sizeof(duplicate_alternative) - 1u, &document),
                 SALTS_EPROTO);
    check_equal(flowie_control_acl_parse(excess_alternatives,
                                         sizeof(excess_alternatives) - 1u, &document),
                SALTS_ENOSPC);
    check_equal(flowie_control_acl_parse(denied_block, sizeof(denied_block) - 1u, &document),
                 SALTS_EPROTO);
  }
}
