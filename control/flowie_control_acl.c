#include "flowie_control_acl_internal.h"
#include "flowie_control_acl_grammar_gen.h"

#include "salts_error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *FlowieControlAclParseAlloc(void *(*malloc_proc)(size_t));
void FlowieControlAclParse(void *parser, int token_id, flowie_control_acl_token_t token,
                           flowie_control_acl_parse_ctx_t *ctx);
void FlowieControlAclParseFree(void *parser, void (*free_proc)(void *));

static void flowie_control_acl_set_error(flowie_control_acl_parse_ctx_t *ctx, int status) {
  if (ctx && ctx->status == SALTS_OK) ctx->status = status;
}

void flowie_control_acl_parse_fail(flowie_control_acl_parse_ctx_t *ctx) {
  flowie_control_acl_set_error(ctx, SALTS_EPROTO);
}

static int flowie_control_acl_copy_token(char *output, size_t capacity,
                                         flowie_control_acl_token_t token) {
  if (!output || capacity == 0u || !token.value || token.length == 0u || token.length >= capacity)
    return SALTS_EPROTO;
  memcpy(output, token.value, token.length);
  output[token.length] = '\0';
  return SALTS_OK;
}

void flowie_control_acl_parse_accept(flowie_control_acl_parse_ctx_t *ctx,
                                     flowie_security_subject_kind_t subject_kind,
                                     flowie_control_acl_token_t subject,
                                     flowie_security_effect_t connection_effect) {
  if (!ctx || ctx->status != SALTS_OK) return;
  if (subject_kind != FLOWIE_SECURITY_SUBJECT_PRINCIPAL &&
      subject_kind != FLOWIE_SECURITY_SUBJECT_ROLE &&
      subject_kind != FLOWIE_SECURITY_SUBJECT_GROUP) {
    flowie_control_acl_set_error(ctx, SALTS_EPROTO);
    return;
  }
  if (connection_effect == FLOWIE_SECURITY_DENY && ctx->document.entry_count != 0u) {
    flowie_control_acl_set_error(ctx, SALTS_EPROTO);
    return;
  }
  if (flowie_control_acl_copy_token(ctx->document.subject, sizeof(ctx->document.subject),
                                    subject) != SALTS_OK) {
    flowie_control_acl_set_error(ctx, SALTS_EPROTO);
    return;
  }
  ctx->document.subject_kind = subject_kind;
  ctx->document.connection_effect = connection_effect;
  ctx->accepted = 1;
}

static int flowie_control_acl_topic_static_char(unsigned char value, int allow_dollar) {
  return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
         (value >= '0' && value <= '9') || value == '_' || value == '.' || value == ':' ||
         value == '@' || value == '~' || value == '-' || (allow_dollar && value == '$');
}

static int flowie_control_acl_topic_static(const char *value, size_t length,
                                           int allow_dollar) {
  if (!value || length == 0u) return 0;
  for (size_t index = 0u; index < length; ++index) {
    if (!flowie_control_acl_topic_static_char((unsigned char)value[index], allow_dollar))
      return 0;
  }
  return 1;
}

static size_t flowie_control_acl_topic_alternatives(flowie_control_acl_parse_ctx_t *ctx,
                                                    const char *value, size_t length) {
  const char *items[FLOWIE_CONTROL_ACL_MAX_ALTERNATIVES];
  size_t item_lengths[FLOWIE_CONTROL_ACL_MAX_ALTERNATIVES];
  const char *cursor;
  const char *close;
  size_t count = 0u;
  if (!ctx || ctx->status != SALTS_OK || !value || length < sizeof("{a,b}") - 1u ||
      value[0] != '{' || value[length - 1u] != '}') {
    flowie_control_acl_set_error(ctx, SALTS_EPROTO);
    return 0u;
  }
  cursor = value + 1u;
  close = value + length - 1u;
  while (cursor < close) {
    const char *comma = memchr(cursor, ',', (size_t)(close - cursor));
    const char *end = comma ? comma : close;
    size_t item_length = (size_t)(end - cursor);
    if (!flowie_control_acl_topic_static(cursor, item_length, 1)) {
      flowie_control_acl_set_error(ctx, SALTS_EPROTO);
      return 0u;
    }
    if (count >= FLOWIE_CONTROL_ACL_MAX_ALTERNATIVES) {
      flowie_control_acl_set_error(ctx, SALTS_ENOSPC);
      return 0u;
    }
    for (size_t index = 0u; index < count; ++index) {
      if (item_lengths[index] == item_length &&
          memcmp(items[index], cursor, item_length) == 0) {
        flowie_control_acl_set_error(ctx, SALTS_EPROTO);
        return 0u;
      }
    }
    items[count] = cursor;
    item_lengths[count] = item_length;
    ++count;
    if (!comma) break;
    cursor = comma + 1u;
    if (cursor == close) {
      flowie_control_acl_set_error(ctx, SALTS_EPROTO);
      return 0u;
    }
  }
  if (count < 2u) {
    flowie_control_acl_set_error(ctx, SALTS_EPROTO);
    return 0u;
  }
  return count;
}

