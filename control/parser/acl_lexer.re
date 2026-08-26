// re2c $INPUT -o $OUTPUT
#include "flowie_control_acl_internal.h"
#include "flowie_control_acl_grammar_gen.h"

#include <string.h>

static int flowie_control_acl_keyword(const char *value, size_t length) {
#define FLOWIE_ACL_KEYWORD(text, token)                                                            \
  if (length == sizeof(text) - 1u && memcmp(value, text, sizeof(text) - 1u) == 0) return token
  FLOWIE_ACL_KEYWORD("user", FLOWIE_CONTROL_ACL_TOKEN_USER);
  FLOWIE_ACL_KEYWORD("role", FLOWIE_CONTROL_ACL_TOKEN_ROLE);
  FLOWIE_ACL_KEYWORD("group", FLOWIE_CONTROL_ACL_TOKEN_GROUP);
  FLOWIE_ACL_KEYWORD("allow", FLOWIE_CONTROL_ACL_TOKEN_ALLOW);
  FLOWIE_ACL_KEYWORD("deny", FLOWIE_CONTROL_ACL_TOKEN_DENY);
  FLOWIE_ACL_KEYWORD("read", FLOWIE_CONTROL_ACL_TOKEN_READ);
  FLOWIE_ACL_KEYWORD("write", FLOWIE_CONTROL_ACL_TOKEN_WRITE);
  FLOWIE_ACL_KEYWORD("readwrite", FLOWIE_CONTROL_ACL_TOKEN_READWRITE);
  FLOWIE_ACL_KEYWORD("topic", FLOWIE_CONTROL_ACL_TOKEN_TOPIC);
#undef FLOWIE_ACL_KEYWORD
  return FLOWIE_CONTROL_ACL_TOKEN_IDENT;
}

void flowie_control_acl_lexer_init(flowie_control_acl_lexer_t *lexer, const char *text,
                                   size_t text_size) {
  if (!lexer) return;
  memset(lexer, 0, sizeof(*lexer));
  lexer->cursor = text;
  lexer->limit = text ? text + text_size : text;
  lexer->line = 1u;
  lexer->column = 1u;
}

int flowie_control_acl_lexer_next(flowie_control_acl_lexer_t *lexer,
                                  flowie_control_acl_token_t *token) {
  const char *YYCURSOR;
  const char *YYLIMIT;
  const char *YYMARKER;
  const char *start;
  if (!lexer || !token || !lexer->cursor || !lexer->limit) return FLOWIE_CONTROL_ACL_LEX_ERROR;
  memset(token, 0, sizeof(*token));
  YYCURSOR = lexer->cursor;
  YYLIMIT = lexer->limit;

default_lex:
  start = YYCURSOR;
  token->value = start;
  token->line = lexer->line;
  token->column = lexer->column;
  if (lexer->mode == FLOWIE_CONTROL_ACL_LEX_TOPIC) goto topic_lex;
  /*!re2c
    re2c:define:YYCTYPE = "unsigned char";
    re2c:yyfill:enable = 0;
    re2c:eof = 0;
    ident = [A-Za-z0-9_.:@~-]+;
    hspace = [ \t]+;
    newline = "\r\n" | "\n" | "\r";
    $ { lexer->cursor = YYCURSOR; return FLOWIE_CONTROL_ACL_LEX_END; }
    hspace { lexer->column += (uint32_t)(YYCURSOR - start); goto default_lex; }
    newline { lexer->line++; lexer->column = 1u; goto default_lex; }
    "{" { token->length = 1u; lexer->cursor = YYCURSOR; lexer->column++; return FLOWIE_CONTROL_ACL_TOKEN_LBRACE; }
    "}" { token->length = 1u; lexer->cursor = YYCURSOR; lexer->column++; return FLOWIE_CONTROL_ACL_TOKEN_RBRACE; }
    ident {
      int id;
      token->length = (size_t)(YYCURSOR - start);
      lexer->cursor = YYCURSOR;
      lexer->column += (uint32_t)token->length;
      id = flowie_control_acl_keyword(token->value, token->length);
      if (id == FLOWIE_CONTROL_ACL_TOKEN_TOPIC) {
        lexer->mode = FLOWIE_CONTROL_ACL_LEX_TOPIC;
      }
      return id;
    }
    * { token->length = 1u; lexer->cursor = YYCURSOR; lexer->column++; return FLOWIE_CONTROL_ACL_LEX_ERROR; }
  */

topic_lex:
  start = YYCURSOR;
  token->value = start;
  token->line = lexer->line;
  token->column = lexer->column;
  /*!re2c
    re2c:define:YYCTYPE = "unsigned char";
    re2c:yyfill:enable = 0;
    re2c:eof = 0;
    topic_static = [A-Za-z0-9_.:@~$-]+;
    topic_atom = topic_static | "%u" | "%c" | "+" | "#";
    topic_alternatives = "{" topic_static ("," topic_static)+ "}";
    topic_pattern = topic_atom ("/" (topic_atom | topic_alternatives))*;
    topic_hspace = [ \t]+;
    topic_newline = "\r\n" | "\n" | "\r";
    $ {
      lexer->cursor = YYCURSOR;
      lexer->mode = FLOWIE_CONTROL_ACL_LEX_DEFAULT;
      return FLOWIE_CONTROL_ACL_LEX_ERROR;
    }
    topic_hspace {
      lexer->column += (uint32_t)(YYCURSOR - start);
      goto default_lex;
    }
    topic_newline {
      lexer->line++;
      lexer->column = 1u;
      goto default_lex;
    }
    topic_pattern {
      token->length = (size_t)(YYCURSOR - start);
      lexer->cursor = YYCURSOR;
      lexer->column += (uint32_t)token->length;
      lexer->mode = FLOWIE_CONTROL_ACL_LEX_DEFAULT;
      return FLOWIE_CONTROL_ACL_TOKEN_TOPIC_PATTERN;
    }
    * { token->length = 1u; lexer->cursor = YYCURSOR; lexer->column++; return FLOWIE_CONTROL_ACL_LEX_ERROR; }
  */
}
