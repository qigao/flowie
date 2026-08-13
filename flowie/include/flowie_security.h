#ifndef FLOWIE_SECURITY_H
#define FLOWIE_SECURITY_H

#include "platform.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FLOWIE_SECURITY_ABI_V3 3u
#define FLOWIE_SECURITY_ID_MAX 255u
#define FLOWIE_SECURITY_TYPE_MAX 63u
#define FLOWIE_SECURITY_PATTERN_MAX 511u
#define FLOWIE_SECURITY_MAX_ROLES 8u
#define FLOWIE_SECURITY_MAX_GROUPS 16u
#define FLOWIE_SECURITY_MAX_RULES 4096u
#define FLOWIE_SECURITY_SECRET_REF_MAX 255u
#define FLOWIE_SECURITY_RULE_LINE_MAX 2047u

typedef struct flowie_security_realm_s flowie_security_realm_t;
typedef struct flowie_security_policy_provider_owner_s
    flowie_security_policy_provider_owner_t;
typedef struct flowie_security_decision_s flowie_security_decision_t;

typedef enum flowie_security_scope_e {
  FLOWIE_SECURITY_SCOPE_SELF = 1,
  FLOWIE_SECURITY_SCOPE_GROUP,
  FLOWIE_SECURITY_SCOPE_DOMAIN,
  FLOWIE_SECURITY_SCOPE_SYSTEM
} flowie_security_scope_t;

/** Authentication output copied by value and safe to pass into authorization. */
typedef struct flowie_security_principal_s {
  size_t size;
  uint32_t abi_version;
  char principal_id[FLOWIE_SECURITY_ID_MAX + 1u];
  char principal_type[FLOWIE_SECURITY_TYPE_MAX + 1u];
  char domain_id[FLOWIE_SECURITY_ID_MAX + 1u];
  char auth_method[FLOWIE_SECURITY_TYPE_MAX + 1u];
  flowie_security_scope_t scope;
  uint32_t role_count;
  char roles[FLOWIE_SECURITY_MAX_ROLES][FLOWIE_SECURITY_TYPE_MAX + 1u];
  uint32_t group_count;
  /** Direct groups and their Group ancestors. Domain identity is carried only by domain_id. */
  char groups[FLOWIE_SECURITY_MAX_GROUPS][FLOWIE_SECURITY_ID_MAX + 1u];
  /** Unix epoch seconds; zero means the provider did not assign an expiry. */
  uint64_t expires_at;
  /** Required realm generation; authorization rejects zero and stale generations. */
  uint64_t policy_version;
} flowie_security_principal_t;

#define FLOWIE_SECURITY_PRINCIPAL_INIT                                                         \
  {sizeof(flowie_security_principal_t), FLOWIE_SECURITY_ABI_V3}

/** Borrowed credentials visible only for the duration of authenticate(). */
typedef struct flowie_security_auth_request_s {
  size_t size;
  const char *identity;
  const char *method;
  const uint8_t *secret;
  size_t secret_size;
  const char *remote_address;
  const char *protocol;
  /**
   * Canonical transport-verified `sha256:<64-lowercase-hex>` client certificate.
   * NULL means the transport did not authenticate a client certificate. Providers
   * must size-gate this appended field before reading it.
   */
  const char *peer_certificate_sha256;
  /**
   * Direct transport peer, even when remote_address came from a trusted proxy.
   * Providers must size-gate this appended diagnostic field before reading it.
   */
  const char *transport_peer_address;
} flowie_security_auth_request_t;

#define FLOWIE_SECURITY_AUTH_REQUEST_INIT {sizeof(flowie_security_auth_request_t)}
#define FLOWIE_SECURITY_AUTH_REQUEST_BASE_SIZE                                                 \
  offsetof(flowie_security_auth_request_t, peer_certificate_sha256)

typedef int (*flowie_security_authenticate_fn)(
    void *ctx, const flowie_security_auth_request_t *request,
    flowie_security_principal_t *principal_out);

typedef struct flowie_security_auth_provider_s {
  size_t size;
  void *ctx;
  flowie_security_authenticate_fn authenticate;
} flowie_security_auth_provider_t;

#define FLOWIE_SECURITY_AUTH_PROVIDER_INIT                                                     \
  {sizeof(flowie_security_auth_provider_t), NULL, NULL}

