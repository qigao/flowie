#include "flowie_security.h"

#include "turbo_error.h"

#include <stdlib.h>
#include <string.h>

struct flowie_security_realm_s {
  uint64_t policy_version;
  flowie_security_rule_t *rules;
  size_t rule_count;
  flowie_security_matcher_t matcher;
  void *compiled_leaf;
  flowie_security_authorization_provider_t authorization;
  flowie_security_policy_provider_t policy;
  char *policy_source;
};

static int flowie_security_principal_valid(const flowie_security_principal_t *principal) {
  return principal && principal->size >= sizeof(*principal) &&
         principal->abi_version == FLOWIE_SECURITY_ABI_V3 && principal->principal_id[0] != '\0' &&
         principal->principal_type[0] != '\0' && principal->auth_method[0] != '\0' &&
         principal->scope >= FLOWIE_SECURITY_SCOPE_SELF &&
         principal->scope <= FLOWIE_SECURITY_SCOPE_SYSTEM &&
         principal->role_count <= FLOWIE_SECURITY_MAX_ROLES &&
         principal->group_count <= FLOWIE_SECURITY_MAX_GROUPS && principal->policy_version != 0u;
}

int flowie_security_authenticate(const flowie_security_auth_provider_t *provider,
                                 const flowie_security_auth_request_t *request,
                                 flowie_security_principal_t *principal_out) {
  flowie_security_principal_t principal = FLOWIE_SECURITY_PRINCIPAL_INIT;
  int rc;
  if (!provider || provider->size < sizeof(*provider) || !provider->authenticate || !request ||
      request->size < FLOWIE_SECURITY_AUTH_REQUEST_BASE_SIZE || !principal_out)
    return TURBO_EINVAL;
  rc = provider->authenticate(provider->ctx, request, &principal);
  if (rc != TURBO_OK) return rc;
  if (!flowie_security_principal_valid(&principal)) return TURBO_EPROTO;
  *principal_out = principal;
  return TURBO_OK;
}

static int flowie_security_enhanced_result_valid(
    const flowie_security_enhanced_auth_result_t *result) {
  if (!result || result->size < sizeof(*result) || (!result->data && result->data_size != 0u))
    return 0;
  if (result->status == FLOWIE_SECURITY_ENHANCED_AUTH_CONTINUE) return 1;
  return result->status == FLOWIE_SECURITY_ENHANCED_AUTH_SUCCESS &&
         flowie_security_principal_valid(&result->principal);
}

int flowie_security_enhanced_auth_begin(
    const flowie_security_enhanced_auth_provider_t *provider,
    const flowie_security_enhanced_auth_request_t *request, void **exchange_out,
    flowie_security_enhanced_auth_result_t *result_out) {
  int rc;
  if (exchange_out) *exchange_out = NULL;
  if (!provider || provider->size < sizeof(*provider) || !provider->begin || !provider->cancel ||
      !request || request->size < FLOWIE_SECURITY_ENHANCED_AUTH_REQUEST_BASE_SIZE || !exchange_out ||
      !result_out)
    return TURBO_EINVAL;
  rc = provider->begin(provider->ctx, request, exchange_out, result_out);
  if (rc != TURBO_OK) return rc;
  if (!*exchange_out || !flowie_security_enhanced_result_valid(result_out)) {
    if (*exchange_out) provider->cancel(provider->ctx, *exchange_out);
    *exchange_out = NULL;
    return TURBO_EPROTO;
  }
  return TURBO_OK;
}

int flowie_security_enhanced_auth_continue(
    const flowie_security_enhanced_auth_provider_t *provider, void *exchange,
    const flowie_security_enhanced_auth_request_t *request,
    flowie_security_enhanced_auth_result_t *result_out) {
  int rc;
  if (!provider || provider->size < sizeof(*provider) || !provider->continue_exchange ||
      !provider->cancel || !exchange || !request ||
      request->size < FLOWIE_SECURITY_ENHANCED_AUTH_REQUEST_BASE_SIZE || !result_out)
    return TURBO_EINVAL;
  rc = provider->continue_exchange(provider->ctx, exchange, request, result_out);
  return rc != TURBO_OK || flowie_security_enhanced_result_valid(result_out) ? rc : TURBO_EPROTO;
}

void flowie_security_enhanced_auth_cancel(
    const flowie_security_enhanced_auth_provider_t *provider, void *exchange) {
  if (provider && provider->size >= sizeof(*provider) && provider->cancel && exchange)
    provider->cancel(provider->ctx, exchange);
}

