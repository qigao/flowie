#include "flowie.h"
#include "flowie_security_internal.h"
#include "flowie_topic_index_internal.h"

#include "salts_error.h"

#include <stdlib.h>
#include <string.h>

int flowie_publish_message_map(const flowie_mqtt_publish_view_t *publish,
                               flowie_mqtt_version_t version, uint64_t owner_instance_id,
                               uint64_t session_id, uint64_t session_generation,
                               flowie_publish_message_view_t *out) {
  flowie_publish_message_view_t mapped = FLOWIE_PUBLISH_MESSAGE_VIEW_INIT;
  int rc;
  if (!publish || publish->size < sizeof(*publish) ||
      publish->abi_version != FLOWIE_MQTT_PROTOCOL_ABI_V1 || !out || out->size != sizeof(*out) ||
      owner_instance_id == 0u || session_id == 0u || session_generation == 0u ||
      (!publish->topic.data && publish->topic.size != 0u) ||
      (!publish->payload.data && publish->payload.size != 0u) ||
      !flowie_mqtt_version_is_supported(version) || publish->qos > 2u) {
    return SALTS_EINVAL;
  }
  mapped.metadata.protocol = FLOWIE_PROTOCOL_MQTT;
  mapped.metadata.protocol_version = (uint32_t)version;
  mapped.metadata.kind = FLOWIE_PROTOCOL_MESSAGE_DATA;
  mapped.metadata.qos = publish->qos;
  mapped.metadata.packet_id = publish->packet_id;
  mapped.metadata.session_generation = session_generation;
  mapped.metadata.duplicate = publish->duplicate;
  mapped.metadata.retain = publish->retain;
  rc = flowie_protocol_message_validate(&mapped.metadata);
  if (rc != SALTS_OK) return rc;
  mapped.route.protocol = FLOWIE_PROTOCOL_MQTT;
  mapped.route.owner_instance_id = owner_instance_id;
  mapped.route.session_id = session_id;
  mapped.route.session_generation = session_generation;
  mapped.topic = publish->topic;
  mapped.properties = publish->properties;
  mapped.payload = publish->payload;
  *out = mapped;
  return SALTS_OK;
}

typedef struct flowie_mqtt_security_leaf_s {
  flowie_topic_index_t topics;
  struct flowie_mqtt_security_candidate_s *candidates;
  size_t candidate_count;
} flowie_mqtt_security_leaf_t;

typedef struct flowie_mqtt_security_candidate_s {
  const char *pattern;
  int uses_placeholders;
} flowie_mqtt_security_candidate_t;

static const uint8_t FLOWIE_MQTT_VALIDATED_SECURITY_PROVENANCE = 0u;

static int flowie_security_principal_text_validate(const char *value, size_t capacity,
                                                   int required) {
  const char *end;
  if (!value || capacity == 0u) return SALTS_EINVAL;
  end = (const char *)memchr(value, '\0', capacity);
  return !end || (required && end == value) ? SALTS_EPROTO : SALTS_OK;
}

int flowie_security_principal_validate(const flowie_security_principal_t *principal) {
  if (!principal || principal->size < sizeof(*principal) ||
      principal->abi_version != FLOWIE_SECURITY_ABI_V3 ||
      principal->scope < FLOWIE_SECURITY_SCOPE_SELF ||
      principal->scope > FLOWIE_SECURITY_SCOPE_SYSTEM ||
      principal->role_count > FLOWIE_SECURITY_MAX_ROLES ||
      principal->group_count > FLOWIE_SECURITY_MAX_GROUPS || principal->policy_version == 0u ||
      flowie_security_principal_text_validate(principal->principal_id,
                                              sizeof(principal->principal_id), 1) != SALTS_OK ||
      flowie_security_principal_text_validate(principal->principal_type,
                                              sizeof(principal->principal_type), 1) != SALTS_OK ||
      flowie_security_principal_text_validate(
          principal->domain_id, sizeof(principal->domain_id),
          principal->scope != FLOWIE_SECURITY_SCOPE_SYSTEM) != SALTS_OK ||
      flowie_security_principal_text_validate(principal->auth_method,
                                              sizeof(principal->auth_method), 1) != SALTS_OK)
    return SALTS_EPROTO;
  for (uint32_t index = 0u; index < principal->role_count; ++index)
    if (flowie_security_principal_text_validate(principal->roles[index],
                                                sizeof(principal->roles[index]), 1) != SALTS_OK)
      return SALTS_EPROTO;
  for (uint32_t index = 0u; index < principal->group_count; ++index) {
    if (flowie_security_principal_text_validate(principal->groups[index],
                                                sizeof(principal->groups[index]), 1) != SALTS_OK)
      return SALTS_EPROTO;
    for (uint32_t prior = 0u; prior < index; ++prior)
      if (strcmp(principal->groups[index], principal->groups[prior]) == 0) return SALTS_EPROTO;
  }
  return SALTS_OK;
}