typedef enum flowie_security_enhanced_auth_status_e {
  FLOWIE_SECURITY_ENHANCED_AUTH_CONTINUE = 1,
  FLOWIE_SECURITY_ENHANCED_AUTH_SUCCESS = 2
} flowie_security_enhanced_auth_status_t;

/** Borrowed MQTT-style enhanced authentication input for one exchange step. */
typedef struct flowie_security_enhanced_auth_request_s {
  size_t size;
  const char *identity;
  const char *method;
  const uint8_t *data;
  size_t data_size;
  const char *remote_address;
  const char *protocol;
  /** Size-gated transport identity with the same semantics as the basic request. */
  const char *peer_certificate_sha256;
  /** Size-gated direct transport peer diagnostic; see the basic request. */
  const char *transport_peer_address;
} flowie_security_enhanced_auth_request_t;

#define FLOWIE_SECURITY_ENHANCED_AUTH_REQUEST_INIT                                             \
  {sizeof(flowie_security_enhanced_auth_request_t), NULL, NULL, NULL, 0u, NULL, NULL, NULL, NULL}
#define FLOWIE_SECURITY_ENHANCED_AUTH_REQUEST_BASE_SIZE                                        \
  offsetof(flowie_security_enhanced_auth_request_t, peer_certificate_sha256)

/** Provider-owned output spans remain valid until the next provider callback or cancel(). */
typedef struct flowie_security_enhanced_auth_result_s {
  size_t size;
  flowie_security_enhanced_auth_status_t status;
  const uint8_t *data;
  size_t data_size;
  flowie_security_principal_t principal;
} flowie_security_enhanced_auth_result_t;

#define FLOWIE_SECURITY_ENHANCED_AUTH_RESULT_INIT                                              \
  {sizeof(flowie_security_enhanced_auth_result_t), 0, NULL, 0u,                                \
   FLOWIE_SECURITY_PRINCIPAL_INIT}

typedef int (*flowie_security_enhanced_auth_begin_fn)(
    void *ctx, const flowie_security_enhanced_auth_request_t *request, void **exchange_out,
    flowie_security_enhanced_auth_result_t *result_out);
typedef int (*flowie_security_enhanced_auth_continue_fn)(
    void *ctx, void *exchange, const flowie_security_enhanced_auth_request_t *request,
    flowie_security_enhanced_auth_result_t *result_out);
typedef void (*flowie_security_enhanced_auth_cancel_fn)(void *ctx, void *exchange);

typedef struct flowie_security_enhanced_auth_provider_s {
  size_t size;
  void *ctx;
  flowie_security_enhanced_auth_begin_fn begin;
  flowie_security_enhanced_auth_continue_fn continue_exchange;
  flowie_security_enhanced_auth_cancel_fn cancel;
} flowie_security_enhanced_auth_provider_t;

#define FLOWIE_SECURITY_ENHANCED_AUTH_PROVIDER_INIT                                            \
  {sizeof(flowie_security_enhanced_auth_provider_t), NULL, NULL, NULL, NULL}

CXX_C_API int flowie_security_enhanced_auth_begin(
    const flowie_security_enhanced_auth_provider_t *provider,
    const flowie_security_enhanced_auth_request_t *request, void **exchange_out,
    flowie_security_enhanced_auth_result_t *result_out);
CXX_C_API int flowie_security_enhanced_auth_continue(
    const flowie_security_enhanced_auth_provider_t *provider, void *exchange,
    const flowie_security_enhanced_auth_request_t *request,
    flowie_security_enhanced_auth_result_t *result_out);
CXX_C_API void flowie_security_enhanced_auth_cancel(
    const flowie_security_enhanced_auth_provider_t *provider, void *exchange);

typedef enum flowie_security_action_e {
  FLOWIE_SECURITY_ACTION_CONNECT = 1u << 0,
  FLOWIE_SECURITY_ACTION_PUBLISH = 1u << 1,
  FLOWIE_SECURITY_ACTION_SUBSCRIBE = 1u << 2,
  FLOWIE_SECURITY_ACTION_READ = 1u << 3,
  FLOWIE_SECURITY_ACTION_WRITE = 1u << 4,
  FLOWIE_SECURITY_ACTION_EXECUTE = 1u << 5,
  FLOWIE_SECURITY_ACTION_ADMIN = 1u << 6
} flowie_security_action_t;