static int flowie_security_subject_matches(const flowie_security_rule_t *rule,
                                           const flowie_security_principal_t *principal) {
  if (rule->subject_kind == FLOWIE_SECURITY_SUBJECT_ANY) return 1;
  if (rule->subject_kind == FLOWIE_SECURITY_SUBJECT_PRINCIPAL)
    return strcmp(rule->subject, principal->principal_id) == 0;
  if (rule->subject_kind == FLOWIE_SECURITY_SUBJECT_ROLE) {
    for (uint32_t i = 0u; i < principal->role_count; ++i)
      if (strcmp(rule->subject, principal->roles[i]) == 0) return 1;
  } else if (rule->subject_kind == FLOWIE_SECURITY_SUBJECT_GROUP) {
    for (uint32_t i = 0u; i < principal->group_count; ++i)
      if (strcmp(rule->subject, principal->groups[i]) == 0) return 1;
  }
  return 0;
}

typedef struct flowie_security_match_state_s {
  uint8_t *matches;
  size_t count;
} flowie_security_match_state_t;

static int flowie_security_emit_match(void *ctx, size_t position) {
  flowie_security_match_state_t *state = (flowie_security_match_state_t *)ctx;
  if (!state || position >= state->count) return TURBO_EPROTO;
  state->matches[position] = 1u;
  return TURBO_OK;
}

int flowie_security_realm_create(const flowie_security_realm_config_t *config,
                                 flowie_security_realm_t **out) {
  flowie_security_realm_t *realm;
  if (out) *out = NULL;
  if (!config || config->size < sizeof(*config) || config->abi_version != FLOWIE_SECURITY_ABI_V3 ||
      !out || config->rule_count > FLOWIE_SECURITY_MAX_RULES ||
      (config->rule_count != 0u && (!config->rules || config->policy_version == 0u)))
    return TURBO_EINVAL;
  realm = (flowie_security_realm_t *)calloc(1u, sizeof(*realm));
  if (!realm) return TURBO_ENOMEM;
  realm->policy_version = config->policy_version;
  realm->rule_count = config->rule_count;
  realm->matcher = config->matcher;
  if (config->policy_source) {
    size_t size = strlen(config->policy_source) + 1u;
    realm->policy_source = (char *)malloc(size);
    if (realm->policy_source) memcpy(realm->policy_source, config->policy_source, size);
  }
  if (realm->rule_count != 0u) {
    size_t *indices;
    flowie_security_matcher_leaf_t leaf = FLOWIE_SECURITY_MATCHER_LEAF_INIT;
    realm->rules = (flowie_security_rule_t *)malloc(realm->rule_count * sizeof(*realm->rules));
    indices = (size_t *)malloc(realm->rule_count * sizeof(*indices));
    if (!realm->rules || !indices) {
      free(indices);
      flowie_security_realm_destroy(realm);
      return TURBO_ENOMEM;
    }
    memcpy(realm->rules, config->rules, realm->rule_count * sizeof(*realm->rules));
    for (size_t i = 0u; i < realm->rule_count; ++i) indices[i] = i;
    if (realm->matcher.compile_leaf) {
      int rc;
      leaf.rules = realm->rules;
      leaf.rule_count = realm->rule_count;
      leaf.candidate_rule_indices = indices;
      leaf.candidate_count = realm->rule_count;
      rc = realm->matcher.compile_leaf(realm->matcher.ctx, &leaf, &realm->compiled_leaf);
      free(indices);
      if (rc != TURBO_OK) {
        flowie_security_realm_destroy(realm);
        return rc;
      }
    } else {
      free(indices);
    }
  }
  if (config->policy_source && !realm->policy_source) {
    flowie_security_realm_destroy(realm);
    return TURBO_ENOMEM;
  }
  *out = realm;
  return TURBO_OK;
}

void flowie_security_realm_destroy(flowie_security_realm_t *realm) {
  if (!realm) return;
  if (realm->compiled_leaf && realm->matcher.destroy_leaf)
    realm->matcher.destroy_leaf(realm->matcher.ctx, realm->compiled_leaf);
  free(realm->rules);
  free(realm->policy_source);
  free(realm);
}

const char *flowie_security_realm_policy_source(const flowie_security_realm_t *realm) {
  return realm ? realm->policy_source : NULL;
}

int flowie_security_realm_bind_policy_provider(
    flowie_security_realm_t *realm, const flowie_security_policy_provider_t *provider) {
  if (!realm || !provider || provider->size < sizeof(*provider) || !provider->load ||
      !provider->release)
    return TURBO_EINVAL;
  realm->policy = *provider;
  return TURBO_OK;
}

