#include "flowie_control_validation_internal.h"

#include "turbo_error.h"

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

int flowie_control_acl_document_syntax_validate(const char *domain_id, const char *document_text,
                                                size_t document_size,
                                                flowie_control_acl_document_t *document_out) {
  flowie_control_acl_validation_workspace_t *workspace = NULL;
  size_t canonical_size = 0u;
  size_t domain_size;
  int rc;
  if (!flowie_control_text_valid(domain_id, FLOWIE_SECURITY_ID_MAX) || !document_text ||
      document_size == 0u || document_size > FLOWIE_CONTROL_ACL_DOCUMENT_MAX ||
      memchr(document_text, '\0', document_size))
    return TURBO_EINVAL;
  workspace = (flowie_control_acl_validation_workspace_t *)malloc(sizeof(*workspace));
  if (!workspace) return TURBO_ENOMEM;
  flowie_control_acl_document_init(&workspace->document);
  rc = flowie_control_acl_parse(document_text, document_size, &workspace->document);
  if (rc != TURBO_OK) goto done;
  domain_size = strlen(domain_id);
  for (size_t index = 0u; index < workspace->document.entry_count; ++index) {
    const char *topic = workspace->document.entries[index].topic;
    if (strncmp(topic, domain_id, domain_size) != 0 || topic[domain_size] != '/') {
      rc = TURBO_EPROTO;
      goto done;
    }
  }
  rc = flowie_control_acl_format(&workspace->document, workspace->canonical,
                                 sizeof(workspace->canonical), &canonical_size);
  if (rc != TURBO_OK || canonical_size != document_size ||
      memcmp(workspace->canonical, document_text, document_size) != 0) {
    rc = TURBO_EPROTO;
    goto done;
  }
  if (document_out) *document_out = workspace->document;
  rc = TURBO_OK;

done:
  free(workspace);
  return rc;
}