#define FLOWIE_SECURITY_ACTION_ALL UINT32_C(0x7f)

typedef enum flowie_security_resource_type_e {
  FLOWIE_SECURITY_RESOURCE_GENERIC = 1,
  FLOWIE_SECURITY_RESOURCE_MQTT_TOPIC,
  FLOWIE_SECURITY_RESOURCE_FLOW_RESOURCE,
  FLOWIE_SECURITY_RESOURCE_SQL_OBJECT,
  FLOWIE_SECURITY_RESOURCE_SECRET
} flowie_security_resource_type_t;

typedef enum flowie_security_effect_e {
  FLOWIE_SECURITY_DENY = 0,
  FLOWIE_SECURITY_ALLOW = 1
} flowie_security_effect_t;

typedef enum flowie_security_subject_kind_e {
  FLOWIE_SECURITY_SUBJECT_ANY = 0,
  FLOWIE_SECURITY_SUBJECT_PRINCIPAL,
  FLOWIE_SECURITY_SUBJECT_ROLE,
  FLOWIE_SECURITY_SUBJECT_GROUP
} flowie_security_subject_kind_t;

typedef enum flowie_security_match_kind_e {
  FLOWIE_SECURITY_MATCH_EXACT = 0,
  FLOWIE_SECURITY_MATCH_PREFIX,
  /** Delegates protocol-specific semantics, such as MQTT topic filters, to matcher. */
  FLOWIE_SECURITY_MATCH_ADAPTER
} flowie_security_match_kind_t;

/** Pointer-free rule copied into one immutable realm generation. */
typedef struct flowie_security_rule_s {
  size_t size;
  uint32_t abi_version;
  flowie_security_effect_t effect;
  flowie_security_subject_kind_t subject_kind;
  char subject[FLOWIE_SECURITY_ID_MAX + 1u];
  /** Immutable security-tree root. Every rule belongs to exactly one domain. */
  char domain_id[FLOWIE_SECURITY_ID_MAX + 1u];
  uint32_t action_mask;
  flowie_security_resource_type_t resource_type;
  flowie_security_match_kind_t match_kind;
  char pattern[FLOWIE_SECURITY_PATTERN_MAX + 1u];
} flowie_security_rule_t;

#define FLOWIE_SECURITY_RULE_INIT                                                              \
  {sizeof(flowie_security_rule_t), FLOWIE_SECURITY_ABI_V3, FLOWIE_SECURITY_DENY}

/**
 * Parse one canonical ACL rule line:
 * `effect|subject_kind|subject|domain|actions|resource_type|match_kind|pattern`.
 * `subject` must be `*` for `any`; actions are comma-separated. `\\`, `\|`, and
 * `\xHH` escapes are accepted in subject, domain, and pattern fields.
 *
 * The parser copies into `rule_out`, performs no allocation, and returns TURBO_OK,
 * TURBO_EINVAL for invalid pointers/capacities, or TURBO_EPROTO for invalid syntax.
 */
CXX_C_API int flowie_security_rule_parse_line(const char *line, size_t line_size,
                                                  flowie_security_rule_t *rule_out);
/**
 * Serialize one validated rule to the canonical line representation.
 * `line_out` is not NUL-terminated by contract; `line_size_out` receives its byte count.
 * Returns TURBO_OK, TURBO_EINVAL for invalid input, or TURBO_ENOSPC for insufficient capacity.
 */
CXX_C_API int flowie_security_rule_format_line(const flowie_security_rule_t *rule,
                                                   char *line_out, size_t line_capacity,
                                                   size_t *line_size_out);

/** Immutable provider-owned policy view. Rules remain borrowed until release(). */
typedef struct flowie_security_policy_bundle_s {
  size_t size;
  uint32_t abi_version;
  uint64_t policy_version;
  /** Unix epoch seconds; zero means the bundle does not expire. */
  uint64_t expires_at;
  const flowie_security_rule_t *rules;
  size_t rule_count;
  void *provider_bundle;
} flowie_security_policy_bundle_t;

#define FLOWIE_SECURITY_POLICY_BUNDLE_INIT                                                     \
  {sizeof(flowie_security_policy_bundle_t), FLOWIE_SECURITY_ABI_V3, 0u, 0u, NULL, 0u, NULL}