int flowie_security_realm_bind_authorization_provider(
    flowie_security_realm_t *realm,
    const flowie_security_authorization_provider_t *provider) {
  if (!realm || !provider || provider->size < sizeof(*provider) || !provider->authorize)
    return TURBO_EINVAL;
  realm->authorization = *provider;
  return TURBO_OK;
}

int flowie_security_realm_evaluate(flowie_security_realm_t *realm,
                                   const flowie_security_request_t *request,
                                   uint64_t now_epoch_seconds,
                                   flowie_security_decision_t *decision_out) {
  flowie_security_decision_t decision = FLOWIE_SECURITY_DECISION_INIT;
  uint8_t *adapter_matches = NULL;
  int rc = TURBO_OK;
  if (!realm || !request || request->size < sizeof(*request) || !request->principal ||
      !request->domain_id || !request->resource || !decision_out ||
      !flowie_security_principal_valid(request->principal))
    return TURBO_EINVAL;
  decision.policy_version = realm->policy_version;
  if (strcmp(request->domain_id, request->principal->domain_id) != 0 &&
      request->principal->scope != FLOWIE_SECURITY_SCOPE_SYSTEM) {
    decision.reason = FLOWIE_SECURITY_REASON_DOMAIN_MISMATCH;
    *decision_out = decision;
    return TURBO_OK;
  }
  if (request->principal->expires_at != 0u && request->principal->expires_at <= now_epoch_seconds) {
    decision.reason = FLOWIE_SECURITY_REASON_PRINCIPAL_EXPIRED;
    *decision_out = decision;
    return TURBO_OK;
  }
  if (request->principal->policy_version != realm->policy_version) {
    decision.reason = FLOWIE_SECURITY_REASON_POLICY_VERSION_MISMATCH;
    *decision_out = decision;
    return TURBO_OK;
  }
  if (realm->compiled_leaf && realm->matcher.evaluate_leaf) {
    flowie_security_match_state_t state;
    adapter_matches = (uint8_t *)calloc(realm->rule_count, 1u);
    if (!adapter_matches) return TURBO_ENOMEM;
    state = (flowie_security_match_state_t){adapter_matches, realm->rule_count};
    rc = realm->matcher.evaluate_leaf(realm->matcher.ctx, realm->compiled_leaf, request,
                                      flowie_security_emit_match, &state);
    if (rc != TURBO_OK) {
      free(adapter_matches);
      return rc;
    }
  }
  for (size_t i = 0u; i < realm->rule_count; ++i) {
    const flowie_security_rule_t *rule = &realm->rules[i];
    int resource_matches = 0;
    if (strcmp(rule->domain_id, request->domain_id) != 0 ||
        (rule->action_mask & request->action) == 0u || rule->resource_type != request->resource_type ||
        !flowie_security_subject_matches(rule, request->principal))
      continue;
    if (rule->match_kind == FLOWIE_SECURITY_MATCH_EXACT)
      resource_matches = strcmp(rule->pattern, request->resource) == 0;
    else if (rule->match_kind == FLOWIE_SECURITY_MATCH_PREFIX)
      resource_matches = strncmp(request->resource, rule->pattern, strlen(rule->pattern)) == 0;
    else if (rule->match_kind == FLOWIE_SECURITY_MATCH_ADAPTER && adapter_matches)
      resource_matches = adapter_matches[i] != 0u;
    if (!resource_matches) continue;
    decision.matched_rule = i;
    decision.effect = rule->effect;
    decision.reason = rule->effect == FLOWIE_SECURITY_ALLOW ? FLOWIE_SECURITY_REASON_ALLOW_RULE
                                                            : FLOWIE_SECURITY_REASON_DENY_RULE;
    if (rule->effect == FLOWIE_SECURITY_DENY) break;
  }
  free(adapter_matches);
  *decision_out = decision;
  return TURBO_OK;
}

int flowie_security_realm_authorize(flowie_security_realm_t *realm,
                                    const flowie_security_request_t *request,
                                    uint64_t now_epoch_seconds,
                                    flowie_security_decision_t *decision_out) {
  if (!realm) return TURBO_EINVAL;
  if (realm->authorization.authorize)
    return realm->authorization.authorize(realm->authorization.ctx, request, now_epoch_seconds,
                                          decision_out);
  return flowie_security_realm_evaluate(realm, request, now_epoch_seconds, decision_out);
}