flowie_control_acl_topic_parse_t
flowie_control_acl_topic_parse(flowie_control_acl_parse_ctx_t *ctx,
                               flowie_control_acl_token_t pattern) {
  flowie_control_acl_topic_parse_t topic;
  const char *cursor;
  const char *end;
  size_t level = 0u;
  memset(&topic, 0, sizeof(topic));
  if (!ctx || ctx->status != SALTS_OK || !pattern.value || pattern.length == 0u) {
    flowie_control_acl_set_error(ctx, SALTS_EPROTO);
    return topic;
  }
  if (pattern.length > FLOWIE_SECURITY_PATTERN_MAX) {
    flowie_control_acl_set_error(ctx, SALTS_ENOSPC);
    return topic;
  }
  cursor = pattern.value;
  end = pattern.value + pattern.length;
  while (cursor < end) {
    const char *slash = memchr(cursor, '/', (size_t)(end - cursor));
    const char *segment_end = slash ? slash : end;
    size_t segment_length = (size_t)(segment_end - cursor);
    int final_segment = segment_end == end;
    if (segment_length == 0u) {
      flowie_control_acl_set_error(ctx, SALTS_EPROTO);
      return topic;
    }
    if (level == 0u) {
      if (!flowie_control_acl_topic_static(cursor, segment_length, 0)) {
        flowie_control_acl_set_error(ctx, SALTS_EPROTO);
        return topic;
      }
    } else if (segment_length == 1u && cursor[0] == '#') {
      if (!final_segment) {
        flowie_control_acl_set_error(ctx, SALTS_EPROTO);
        return topic;
      }
    } else if (segment_length == 2u && memcmp(cursor, "%u", 2u) == 0) {
      topic.uses_username = 1;
    } else if (segment_length == 2u && memcmp(cursor, "%c", 2u) == 0) {
      topic.uses_client_id = 1;
    } else if (segment_length > 1u && cursor[0] == '%' &&
               flowie_control_acl_topic_static(cursor + 1u, segment_length - 1u, 0)) {
      topic.uses_client_id = 1;
    } else if (cursor[0] == '{') {
      if (!final_segment) {
        flowie_control_acl_set_error(ctx, SALTS_EPROTO);
        return topic;
      }
      topic.alternative_count =
          flowie_control_acl_topic_alternatives(ctx, cursor, segment_length);
      if (ctx->status != SALTS_OK) return topic;
    } else if (!(segment_length == 1u && cursor[0] == '+') &&
               !flowie_control_acl_topic_static(cursor, segment_length, 1)) {
      flowie_control_acl_set_error(ctx, SALTS_EPROTO);
      return topic;
    }
    ++level;
    if (!slash) break;
    cursor = slash + 1u;
    if (cursor == end) {
      flowie_control_acl_set_error(ctx, SALTS_EPROTO);
      return topic;
    }
  }
  if (level < 2u) {
    flowie_control_acl_set_error(ctx, SALTS_EPROTO);
    return topic;
  }
  topic.complete = pattern;
  if (topic.alternative_count == 0u) topic.alternative_count = 1u;
  return topic;
}

void flowie_control_acl_entry_add(flowie_control_acl_parse_ctx_t *ctx,
                                  flowie_security_effect_t effect, uint32_t action_mask,
                                  flowie_control_acl_topic_parse_t topic) {
  flowie_control_acl_entry_t *entry;
  if (!ctx || ctx->status != SALTS_OK) return;
  if (ctx->document.entry_count >= FLOWIE_CONTROL_ACL_MAX_ENTRIES) {
    flowie_control_acl_set_error(ctx, SALTS_ENOSPC);
    return;
  }
  if (!topic.complete.value || topic.complete.length == 0u ||
      topic.complete.length > FLOWIE_SECURITY_PATTERN_MAX || topic.alternative_count == 0u ||
      topic.alternative_count > FLOWIE_CONTROL_ACL_MAX_ALTERNATIVES) {
    flowie_control_acl_set_error(ctx, SALTS_EPROTO);
    return;
  }
  entry = &ctx->document.entries[ctx->document.entry_count];
  memset(entry, 0, sizeof(*entry));
  entry->effect = effect;
  entry->action_mask = action_mask;
  memcpy(entry->topic, topic.complete.value, topic.complete.length);
  entry->topic[topic.complete.length] = '\0';
  entry->alternative_count = (uint8_t)topic.alternative_count;
  entry->uses_username = topic.uses_username;
  entry->uses_client_id = topic.uses_client_id;
  ++ctx->document.entry_count;
}