/**
 * Load the requested immutable policy generation.
 *
 * required_version is positive for authorization refreshes and zero for the provider's current
 * generation. Implementations may yield when called from a coroutine, but must not retain realm
 * or request pointers. A successful load must be paired with release().
 */
typedef int (*flowie_security_policy_load_fn)(void *ctx, uint64_t required_version,
                                                  flowie_security_policy_bundle_t *bundle_out);
typedef void (*flowie_security_policy_release_fn)(void *ctx,
                                                      flowie_security_policy_bundle_t *bundle);

typedef struct flowie_security_policy_provider_s {
  size_t size;
  void *ctx;
  flowie_security_policy_load_fn load;
  flowie_security_policy_release_fn release;
} flowie_security_policy_provider_t;

#define FLOWIE_SECURITY_POLICY_PROVIDER_INIT                                                   \
  {sizeof(flowie_security_policy_provider_t), NULL, NULL, NULL}

typedef struct flowie_security_request_s {
  size_t size;
  const flowie_security_principal_t *principal;
  const char *domain_id;
  uint32_t action;
  flowie_security_resource_type_t resource_type;
  const char *resource;
  /** Protocol-owner facts available to an injected matcher; Core never dereferences it. */
  const void *protocol_context;
  /** Optional bounded MQTT identity facts forwarded to centralized authorization providers. */
  const uint8_t *username;
  size_t username_size;
  const uint8_t *client_id;
  size_t client_id_size;
} flowie_security_request_t;

#define FLOWIE_SECURITY_REQUEST_INIT                                                            \
  {sizeof(flowie_security_request_t), NULL, NULL, 0u, 0, NULL, NULL, NULL, 0u, NULL, 0u}

/**
 * Immutable adapter leaf input borrowed only for compile_leaf(). Candidate indices are ordered
 * positions in rules and remain owned by Core. A compiled leaf may borrow rule strings until its
 * destroy_leaf() call; it must not retain candidate_rule_indices.
 */
typedef struct flowie_security_matcher_leaf_s {
  size_t size;
  const flowie_security_rule_t *rules;
  size_t rule_count;
  const size_t *candidate_rule_indices;
  size_t candidate_count;
} flowie_security_matcher_leaf_t;

#define FLOWIE_SECURITY_MATCHER_LEAF_INIT                                                      \
  {sizeof(flowie_security_matcher_leaf_t), NULL, 0u, NULL, 0u}

/** Emit one leaf-local candidate position. The adapter must stop if this returns an error. */
typedef int (*flowie_security_match_emit_fn)(void *ctx, size_t candidate_position);

/** Build one immutable, concurrently readable adapter index for a subject leaf. */
typedef int (*flowie_security_match_compile_leaf_fn)(
    void *ctx, const flowie_security_matcher_leaf_t *leaf, void **compiled_leaf_out);

/**
 * Evaluate one compiled leaf without mutating it. Each match is emitted at most once and must be
 * in [0, candidate_count). Core validates emitted positions and retains all authorization policy.
 */
typedef int (*flowie_security_match_evaluate_leaf_fn)(
    void *ctx, const void *compiled_leaf, const flowie_security_request_t *request,
    flowie_security_match_emit_fn emit, void *emit_ctx);

typedef void (*flowie_security_match_destroy_leaf_fn)(void *ctx, void *compiled_leaf);

typedef struct flowie_security_matcher_s {
  size_t size;
  uint32_t abi_version;
  void *ctx;
  flowie_security_match_compile_leaf_fn compile_leaf;
  flowie_security_match_evaluate_leaf_fn evaluate_leaf;
  flowie_security_match_destroy_leaf_fn destroy_leaf;
} flowie_security_matcher_t;

#define FLOWIE_SECURITY_MATCHER_INIT                                                           \
  {sizeof(flowie_security_matcher_t), FLOWIE_SECURITY_ABI_V3, NULL, NULL, NULL, NULL}

typedef enum flowie_security_decision_reason_e {
  FLOWIE_SECURITY_REASON_DEFAULT_DENY = 1,
  FLOWIE_SECURITY_REASON_ALLOW_RULE,
  FLOWIE_SECURITY_REASON_DENY_RULE,
  FLOWIE_SECURITY_REASON_DOMAIN_MISMATCH,
  FLOWIE_SECURITY_REASON_PRINCIPAL_EXPIRED,
  FLOWIE_SECURITY_REASON_POLICY_VERSION_MISMATCH
} flowie_security_decision_reason_t;

