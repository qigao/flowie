#ifndef FLOWIE_CONTROL_ACL_INTERNAL_H
#define FLOWIE_CONTROL_ACL_INTERNAL_H

#include "flowie_security.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FLOWIE_CONTROL_ACL_DOCUMENT_MAX 16383u
#define FLOWIE_CONTROL_ACL_MAX_ENTRIES 64u
#define FLOWIE_CONTROL_ACL_MAX_ALTERNATIVES 16u

typedef enum flowie_control_acl_token_id_e {
  FLOWIE_CONTROL_ACL_LEX_ERROR = -1,
  FLOWIE_CONTROL_ACL_LEX_END = 0
} flowie_control_acl_token_id_t;

typedef struct flowie_control_acl_token_s {
  const char *value;
  size_t length;
  uint32_t line;
  uint32_t column;
} flowie_control_acl_token_t;

typedef struct flowie_control_acl_topic_parse_s {
  flowie_control_acl_token_t complete;
  size_t alternative_count;
  int uses_username;
  int uses_client_id;
} flowie_control_acl_topic_parse_t;

typedef struct flowie_control_acl_entry_s {
  flowie_security_effect_t effect;
  uint32_t action_mask;
  char topic[FLOWIE_SECURITY_PATTERN_MAX + 1u];
  uint8_t alternative_count;
  int uses_username;
  int uses_client_id;
} flowie_control_acl_entry_t;

typedef struct flowie_control_acl_document_s {
  size_t size;
  flowie_security_subject_kind_t subject_kind;
  char subject[FLOWIE_SECURITY_ID_MAX + 1u];
  flowie_security_effect_t connection_effect;
  flowie_control_acl_entry_t entries[FLOWIE_CONTROL_ACL_MAX_ENTRIES];
  size_t entry_count;
} flowie_control_acl_document_t;

#define FLOWIE_CONTROL_ACL_DOCUMENT_INIT                                                           \
  {sizeof(flowie_control_acl_document_t), FLOWIE_SECURITY_SUBJECT_ANY, "",                       \
   FLOWIE_SECURITY_DENY, {{0}}, 0u}

typedef enum flowie_control_acl_lexer_mode_e {
  FLOWIE_CONTROL_ACL_LEX_DEFAULT = 0,
  FLOWIE_CONTROL_ACL_LEX_TOPIC
} flowie_control_acl_lexer_mode_t;

typedef struct flowie_control_acl_lexer_s {
  const char *cursor;
  const char *limit;
  uint32_t line;
  uint32_t column;
  flowie_control_acl_lexer_mode_t mode;
} flowie_control_acl_lexer_t;

typedef struct flowie_control_acl_parse_ctx_s {
  flowie_control_acl_document_t document;
  int status;
  int accepted;
} flowie_control_acl_parse_ctx_t;

void flowie_control_acl_lexer_init(flowie_control_acl_lexer_t *lexer, const char *text,
                                   size_t text_size);
int flowie_control_acl_lexer_next(flowie_control_acl_lexer_t *lexer,
                                  flowie_control_acl_token_t *token);
void flowie_control_acl_parse_fail(flowie_control_acl_parse_ctx_t *ctx);
void flowie_control_acl_parse_accept(flowie_control_acl_parse_ctx_t *ctx,
                                     flowie_security_subject_kind_t subject_kind,
                                     flowie_control_acl_token_t subject,
                                     flowie_security_effect_t connection_effect);
flowie_control_acl_topic_parse_t
flowie_control_acl_topic_parse(flowie_control_acl_parse_ctx_t *ctx,
                               flowie_control_acl_token_t pattern);
void flowie_control_acl_entry_add(flowie_control_acl_parse_ctx_t *ctx,
                                  flowie_security_effect_t effect, uint32_t action_mask,
                                  flowie_control_acl_topic_parse_t topic);

int flowie_control_acl_parse(const char *text, size_t text_size,
                             flowie_control_acl_document_t *out);
int flowie_control_acl_format(const flowie_control_acl_document_t *document, char *text_out,
                              size_t text_capacity, size_t *text_size_out);
int flowie_control_acl_compile(const flowie_control_acl_document_t *document,
                               const char *domain_id, flowie_security_rule_t *rules,
                               size_t rule_capacity, size_t *rule_count_out);

#ifdef __cplusplus
}
#endif

#endif