int flowie_mqtt_validated_security_context_init(flowie_mqtt_validated_security_context_t *out,
                                                flowie_mqtt_security_resource_kind_t kind,
                                                tstr parser_validated_resource) {
  flowie_mqtt_validated_security_context_t initialized =
      FLOWIE_MQTT_VALIDATED_SECURITY_CONTEXT_INIT;
  if (!out || (kind != FLOWIE_MQTT_SECURITY_TOPIC && kind != FLOWIE_MQTT_SECURITY_TOPIC_FILTER) ||
      !parser_validated_resource || tstr_len(parser_validated_resource) == 0u)
    return SALTS_EINVAL;
  initialized.public_context.kind = kind;
  initialized.resource = (flowie_mqtt_span_t){(const uint8_t *)parser_validated_resource,
                                              tstr_len(parser_validated_resource)};
  initialized.provenance = &FLOWIE_MQTT_VALIDATED_SECURITY_PROVENANCE;
  *out = initialized;
  return SALTS_OK;
}

static int flowie_mqtt_security_resource(const flowie_security_request_t *request,
                                         flowie_mqtt_span_t *resource_out,
                                         flowie_mqtt_security_resource_kind_t *kind_out,
                                         int *validated_out,
                                         const flowie_mqtt_security_context_t **context_out) {
  const flowie_mqtt_security_context_t *context;
  flowie_mqtt_span_t resource;
  flowie_mqtt_security_resource_kind_t kind = FLOWIE_MQTT_SECURITY_TOPIC;
  int validated = 0;
  if (!request || !request->resource || !resource_out || !kind_out || !validated_out ||
      !context_out)
    return SALTS_EINVAL;
  context = (const flowie_mqtt_security_context_t *)request->protocol_context;
  if (context) {
    if (context->size < offsetof(flowie_mqtt_security_context_t, username)) return SALTS_EPROTO;
    kind = context->kind;
    if (kind != FLOWIE_MQTT_SECURITY_TOPIC && kind != FLOWIE_MQTT_SECURITY_TOPIC_FILTER)
      return SALTS_EPROTO;
    if (context->size == sizeof(flowie_mqtt_validated_security_context_t)) {
      const flowie_mqtt_validated_security_context_t *trusted =
          (const flowie_mqtt_validated_security_context_t *)context;
      if (trusted->abi_version != FLOWIE_MQTT_VALIDATED_SECURITY_CONTEXT_ABI_V1 ||
          trusted->provenance != &FLOWIE_MQTT_VALIDATED_SECURITY_PROVENANCE ||
          trusted->resource.data != (const uint8_t *)request->resource ||
          trusted->resource.size == 0u) {
        return SALTS_EPROTO;
      }
      resource = trusted->resource;
      validated = 1;
    } else {
      resource =
          (flowie_mqtt_span_t){(const uint8_t *)request->resource, strlen(request->resource)};
    }
  } else {
    resource = (flowie_mqtt_span_t){(const uint8_t *)request->resource, strlen(request->resource)};
  }
  *resource_out = resource;
  *kind_out = kind;
  *validated_out = validated;
  *context_out = context;
  return SALTS_OK;
}

static int flowie_mqtt_security_filter_compile(const char *pattern, char *filter_out,
                                               size_t filter_capacity,
                                               int *uses_placeholders_out) {
  const char *cursor;
  size_t written = 0u;
  int uses_placeholders = 0;
  if (!pattern || !pattern[0] || !filter_out || filter_capacity == 0u ||
      !uses_placeholders_out)
    return SALTS_EINVAL;
  cursor = pattern;
  while (*cursor) {
    const char *end = strchr(cursor, '/');
    size_t segment_size = end ? (size_t)(end - cursor) : strlen(cursor);
    const char *segment = cursor;
    if (segment_size > 1u && segment[0] == '%') {
      segment = "+";
      segment_size = 1u;
      uses_placeholders = 1;
    }
    if (segment_size > filter_capacity - 1u - written) return SALTS_ENOSPC;
    memcpy(filter_out + written, segment, segment_size);
    written += segment_size;
    if (!end) break;
    if (written >= filter_capacity - 1u) return SALTS_ENOSPC;
    filter_out[written++] = '/';
    cursor = end + 1u;
  }
  filter_out[written] = '\0';
  *uses_placeholders_out = uses_placeholders;
  return SALTS_OK;
}

static int flowie_mqtt_security_identity_segment_valid(flowie_mqtt_span_t value) {
  if (!value.data || value.size == 0u) return 0;
  for (size_t index = 0u; index < value.size; ++index)
    if (value.data[index] == '/' || value.data[index] == '+' || value.data[index] == '#') return 0;
  return 1;
}