void flowie_control_acl_document_init(flowie_control_acl_document_t *document) {
  if (!document) return;
  memset(document, 0, sizeof(*document));
  document->size = sizeof(*document);
  document->subject_kind = FLOWIE_SECURITY_SUBJECT_ANY;
  document->connection_effect = FLOWIE_SECURITY_DENY;
}

int flowie_control_acl_parse(const char *text, size_t text_size,
                             flowie_control_acl_document_t *out) {
  flowie_control_acl_parse_ctx_t *ctx = NULL;
  flowie_control_acl_lexer_t lexer;
  flowie_control_acl_token_t token;
  void *parser = NULL;
  int token_id = FLOWIE_CONTROL_ACL_LEX_END;
  int rc = SALTS_OK;
  if (out && out->size >= sizeof(*out)) flowie_control_acl_document_init(out);
  if (!text || text_size == 0u || text_size > FLOWIE_CONTROL_ACL_DOCUMENT_MAX ||
      memchr(text, '\0', text_size) || !out || out->size < sizeof(*out))
    return SALTS_EINVAL;
  ctx = (flowie_control_acl_parse_ctx_t *)calloc(1u, sizeof(*ctx));
  if (!ctx) return SALTS_ENOMEM;
  flowie_control_acl_document_init(&ctx->document);
  parser = FlowieControlAclParseAlloc(malloc);
  if (!parser) {
    free(ctx);
    return SALTS_ENOMEM;
  }
  flowie_control_acl_lexer_init(&lexer, text, text_size);
  while ((token_id = flowie_control_acl_lexer_next(&lexer, &token)) > 0) {
    FlowieControlAclParse(parser, token_id, token, ctx);
    if (ctx->status != SALTS_OK) break;
  }
  if (token_id < 0 && ctx->status == SALTS_OK) ctx->status = SALTS_EPROTO;
  if (ctx->status == SALTS_OK) {
    memset(&token, 0, sizeof(token));
    token.line = lexer.line;
    token.column = lexer.column;
    FlowieControlAclParse(parser, 0, token, ctx);
  }
  FlowieControlAclParseFree(parser, free);
  rc = ctx->status != SALTS_OK || !ctx->accepted ? (ctx->status ? ctx->status : SALTS_EPROTO)
                                                  : SALTS_OK;
  if (rc == SALTS_OK) *out = ctx->document;
  free(ctx);
  return rc;
}

static const char *flowie_control_acl_access_name(uint32_t action_mask) {
  if (action_mask == FLOWIE_SECURITY_ACTION_SUBSCRIBE) return "read";
  if (action_mask == FLOWIE_SECURITY_ACTION_PUBLISH) return "write";
  if (action_mask ==
      (FLOWIE_SECURITY_ACTION_PUBLISH | FLOWIE_SECURITY_ACTION_SUBSCRIBE))
    return "readwrite";
  return NULL;
}

static const char *
flowie_control_acl_subject_kind_name(flowie_security_subject_kind_t subject_kind) {
  switch (subject_kind) {
  case FLOWIE_SECURITY_SUBJECT_PRINCIPAL:
    return "user";
  case FLOWIE_SECURITY_SUBJECT_ROLE:
    return "role";
  case FLOWIE_SECURITY_SUBJECT_GROUP:
    return "group";
  default:
    return NULL;
  }
}

static int flowie_control_acl_append(char *output, size_t capacity, size_t *offset,
                                     const char *text, size_t text_size) {
  if (!output || !offset || !text || *offset > capacity || text_size > capacity - *offset)
    return SALTS_ENOSPC;
  memcpy(output + *offset, text, text_size);
  *offset += text_size;
  return SALTS_OK;
}