struct flowie_security_decision_s {
  size_t size;
  flowie_security_effect_t effect;
  flowie_security_decision_reason_t reason;
  size_t matched_rule;
  uint64_t policy_version;
};

#define FLOWIE_SECURITY_DECISION_INIT                                                          \
  {sizeof(flowie_security_decision_t), FLOWIE_SECURITY_DENY,                               \
   FLOWIE_SECURITY_REASON_DEFAULT_DENY, SIZE_MAX, 0u}

/** Remote or otherwise centralized per-request authorization decision provider. */
typedef int (*flowie_security_authorize_fn)(
    void *ctx, const flowie_security_request_t *request, uint64_t now_epoch_seconds,
    flowie_security_decision_t *decision_out);

typedef struct flowie_security_authorization_provider_s {
  size_t size;
  void *ctx;
  flowie_security_authorize_fn authorize;
} flowie_security_authorization_provider_t;

#define FLOWIE_SECURITY_AUTHORIZATION_PROVIDER_INIT                                           \
  {sizeof(flowie_security_authorization_provider_t), NULL, NULL}

typedef struct flowie_security_realm_config_s {
  size_t size;
  uint32_t abi_version;
  const char *resource_uid;
  const char *owner_name;
  uint64_t policy_version;
  const flowie_security_rule_t *rules;
  size_t rule_count;
  flowie_security_matcher_t matcher;
  /** Required when no initial rules are supplied; copied and resolved by the product root. */
  const char *policy_source;
} flowie_security_realm_config_t;

#define FLOWIE_SECURITY_REALM_CONFIG_INIT                                                      \
  {sizeof(flowie_security_realm_config_t),                                                     \
   FLOWIE_SECURITY_ABI_V3,                                                                     \
   NULL,                                                                                           \
   NULL,                                                                                           \
   0u,                                                                                             \
   NULL,                                                                                           \
   0u,                                                                                             \
   FLOWIE_SECURITY_MATCHER_INIT,                                                               \
   NULL}

/** Validate provider output and authenticate without retaining credential bytes. */
CXX_C_API int flowie_security_authenticate(const flowie_security_auth_provider_t *provider,
                                               const flowie_security_auth_request_t *request,
                                               flowie_security_principal_t *principal_out);

CXX_C_API int flowie_security_realm_create(const flowie_security_realm_config_t *config,
                                               flowie_security_realm_t **out);
CXX_C_API void flowie_security_realm_destroy(flowie_security_realm_t *realm);

/** Borrowed configured source channel name, or NULL for a programmatic static realm. */
CXX_C_API const char *
flowie_security_realm_policy_source(const flowie_security_realm_t *realm);

/** Bind one borrowed provider before the realm becomes reachable by protocol adapters. */
CXX_C_API int flowie_security_realm_bind_policy_provider(
    flowie_security_realm_t *realm, const flowie_security_policy_provider_t *provider);

/** Bind one per-request decision provider instead of a policy-bundle provider. */
CXX_C_API int flowie_security_realm_bind_authorization_provider(
    flowie_security_realm_t *realm,
    const flowie_security_authorization_provider_t *provider);

/** Fetch, validate, copy, and atomically install one exact generation. */
CXX_C_API int flowie_security_realm_refresh(flowie_security_realm_t *realm,
                                                uint64_t required_version,
                                                uint64_t now_epoch_seconds);

/** Complete one deterministic decision. Deny is a valid result and returns TURBO_OK. */
CXX_C_API int flowie_security_realm_evaluate(flowie_security_realm_t *realm,
                                                 const flowie_security_request_t *request,
                                                 uint64_t now_epoch_seconds,
                                                 flowie_security_decision_t *decision);

/** Evaluate and map a deny decision to TURBO_EPERM for protocol-owner boundaries. */
CXX_C_API int flowie_security_realm_authorize(flowie_security_realm_t *realm,
                                                  const flowie_security_request_t *request,
                                                  uint64_t now_epoch_seconds,
                                                  flowie_security_decision_t *decision);

/** Register the realm as a borrowed, independently addressable SECURITY_REALM resource. */
#ifdef __cplusplus
}
#endif

#endif /* FLOWIE_SECURITY_H */
