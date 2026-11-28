#include "flowie_control_validation_internal.h"

#include <core/error.h>

#include <stdlib.h>
#include <string.h>

typedef struct flowie_control_acl_validation_workspace_s {
  flowie_control_acl_document_t document;
  char canonical[FLOWIE_CONTROL_ACL_DOCUMENT_MAX + 1u];
} flowie_control_acl_validation_workspace_t;

int flowie_control_text_valid(const char *value, size_t limit) {
  size_t length;
  if (!value || limit == 0u) return 0;
  length = strnlen(value, limit + 1u);
  if (length == 0u || length > limit) return 0;
  for (size_t index = 0u; index < length; ++index) {
    const unsigned char byte = (unsigned char)value[index];
    if (byte < 0x20u || byte == 0x7fu) return 0;
  }
  return 1;
}

static int flowie_control_acl_validate_topics(const flowie_control_acl_document_t *document,
                                             const char *domain) {
  const size_t size = strlen(domain);
  for (size_t i = 0; i < document->entry_count; ++i) {
    const char *topic = document->entries[i].topic;
    if (strncmp(topic, domain, size) != 0 || topic[size] != '/') return SALTS_EPROTO;
  }
  return SALTS_OK;
}

int flowie_control_acl_document_syntax_validate(const char *domain_id, const char *document_text,
                                                size_t document_size,
                                                flowie_control_acl_document_t *document_out) {
  flowie_control_acl_validation_workspace_t *workspace = NULL;
  size_t canonical_size = 0u;
  int rc;
  if (!flowie_control_text_valid(domain_id, FLOWIE_SECURITY_ID_MAX) || !document_text ||
      document_size == 0u || document_size > FLOWIE_CONTROL_ACL_DOCUMENT_MAX ||
      memchr(document_text, '\0', document_size))
    return SALTS_EINVAL;
  workspace = (flowie_control_acl_validation_workspace_t *)malloc(sizeof(*workspace));
  if (!workspace) return SALTS_ENOMEM;
  flowie_control_acl_document_init(&workspace->document);
  rc = flowie_control_acl_parse(document_text, document_size, &workspace->document);
  if (rc != SALTS_OK) goto done;
  rc = flowie_control_acl_validate_topics(&workspace->document, domain_id);
  if (rc != SALTS_OK) goto done;
  rc = flowie_control_acl_format(&workspace->document, workspace->canonical,
                                 sizeof(workspace->canonical), &canonical_size);
  if (rc != SALTS_OK || canonical_size != document_size ||
      memcmp(workspace->canonical, document_text, document_size) != 0) {
    rc = SALTS_EPROTO;
    goto done;
  }
  if (document_out) *document_out = workspace->document;
  rc = SALTS_OK;

done:
  free(workspace);
  return rc;
}

typedef struct flowie_control_acl_bundle_s {
  const char *domain;
  const char *text;
  size_t size;
  size_t offset;
  char canonical[FLOWIE_CONTROL_ACL_DOCUMENT_MAX + 1u];
  flowie_security_rule_t *rules;
  size_t capacity;
  size_t count;
} flowie_control_acl_bundle_t;

static int flowie_control_acl_bundle_append(const flowie_control_acl_document_t *document,
                                            void *context) {
  flowie_control_acl_bundle_t *bundle = context;
  int rc = flowie_control_acl_validate_topics(document, bundle->domain);
  if (rc != SALTS_OK) return rc;
  size_t size = 0u;
  rc = flowie_control_acl_format(document, bundle->canonical, sizeof(bundle->canonical), &size);
  if (rc != SALTS_OK) return rc;
  if (bundle->count) {
    if (bundle->offset == bundle->size || bundle->text[bundle->offset] != '\n') return SALTS_EPROTO;
    ++bundle->offset;
  }
  if (size > bundle->size - bundle->offset ||
      memcmp(bundle->text + bundle->offset, bundle->canonical, size)) return SALTS_EPROTO;
  bundle->offset += size;
  for (size_t i = 0; i < bundle->count; ++i) {
    const flowie_security_rule_t *rule = &bundle->rules[i];
    if (rule->action_mask == FLOWIE_SECURITY_ACTION_CONNECT && rule->subject_kind == document->subject_kind &&
        strcmp(rule->subject, document->subject) == 0) return SALTS_EPROTO;
  }
  size_t required = bundle->count + 1u;
  for (size_t i = 0; i < document->entry_count; ++i) {
    const size_t alternatives = document->entries[i].alternative_count;
    required += alternatives > 1u ? alternatives : 1u;
  }
  if (required > FLOWIE_SECURITY_MAX_RULES) return SALTS_ENOSPC;
  if (required > bundle->capacity) {
    size_t capacity = bundle->capacity ? bundle->capacity * 2u : 8u;
    if (capacity < required) capacity = required;
    if (capacity > FLOWIE_SECURITY_MAX_RULES) capacity = FLOWIE_SECURITY_MAX_RULES;
    flowie_security_rule_t *rules = realloc(bundle->rules, capacity * sizeof(*rules));
    if (!rules) return SALTS_ENOMEM;
    bundle->rules = rules;
    bundle->capacity = capacity;
  }
  size_t added;
  rc = flowie_control_acl_compile(document, bundle->domain, bundle->rules + bundle->count,
                                  bundle->capacity - bundle->count, &added);
  if (rc == SALTS_OK) bundle->count += added;
  return rc;
}

int flowie_control_acl_bundle_compile(const char *domain_id, const char *text, size_t size,
    flowie_security_rule_t **out, size_t *count) {
  if (!out || !count) return SALTS_EINVAL;
  *out = NULL;
  *count = 0u;
  if (!flowie_control_text_valid(domain_id, FLOWIE_SECURITY_ID_MAX) || !text ||
      size == 0u || size > FLOWIE_CONTROL_ACL_DOCUMENT_MAX) return SALTS_EINVAL;
  flowie_control_acl_bundle_t *bundle = calloc(1u, sizeof(*bundle));
  if (!bundle) return SALTS_ENOMEM;
  bundle->text = text;
  bundle->size = size;
  bundle->domain = domain_id;
  int rc = flowie_control_acl_parse_each(text, size, flowie_control_acl_bundle_append, bundle);
  if (rc == SALTS_OK && bundle->offset != size) rc = SALTS_EPROTO;
  if (rc == SALTS_OK) {
    *out = bundle->rules;
    *count = bundle->count;
  } else {
    free(bundle->rules);
  }
  free(bundle);
  return rc;
}