int flowie_control_acl_format(const flowie_control_acl_document_t *document, char *text_out,
                              size_t text_capacity, size_t *text_size_out) {
  size_t offset = 0u;
  const char *subject_kind;
  int rc = SALTS_OK;
  if (text_size_out) *text_size_out = 0u;
  if (!document || document->size < sizeof(*document) ||
      !(subject_kind = flowie_control_acl_subject_kind_name(document->subject_kind)) ||
      !document->subject[0] ||
      document->entry_count > FLOWIE_CONTROL_ACL_MAX_ENTRIES || !text_out ||
      text_capacity == 0u || !text_size_out)
    return SALTS_EINVAL;
#define FLOWIE_ACL_APPEND_LITERAL(value)                                                           \
  flowie_control_acl_append(text_out, text_capacity - 1u, &offset, value, sizeof(value) - 1u)
  rc = flowie_control_acl_append(text_out, text_capacity - 1u, &offset, subject_kind,
                                 strlen(subject_kind));
  if (rc == SALTS_OK) rc = FLOWIE_ACL_APPEND_LITERAL(" ");
  if (rc == SALTS_OK)
    rc = flowie_control_acl_append(text_out, text_capacity - 1u, &offset, document->subject,
                                   strlen(document->subject));
  if (rc == SALTS_OK)
    rc = document->connection_effect == FLOWIE_SECURITY_ALLOW
             ? FLOWIE_ACL_APPEND_LITERAL(" allow")
             : FLOWIE_ACL_APPEND_LITERAL(" deny");
  if (rc == SALTS_OK && document->entry_count != 0u) rc = FLOWIE_ACL_APPEND_LITERAL(" {\n");
  for (size_t index = 0u; rc == SALTS_OK && index < document->entry_count; ++index) {
    const flowie_control_acl_entry_t *entry = &document->entries[index];
    const char *access = flowie_control_acl_access_name(entry->action_mask);
    if (!access || !entry->topic[0] ||
        (entry->effect != FLOWIE_SECURITY_ALLOW &&
         entry->effect != FLOWIE_SECURITY_DENY)) {
      rc = SALTS_EINVAL;
      break;
    }
    rc = FLOWIE_ACL_APPEND_LITERAL("  ");
    if (rc == SALTS_OK && entry->effect == FLOWIE_SECURITY_DENY)
      rc = FLOWIE_ACL_APPEND_LITERAL("deny ");
    if (rc == SALTS_OK)
      rc = flowie_control_acl_append(text_out, text_capacity - 1u, &offset, access,
                                     strlen(access));
    if (rc == SALTS_OK) rc = FLOWIE_ACL_APPEND_LITERAL(" topic ");
    if (rc == SALTS_OK)
      rc = flowie_control_acl_append(text_out, text_capacity - 1u, &offset, entry->topic,
                                     strlen(entry->topic));
    if (rc == SALTS_OK) rc = FLOWIE_ACL_APPEND_LITERAL("\n");
  }
  if (rc == SALTS_OK && document->entry_count != 0u) rc = FLOWIE_ACL_APPEND_LITERAL("}");
  if (rc == SALTS_OK) {
    text_out[offset] = '\0';
    *text_size_out = offset;
  }
#undef FLOWIE_ACL_APPEND_LITERAL
  return rc;
}

static int flowie_control_acl_rule_base(const flowie_control_acl_document_t *document,
                                        const char *domain_id,
                                        flowie_security_rule_t *rule) {
  size_t subject_size;
  size_t domain_size;
  if (!document || !domain_id || !rule) return SALTS_EINVAL;
  subject_size = strnlen(document->subject, sizeof(document->subject));
  domain_size = strnlen(domain_id, FLOWIE_SECURITY_ID_MAX + 1u);
  if (!flowie_control_acl_subject_kind_name(document->subject_kind) || subject_size == 0u ||
      subject_size >= sizeof(document->subject) || domain_size == 0u ||
      domain_size > FLOWIE_SECURITY_ID_MAX)
    return SALTS_EINVAL;
  *rule = (flowie_security_rule_t)FLOWIE_SECURITY_RULE_INIT;
  rule->subject_kind = document->subject_kind;
  memcpy(rule->subject, document->subject, subject_size + 1u);
  memcpy(rule->domain_id, domain_id, domain_size + 1u);
  return SALTS_OK;
}