static int flowie_mqtt_security_role_client_id(flowie_mqtt_span_t client_id,
                                               const char *role, size_t role_size,
                                               flowie_mqtt_span_t *base_out) {
  flowie_mqtt_span_t role_span;
  size_t role_offset;
  if (!role || role_size == 0u || !base_out || client_id.size <= role_size + 1u ||
      !flowie_mqtt_security_identity_segment_valid(client_id))
    return 0;
  role_span = (flowie_mqtt_span_t){(const uint8_t *)role, role_size};
  if (!flowie_mqtt_security_identity_segment_valid(role_span)) return 0;
  role_offset = client_id.size - role_size;
  if (client_id.data[role_offset - 1u] != '-' ||
      memcmp(client_id.data + role_offset, role, role_size) != 0)
    return 0;
  *base_out = (flowie_mqtt_span_t){client_id.data, role_offset - 1u};
  return flowie_mqtt_security_identity_segment_valid(*base_out);
}

static int flowie_mqtt_security_placeholders_match(
    const char *pattern, flowie_mqtt_span_t resource,
    const flowie_mqtt_security_context_t *context) {
  const char *pattern_cursor = pattern;
  const uint8_t *resource_cursor = resource.data;
  const uint8_t *resource_limit = resource.data + resource.size;
  if (!pattern || !resource.data || !context || context->size < sizeof(*context)) return 0;
  while (*pattern_cursor) {
    const char *pattern_end = strchr(pattern_cursor, '/');
    const uint8_t *resource_end =
        (const uint8_t *)memchr(resource_cursor, '/', (size_t)(resource_limit - resource_cursor));
    size_t pattern_size =
        pattern_end ? (size_t)(pattern_end - pattern_cursor) : strlen(pattern_cursor);
    size_t resource_size =
        resource_end ? (size_t)(resource_end - resource_cursor)
                     : (size_t)(resource_limit - resource_cursor);
    if (pattern_size == 2u && pattern_cursor[0] == '%' &&
        (pattern_cursor[1] == 'u' || pattern_cursor[1] == 'c')) {
      flowie_mqtt_span_t expected =
          pattern_cursor[1] == 'u' ? context->username : context->client_id;
      if (!flowie_mqtt_security_identity_segment_valid(expected) || resource_size != expected.size ||
          memcmp(resource_cursor, expected.data, resource_size) != 0)
        return 0;
    } else if (pattern_size > 1u && pattern_cursor[0] == '%') {
      flowie_mqtt_span_t expected;
      if (!flowie_mqtt_security_role_client_id(context->client_id, pattern_cursor + 1u,
                                                pattern_size - 1u, &expected) ||
          resource_size != expected.size ||
          memcmp(resource_cursor, expected.data, resource_size) != 0)
        return 0;
    }
    if (!pattern_end) break;
    if (!resource_end) return 0;
    pattern_cursor = pattern_end + 1u;
    resource_cursor = resource_end + 1u;
  }
  return 1;
}

static int flowie_mqtt_security_compile_leaf(void *ctx,
                                             const flowie_security_matcher_leaf_t *input,
                                             void **compiled_leaf_out) {
  flowie_mqtt_security_leaf_t *compiled;
  int rc;
  (void)ctx;
  if (compiled_leaf_out) *compiled_leaf_out = NULL;
  if (!input || input->size < sizeof(*input) || !input->rules || input->rule_count == 0u ||
      !input->candidate_rule_indices || input->candidate_count == 0u || !compiled_leaf_out)
    return SALTS_EINVAL;
  compiled = (flowie_mqtt_security_leaf_t *)calloc(1u, sizeof(*compiled));
  if (!compiled) return SALTS_ENOMEM;
  rc = flowie_topic_index_init(&compiled->topics);
  if (rc == SALTS_OK) {
    compiled->candidates = (flowie_mqtt_security_candidate_t *)calloc(
        input->candidate_count, sizeof(*compiled->candidates));
    if (!compiled->candidates) rc = SALTS_ENOMEM;
  }
  compiled->candidate_count = input->candidate_count;
  for (size_t position = 0u; rc == SALTS_OK && position < input->candidate_count; ++position) {
    size_t rule_index = input->candidate_rule_indices[position];
    const flowie_security_rule_t *rule =
        rule_index < input->rule_count ? &input->rules[rule_index] : NULL;
    flowie_mqtt_span_t filter;
    char compiled_filter[FLOWIE_SECURITY_PATTERN_MAX + 1u];
    if (!rule || rule->size < sizeof(*rule) || rule->abi_version != FLOWIE_SECURITY_ABI_V3 ||
        rule->match_kind != FLOWIE_SECURITY_MATCH_ADAPTER ||
        rule->resource_type != FLOWIE_SECURITY_RESOURCE_MQTT_TOPIC) {
      rc = SALTS_EPROTO;
      break;
    }
    rc = flowie_mqtt_security_filter_compile(
        rule->pattern, compiled_filter, sizeof(compiled_filter),
        &compiled->candidates[position].uses_placeholders);
    if (rc != SALTS_OK) break;
    compiled->candidates[position].pattern = rule->pattern;
    filter = (flowie_mqtt_span_t){(const uint8_t *)compiled_filter, strlen(compiled_filter)};
    if (!flowie_mqtt_topic_filter_validate(filter)) {
      rc = SALTS_EPROTO;
      break;
    }
    rc = flowie_topic_index_insert(&compiled->topics, filter, position);
  }
  if (rc != SALTS_OK) {
    flowie_topic_index_destroy(&compiled->topics);
    free(compiled->candidates);
    free(compiled);
    return rc;
  }
  *compiled_leaf_out = compiled;
  return SALTS_OK;
}

