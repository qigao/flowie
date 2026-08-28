%name FlowieControlAclParse
%token_prefix FLOWIE_CONTROL_ACL_TOKEN_
%token_type {flowie_control_acl_token_t}
%default_type {flowie_control_acl_token_t}
%stack_size 128

%extra_argument {flowie_control_acl_parse_ctx_t *ctx}

%include {
#include "flowie_control_acl_internal.h"
}

%token USER ROLE GROUP ALLOW DENY READ WRITE READWRITE TOPIC IDENT TOPIC_PATTERN.
%token LBRACE RBRACE.

%type subject_kind {flowie_security_subject_kind_t}
%type connection_effect {flowie_security_effect_t}
%type entry_effect {flowie_security_effect_t}
%type access_mode {uint32_t}
%type topic_pattern {flowie_control_acl_topic_parse_t}

%start_symbol document

document ::= subject_kind(K) IDENT(S) connection_effect(E) optional_entries. {
  flowie_control_acl_parse_accept(ctx, K, S, E);
}

subject_kind(K) ::= USER. { K = FLOWIE_SECURITY_SUBJECT_PRINCIPAL; }
subject_kind(K) ::= ROLE. { K = FLOWIE_SECURITY_SUBJECT_ROLE; }
subject_kind(K) ::= GROUP. { K = FLOWIE_SECURITY_SUBJECT_GROUP; }

connection_effect(E) ::= ALLOW. { E = FLOWIE_SECURITY_ALLOW; }
connection_effect(E) ::= DENY. { E = FLOWIE_SECURITY_DENY; }

optional_entries ::= .
optional_entries ::= LBRACE entries RBRACE.

entries ::= entry.
entries ::= entries entry.

entry ::= entry_effect(E) access_mode(A) TOPIC topic_pattern(T). {
  flowie_control_acl_entry_add(ctx, E, A, T);
}

entry_effect(E) ::= . { E = FLOWIE_SECURITY_ALLOW; }
entry_effect(E) ::= ALLOW. { E = FLOWIE_SECURITY_ALLOW; }
entry_effect(E) ::= DENY. { E = FLOWIE_SECURITY_DENY; }

access_mode(A) ::= READ. { A = FLOWIE_SECURITY_ACTION_SUBSCRIBE; }
access_mode(A) ::= WRITE. { A = FLOWIE_SECURITY_ACTION_PUBLISH; }
access_mode(A) ::= READWRITE. {
  A = FLOWIE_SECURITY_ACTION_PUBLISH | FLOWIE_SECURITY_ACTION_SUBSCRIBE;
}

topic_pattern(T) ::= TOPIC_PATTERN(P). {
  T = flowie_control_acl_topic_parse(ctx, P);
}

%syntax_error { flowie_control_acl_parse_fail(ctx); }
%parse_failure { flowie_control_acl_parse_fail(ctx); }
%stack_overflow { flowie_control_acl_parse_fail(ctx); }