static int flowie_control_acl_expand_topic(const flowie_control_acl_document_t *document,
                                           const flowie_control_acl_entry_t *entry,
                                           const char *alternative, size_t alternative_size,
                                           char output[FLOWIE_SECURITY_PATTERN_MAX + 1u]) {
  const char *topic;
  size_t topic_size;
  size_t read = 0u;
  size_t written = 0u;
  if (!document || !entry || !output) return SALTS_EINVAL;
  topic = entry->topic;
  topic_size = strnlen(topic, sizeof(entry->topic));
  if (topic_size == 0u || topic_size >= sizeof(entry->topic)) return SALTS_EPROTO;
  while (read < topic_size) {
    const char *replacement = NULL;
    size_t replacement_size = 0u;
    if (topic[read] == '{') {
      const char *close = strchr(topic + read + 1u, '}');
      if (!alternative || !close || close[1] != '\0') return SALTS_EPROTO;
      replacement = alternative;
      replacement_size = alternative_size;
      read = topic_size;
    } else {
      replacement = topic + read;
      replacement_size = 1u;
      ++read;
    }
    if (replacement_size > FLOWIE_SECURITY_PATTERN_MAX - written) return SALTS_ENOSPC;
    memcpy(output + written, replacement, replacement_size);
    written += replacement_size;
  }
  output[written] = '\0';
  return SALTS_OK;
}

static int flowie_control_acl_rule_add(const flowie_control_acl_document_t *document,
                                       const char *domain_id,
                                       const flowie_control_acl_entry_t *entry,
                                       const char *alternative, size_t alternative_size,
                                       flowie_security_rule_t *rules, size_t rule_capacity,
                                       size_t *count) {
  flowie_security_rule_t rule = FLOWIE_SECURITY_RULE_INIT;
  int rc;
  if (!entry || !rules || !count) return SALTS_EINVAL;
  if (*count >= rule_capacity) return SALTS_ENOSPC;
  rc = flowie_control_acl_rule_base(document, domain_id, &rule);
  if (rc != SALTS_OK) return rc;
  rule.effect = entry->effect;
  rule.action_mask = entry->action_mask;
  rule.resource_type = FLOWIE_SECURITY_RESOURCE_MQTT_TOPIC;
  rule.match_kind = FLOWIE_SECURITY_MATCH_ADAPTER;
  rc = flowie_control_acl_expand_topic(document, entry, alternative, alternative_size,
                                       rule.pattern);
  if (rc != SALTS_OK) return rc;
  rules[(*count)++] = rule;
  return SALTS_OK;
}

int flowie_control_acl_compile(const flowie_control_acl_document_t *document,
                               const char *domain_id, flowie_security_rule_t *rules,
                               size_t rule_capacity, size_t *rule_count_out) {
  size_t count = 0u;
  int rc;
  if (rule_count_out) *rule_count_out = 0u;
  if (!document || document->size < sizeof(*document) || !domain_id || !rules ||
      rule_capacity == 0u || !rule_count_out ||
      document->entry_count > FLOWIE_CONTROL_ACL_MAX_ENTRIES)
    return SALTS_EINVAL;
  rc = flowie_control_acl_rule_base(document, domain_id, &rules[count]);
  if (rc != SALTS_OK) return rc;
  rules[count].effect = document->connection_effect;
  rules[count].action_mask = FLOWIE_SECURITY_ACTION_CONNECT;
  rules[count].resource_type = FLOWIE_SECURITY_RESOURCE_GENERIC;
  rules[count].match_kind = FLOWIE_SECURITY_MATCH_PREFIX;
  rules[count].pattern[0] = '\0';
  ++count;
  for (size_t index = 0u; rc == SALTS_OK && index < document->entry_count; ++index) {
    const flowie_control_acl_entry_t *entry = &document->entries[index];
    if (entry->alternative_count <= 1u) {
      rc = flowie_control_acl_rule_add(document, domain_id, entry, NULL, 0u, rules,
                                       rule_capacity, &count);
      continue;
    }
    {
      const char *open = strrchr(entry->topic, '{');
      const char *close = open ? strchr(open + 1u, '}') : NULL;
      const char *cursor = open ? open + 1u : NULL;
      size_t expanded = 0u;
      if (!open || !close || close[1] != '\0') {
        rc = SALTS_EPROTO;
        break;
      }
      while (cursor < close) {
        const char *comma = memchr(cursor, ',', (size_t)(close - cursor));
        const char *end = comma ? comma : close;
        if (end == cursor) {
          rc = SALTS_EPROTO;
          break;
        }
        rc = flowie_control_acl_rule_add(document, domain_id, entry, cursor,
                                         (size_t)(end - cursor), rules, rule_capacity, &count);
        if (rc != SALTS_OK) break;
        ++expanded;
        cursor = comma ? comma + 1u : close;
      }
      if (rc == SALTS_OK && expanded != entry->alternative_count) rc = SALTS_EPROTO;
    }
  }
  if (rc != SALTS_OK) return rc;
  *rule_count_out = count;
  return SALTS_OK;
}