typedef struct flowie_mqtt_security_emit_context_s {
  const flowie_mqtt_security_leaf_t *leaf;
  const flowie_mqtt_security_context_t *security;
  flowie_mqtt_span_t resource;
  flowie_security_match_emit_fn emit;
  void *emit_ctx;
} flowie_mqtt_security_emit_context_t;

static int flowie_mqtt_security_emit_candidate(void *ctx, size_t candidate_position) {
  flowie_mqtt_security_emit_context_t *state = (flowie_mqtt_security_emit_context_t *)ctx;
  const flowie_mqtt_security_candidate_t *candidate;
  if (!state || !state->leaf || candidate_position >= state->leaf->candidate_count)
    return SALTS_EPROTO;
  candidate = &state->leaf->candidates[candidate_position];
  if (candidate->uses_placeholders &&
      !flowie_mqtt_security_placeholders_match(candidate->pattern, state->resource,
                                               state->security))
    return SALTS_OK;
  return state->emit(state->emit_ctx, candidate_position);
}

static int flowie_mqtt_security_evaluate_leaf(void *ctx, const void *compiled_leaf,
                                              const flowie_security_request_t *request,
                                              flowie_security_match_emit_fn emit,
                                              void *emit_ctx) {
  const flowie_mqtt_security_leaf_t *compiled = (const flowie_mqtt_security_leaf_t *)compiled_leaf;
  flowie_mqtt_span_t resource;
  flowie_mqtt_security_resource_kind_t kind;
  const flowie_mqtt_security_context_t *security_context;
  flowie_mqtt_security_emit_context_t emit_context;
  int validated;
  int rc;
  (void)ctx;
  if (!compiled || !request || request->size < sizeof(*request) || !emit || !request->resource)
    return SALTS_EINVAL;
  if (request->resource_type != FLOWIE_SECURITY_RESOURCE_MQTT_TOPIC) return SALTS_OK;
  rc = flowie_mqtt_security_resource(request, &resource, &kind, &validated, &security_context);
  if (rc != SALTS_OK) return rc;
  emit_context = (flowie_mqtt_security_emit_context_t){compiled, security_context, resource, emit,
                                                       emit_ctx};
  if (kind == FLOWIE_MQTT_SECURITY_TOPIC_FILTER) {
    if (!validated && !flowie_mqtt_topic_filter_validate(resource)) return SALTS_EPROTO;
    return flowie_topic_index_visit_validated_containing_filters(
        &compiled->topics, resource, flowie_mqtt_security_emit_candidate, &emit_context);
  }
  if (!validated && !flowie_mqtt_topic_name_validate(resource)) return SALTS_EPROTO;
  return flowie_topic_index_visit_validated_topic(
      &compiled->topics, resource, flowie_mqtt_security_emit_candidate, &emit_context);
}

static void flowie_mqtt_security_destroy_leaf(void *ctx, void *compiled_leaf) {
  flowie_mqtt_security_leaf_t *compiled = (flowie_mqtt_security_leaf_t *)compiled_leaf;
  (void)ctx;
  if (!compiled) return;
  flowie_topic_index_destroy(&compiled->topics);
  free(compiled->candidates);
  free(compiled);
}

int flowie_mqtt_security_matcher_init(flowie_security_matcher_t *out) {
  flowie_security_matcher_t matcher = FLOWIE_SECURITY_MATCHER_INIT;
  if (!out || out->size < sizeof(*out)) return SALTS_EINVAL;
  matcher.compile_leaf = flowie_mqtt_security_compile_leaf;
  matcher.evaluate_leaf = flowie_mqtt_security_evaluate_leaf;
  matcher.destroy_leaf = flowie_mqtt_security_destroy_leaf;
  *out = matcher;
  return SALTS_OK;
}
