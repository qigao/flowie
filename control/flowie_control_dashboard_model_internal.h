#ifndef FLOWIE_CONTROL_DASHBOARD_MODEL_INTERNAL_H
#define FLOWIE_CONTROL_DASHBOARD_MODEL_INTERNAL_H

#include "cmeta_cmeta_data.h"
#include "flowie_control_dashboard_internal.h"
#include "monocypher.h"
#include <chttp_app/app.h>
#include <cmeta/data_reflect.h>
#include <stdlib.h>

/* Presentation frames own bounded rows and strings. CMeta exposes read-only
 * views; collection views borrow these address-stable rows through rendering. */
enum {
  FLOWIE_CONTROL_DASHBOARD_PAGE_SIZE = 25,
  FLOWIE_CONTROL_DASHBOARD_DOMAIN_LIMIT = 100,
  FLOWIE_CONTROL_DASHBOARD_GROUP_SELECTOR_LIMIT = FLOWIE_CONTROL_PAGE_MAX,
  FLOWIE_CONTROL_DASHBOARD_ROLE_SELECTOR_LIMIT = FLOWIE_CONTROL_PAGE_MAX,
  FLOWIE_CONTROL_DASHBOARD_GROUP_LABEL_MAX =
      FLOWIE_SECURITY_ID_MAX + FLOWIE_CONTROL_GROUP_MAX_DEPTH * 2 + 2,
  FLOWIE_CONTROL_DASHBOARD_RESOURCE_PATH_MAX = 1024,
  FLOWIE_CONTROL_DASHBOARD_TEMPLATE_MAX = 512 * 1024,
  FLOWIE_CONTROL_DASHBOARD_ASSET_MAX = 1024 * 1024,
  FLOWIE_CONTROL_DASHBOARD_HTML_MAX = 16 * 1024 * 1024
};

typedef struct flowie_control_dashboard_domain_model {
  tstr domain_id;
  bool selected;
} flowie_control_dashboard_domain_model;
// clang-format off
cmeta_reflect_data(flowie_control_dashboard_domain_model, "flowie.control.dashboard.domain",
    cmeta_data_field(tstr, domain_id, &cmeta_tstr_cmeta_data, &cmeta_tstr_cmeta_type)
    cmeta_field(bool, selected));
// clang-format on

static void
flowie_control_dashboard_domain_model_clear(flowie_control_dashboard_domain_model *model) {
  tstr_free(model->domain_id);
}

typedef struct flowie_control_dashboard_group_model {
  uint64_t row_index;
  tstr group_id;
  tstr parent_group_id;
  tstr tree_label;
  uint64_t depth;
  uint64_t aria_level;
  bool enabled;
  bool is_root;
  bool member_allowed;
  bool delete_candidate;
  bool add_disabled;
  bool remove_disabled;
  bool parent_disabled;
} flowie_control_dashboard_group_model;
// clang-format off
cmeta_reflect_data(flowie_control_dashboard_group_model, "flowie.control.dashboard.group",
    cmeta_data_field(uint64_t, row_index, &cmeta_data_uint64, &cmeta_type_uint64)
    cmeta_data_field(tstr, group_id, &cmeta_tstr_cmeta_data, &cmeta_tstr_cmeta_type)
    cmeta_data_field(tstr, parent_group_id, &cmeta_tstr_cmeta_data, &cmeta_tstr_cmeta_type)
    cmeta_data_field(tstr, tree_label, &cmeta_tstr_cmeta_data, &cmeta_tstr_cmeta_type)
    cmeta_data_field(uint64_t, depth, &cmeta_data_uint64, &cmeta_type_uint64)
    cmeta_data_field(uint64_t, aria_level, &cmeta_data_uint64, &cmeta_type_uint64)
    cmeta_field(bool, enabled)
    cmeta_field(bool, is_root)
    cmeta_field(bool, member_allowed)
    cmeta_field(bool, delete_candidate)
    cmeta_field(bool, add_disabled)
    cmeta_field(bool, remove_disabled)
    cmeta_field(bool, parent_disabled));
// clang-format on

static void
flowie_control_dashboard_group_model_clear(flowie_control_dashboard_group_model *model) {
  tstr_free(model->group_id);
  tstr_free(model->parent_group_id);
  tstr_free(model->tree_label);
}

typedef struct flowie_control_dashboard_user_model {
  tstr principal_id;
  tstr principal_type;
  bool enabled;
  uint64_t row_index;
  bool is_service;
  bool is_human;
} flowie_control_dashboard_user_model;
// clang-format off
cmeta_reflect_data(flowie_control_dashboard_user_model, "flowie.control.dashboard.user",
    cmeta_data_field(tstr, principal_id, &cmeta_tstr_cmeta_data, &cmeta_tstr_cmeta_type)
    cmeta_data_field(tstr, principal_type, &cmeta_tstr_cmeta_data, &cmeta_tstr_cmeta_type)
    cmeta_field(bool, enabled)
    cmeta_data_field(uint64_t, row_index, &cmeta_data_uint64, &cmeta_type_uint64)
    cmeta_field(bool, is_service)
    cmeta_field(bool, is_human));
// clang-format on

static void flowie_control_dashboard_user_model_clear(flowie_control_dashboard_user_model *model) {
  tstr_free(model->principal_id);
  tstr_free(model->principal_type);
}

typedef struct flowie_control_dashboard_role_model {
  tstr role_id;
  bool enabled;
  uint64_t row_index;
} flowie_control_dashboard_role_model;
// clang-format off
cmeta_reflect_data(flowie_control_dashboard_role_model, "flowie.control.dashboard.role",
    cmeta_data_field(tstr, role_id, &cmeta_tstr_cmeta_data, &cmeta_tstr_cmeta_type)
    cmeta_field(bool, enabled)
    cmeta_data_field(uint64_t, row_index, &cmeta_data_uint64, &cmeta_type_uint64));
// clang-format on

static void flowie_control_dashboard_role_model_clear(flowie_control_dashboard_role_model *model) {
  tstr_free(model->role_id);
}

typedef struct flowie_control_dashboard_rule_model {
  uint64_t row_index;
  uint64_t ordinal;
  tstr rule_document;
  tstr connection_label;
  tstr subject_label;
  tstr subject_kind_label;
  tstr subject_kind;
  uint64_t entry_count;
  uint64_t expanded_topic_count;
  bool uses_username;
  bool uses_client_id;
} flowie_control_dashboard_rule_model;
// clang-format off
cmeta_reflect_data(flowie_control_dashboard_rule_model, "flowie.control.dashboard.rule",
    cmeta_data_field(uint64_t, row_index, &cmeta_data_uint64, &cmeta_type_uint64)
    cmeta_data_field( uint64_t, ordinal, &cmeta_data_uint64, &cmeta_type_uint64)
    cmeta_data_field(tstr, rule_document, &cmeta_tstr_cmeta_data, &cmeta_tstr_cmeta_type)
    cmeta_data_field(tstr, connection_label, &cmeta_tstr_cmeta_data, &cmeta_tstr_cmeta_type)
    cmeta_data_field(tstr, subject_label, &cmeta_tstr_cmeta_data, &cmeta_tstr_cmeta_type)
    cmeta_data_field(tstr, subject_kind_label, &cmeta_tstr_cmeta_data, &cmeta_tstr_cmeta_type)
    cmeta_data_field(tstr, subject_kind, &cmeta_tstr_cmeta_data, &cmeta_tstr_cmeta_type)
    cmeta_data_field(uint64_t, entry_count, &cmeta_data_uint64, &cmeta_type_uint64)
    cmeta_data_field(uint64_t, expanded_topic_count, &cmeta_data_uint64, &cmeta_type_uint64)
    cmeta_field(bool, uses_username)
    cmeta_field(bool, uses_client_id));
// clang-format on

static void flowie_control_dashboard_rule_model_clear(flowie_control_dashboard_rule_model *model) {
  tstr_free(model->rule_document);
  tstr_free(model->connection_label);
  tstr_free(model->subject_label);
  tstr_free(model->subject_kind_label);
  tstr_free(model->subject_kind);
}

typedef struct flowie_control_dashboard_audit_model {
  uint64_t cursor;
  tstr actor;
  tstr operation;
  tstr target_id;
} flowie_control_dashboard_audit_model;
// clang-format off
cmeta_reflect_data(flowie_control_dashboard_audit_model, "flowie.control.dashboard.audit",
    cmeta_data_field(uint64_t, cursor, &cmeta_data_uint64, &cmeta_type_uint64)
    cmeta_data_field(tstr, actor, &cmeta_tstr_cmeta_data, &cmeta_tstr_cmeta_type)
    cmeta_data_field(tstr, operation, &cmeta_tstr_cmeta_data, &cmeta_tstr_cmeta_type)
    cmeta_data_field(tstr, target_id, &cmeta_tstr_cmeta_data, &cmeta_tstr_cmeta_type));
// clang-format on

static void
flowie_control_dashboard_audit_model_clear(flowie_control_dashboard_audit_model *model) {
  tstr_free(model->actor);
  tstr_free(model->operation);
  tstr_free(model->target_id);
}

typedef struct flowie_control_dashboard_pager_model {
  uint64_t count;
  tstr refresh_url;
  tstr query_url;
  bool first;
  tstr first_url;
  bool more;
  tstr more_url;
} flowie_control_dashboard_pager_model;
// clang-format off
cmeta_reflect_data(flowie_control_dashboard_pager_model, "flowie.control.dashboard.pager",
    cmeta_data_field(uint64_t, count, &cmeta_data_uint64, &cmeta_type_uint64)
    cmeta_data_field(tstr, refresh_url, &cmeta_tstr_cmeta_data, &cmeta_tstr_cmeta_type)
    cmeta_data_field(tstr, query_url, &cmeta_tstr_cmeta_data, &cmeta_tstr_cmeta_type)
    cmeta_field(bool, first)
    cmeta_data_field(tstr, first_url, &cmeta_tstr_cmeta_data, &cmeta_tstr_cmeta_type)
    cmeta_field(bool, more)
    cmeta_data_field(tstr, more_url, &cmeta_tstr_cmeta_data, &cmeta_tstr_cmeta_type));
// clang-format on

static void
flowie_control_dashboard_pager_model_clear(flowie_control_dashboard_pager_model *model) {
  tstr_free(model->refresh_url);
  tstr_free(model->query_url);
  tstr_free(model->first_url);
  tstr_free(model->more_url);
}

typedef struct flowie_control_dashboard_identity_model {
  tstr domain_id;
  tstr actor;
  tstr csrf;
  uint64_t policy_version;
  uint64_t draft_rule_count;
  uint64_t published_rule_count;
  tstr credential_domain;
  tstr credential_principal;
  tstr credential_token;
} flowie_control_dashboard_identity_model;
// clang-format off
cmeta_reflect_data(flowie_control_dashboard_identity_model, "flowie.control.dashboard.identity",
    cmeta_data_field(tstr, domain_id, &cmeta_tstr_cmeta_data, &cmeta_tstr_cmeta_type)
    cmeta_data_field(tstr, actor, &cmeta_tstr_cmeta_data, &cmeta_tstr_cmeta_type)
    cmeta_data_field(tstr, csrf, &cmeta_tstr_cmeta_data, &cmeta_tstr_cmeta_type)
    cmeta_data_field(uint64_t, policy_version, &cmeta_data_uint64, &cmeta_type_uint64)
    cmeta_data_field(uint64_t, draft_rule_count, &cmeta_data_uint64, &cmeta_type_uint64)
    cmeta_data_field(uint64_t, published_rule_count, &cmeta_data_uint64, &cmeta_type_uint64)
    cmeta_data_field(tstr, credential_domain, &cmeta_tstr_cmeta_data, &cmeta_tstr_cmeta_type)
    cmeta_data_field(tstr, credential_principal, &cmeta_tstr_cmeta_data, &cmeta_tstr_cmeta_type)
    cmeta_data_field(tstr, credential_token, &cmeta_tstr_cmeta_data, &cmeta_tstr_cmeta_type));
// clang-format on

static void
flowie_control_dashboard_identity_model_clear(flowie_control_dashboard_identity_model *model) {
  tstr_free(model->domain_id);
  tstr_free(model->actor);
  tstr_free(model->csrf);
  tstr_free(model->credential_domain);
  tstr_free(model->credential_principal);
  if (model->credential_token)
    crypto_wipe(model->credential_token, tstr_len(model->credential_token));
  tstr_free(model->credential_token);
}

typedef struct flowie_control_dashboard_navigation_model {
  tstr rpc_path;
  tstr action_url;
  tstr overview_path;
  tstr users_path;
  tstr groups_path;
  tstr roles_path;
  tstr acls_path;
  tstr audit_path;
  tstr integration_path;
} flowie_control_dashboard_navigation_model;
// clang-format off
cmeta_reflect_data(flowie_control_dashboard_navigation_model, "flowie.control.dashboard.navigation",
    cmeta_data_field(tstr, rpc_path, &cmeta_tstr_cmeta_data, &cmeta_tstr_cmeta_type)
    cmeta_data_field(tstr, action_url, &cmeta_tstr_cmeta_data, &cmeta_tstr_cmeta_type)
    cmeta_data_field(tstr, overview_path, &cmeta_tstr_cmeta_data, &cmeta_tstr_cmeta_type)
    cmeta_data_field(tstr, users_path, &cmeta_tstr_cmeta_data, &cmeta_tstr_cmeta_type)
    cmeta_data_field(tstr, groups_path, &cmeta_tstr_cmeta_data, &cmeta_tstr_cmeta_type)
    cmeta_data_field(tstr, roles_path, &cmeta_tstr_cmeta_data, &cmeta_tstr_cmeta_type)
    cmeta_data_field(tstr, acls_path, &cmeta_tstr_cmeta_data, &cmeta_tstr_cmeta_type)
    cmeta_data_field(tstr, audit_path, &cmeta_tstr_cmeta_data, &cmeta_tstr_cmeta_type)
    cmeta_data_field(tstr, integration_path, &cmeta_tstr_cmeta_data, &cmeta_tstr_cmeta_type));
// clang-format on

static void
flowie_control_dashboard_navigation_model_clear(flowie_control_dashboard_navigation_model *model) {
  tstr_free(model->rpc_path);
  tstr_free(model->action_url);
  tstr_free(model->overview_path);
  tstr_free(model->users_path);
  tstr_free(model->groups_path);
  tstr_free(model->roles_path);
  tstr_free(model->acls_path);
  tstr_free(model->audit_path);
  tstr_free(model->integration_path);
}

typedef struct flowie_control_dashboard_capabilities_model {
  bool can_user_admin;
  bool can_security_admin;
  bool can_policy_admin;
  bool can_manage_access;
  bool can_audit_read;
  bool can_create_domain;
  bool is_platform_workspace;
  bool is_domain_workspace;
  bool credential_issued;
  bool user_options_truncated;
  bool role_options_truncated;
} flowie_control_dashboard_capabilities_model;
// clang-format off
cmeta_reflect_data(flowie_control_dashboard_capabilities_model, "flowie.control.dashboard.capabilities",
    cmeta_field(bool, can_user_admin)
    cmeta_field(bool, can_security_admin)
    cmeta_field(bool, can_policy_admin)
    cmeta_field(bool, can_manage_access)
    cmeta_field(bool, can_audit_read)
    cmeta_field(bool, can_create_domain)
    cmeta_field(bool, is_platform_workspace)
    cmeta_field(bool, is_domain_workspace)
    cmeta_field(bool, credential_issued)
    cmeta_field(bool, user_options_truncated)
    cmeta_field(bool, role_options_truncated));
// clang-format on

typedef struct flowie_control_dashboard_sections_model {
  bool show_overview;
  bool show_users;
  bool show_groups;
  bool show_roles;
  bool show_acls;
  bool show_audit;
  bool show_integration;
  bool is_overview;
  bool is_users;
  bool is_groups;
  bool is_roles;
  bool is_acls;
  bool is_audit;
  bool is_integration;
} flowie_control_dashboard_sections_model;
// clang-format off
cmeta_reflect_data(flowie_control_dashboard_sections_model, "flowie.control.dashboard.sections",
    cmeta_field(bool, show_overview)
    cmeta_field(bool, show_users)
    cmeta_field(bool, show_groups)
    cmeta_field(bool, show_roles)
    cmeta_field(bool, show_acls)
    cmeta_field(bool, show_audit)
    cmeta_field(bool, show_integration)
    cmeta_field(bool, is_overview)
    cmeta_field(bool, is_users)
    cmeta_field(bool, is_groups)
    cmeta_field(bool, is_roles)
    cmeta_field(bool, is_acls)
    cmeta_field(bool, is_audit)
    cmeta_field(bool, is_integration));
// clang-format on

typedef struct flowie_control_dashboard_collections_model {
  chttp_web_sequence_view domains;
  chttp_web_sequence_view group_options;
  chttp_web_sequence_view user_options;
  chttp_web_sequence_view role_options;
  chttp_web_sequence_view users;
  chttp_web_sequence_view roles;
  chttp_web_sequence_view rules;
  chttp_web_sequence_view audits;
} flowie_control_dashboard_collections_model;
// clang-format off
cmeta_reflect_data(flowie_control_dashboard_collections_model, "flowie.control.dashboard.collections",
    cmeta_data_field(chttp_web_sequence_view, domains, &cmeta_data_sequence_view, &cmeta_type_collection_view)
    cmeta_data_field(chttp_web_sequence_view, group_options, &cmeta_data_sequence_view, &cmeta_type_collection_view)
    cmeta_data_field(chttp_web_sequence_view, user_options, &cmeta_data_sequence_view, &cmeta_type_collection_view)
    cmeta_data_field(chttp_web_sequence_view, role_options, &cmeta_data_sequence_view, &cmeta_type_collection_view)
    cmeta_data_field(chttp_web_sequence_view, users, &cmeta_data_sequence_view, &cmeta_type_collection_view)
    cmeta_data_field(chttp_web_sequence_view, roles, &cmeta_data_sequence_view, &cmeta_type_collection_view)
    cmeta_data_field(chttp_web_sequence_view, rules, &cmeta_data_sequence_view, &cmeta_type_collection_view)
    cmeta_data_field(chttp_web_sequence_view, audits, &cmeta_data_sequence_view, &cmeta_type_collection_view));
// clang-format on

typedef struct flowie_control_dashboard_pagination_model {
  flowie_control_dashboard_pager_model users_pager;
  flowie_control_dashboard_pager_model roles_pager;
  flowie_control_dashboard_pager_model policy_pager;
  flowie_control_dashboard_pager_model audit_pager;
} flowie_control_dashboard_pagination_model;
// clang-format off
cmeta_reflect_data(flowie_control_dashboard_pagination_model, "flowie.control.dashboard.pagination",
    cmeta_data_field(flowie_control_dashboard_pager_model, users_pager, cmeta_reflected_data(flowie_control_dashboard_pager_model), cmeta_reflected_storage(flowie_control_dashboard_pager_model))
    cmeta_data_field(flowie_control_dashboard_pager_model, roles_pager, cmeta_reflected_data(flowie_control_dashboard_pager_model), cmeta_reflected_storage(flowie_control_dashboard_pager_model))
    cmeta_data_field(flowie_control_dashboard_pager_model, policy_pager, cmeta_reflected_data(flowie_control_dashboard_pager_model), cmeta_reflected_storage(flowie_control_dashboard_pager_model))
    cmeta_data_field(flowie_control_dashboard_pager_model, audit_pager, cmeta_reflected_data(flowie_control_dashboard_pager_model), cmeta_reflected_storage(flowie_control_dashboard_pager_model)));
// clang-format on

static void
flowie_control_dashboard_pagination_model_clear(flowie_control_dashboard_pagination_model *model) {
  flowie_control_dashboard_pager_model_clear(&model->users_pager);
  flowie_control_dashboard_pager_model_clear(&model->roles_pager);
  flowie_control_dashboard_pager_model_clear(&model->policy_pager);
  flowie_control_dashboard_pager_model_clear(&model->audit_pager);
}

typedef struct flowie_control_dashboard_content_model {
  flowie_control_dashboard_identity_model identity;
  flowie_control_dashboard_navigation_model navigation;
  flowie_control_dashboard_capabilities_model capabilities;
  flowie_control_dashboard_sections_model sections;
  flowie_control_dashboard_collections_model collections;
  flowie_control_dashboard_pagination_model pagination;
  flowie_control_dashboard_domain_model domains_storage[FLOWIE_CONTROL_DASHBOARD_DOMAIN_LIMIT];
  flowie_control_dashboard_group_model
      group_options_storage[FLOWIE_CONTROL_DASHBOARD_GROUP_SELECTOR_LIMIT];
  flowie_control_dashboard_user_model user_options_storage[FLOWIE_CONTROL_PAGE_MAX];
  flowie_control_dashboard_role_model
      role_options_storage[FLOWIE_CONTROL_DASHBOARD_ROLE_SELECTOR_LIMIT];
  flowie_control_dashboard_user_model users_storage[FLOWIE_CONTROL_DASHBOARD_PAGE_SIZE];
  flowie_control_dashboard_role_model roles_storage[FLOWIE_CONTROL_DASHBOARD_PAGE_SIZE];
  flowie_control_dashboard_rule_model rules_storage[FLOWIE_CONTROL_DASHBOARD_PAGE_SIZE];
  flowie_control_dashboard_audit_model audits_storage[FLOWIE_CONTROL_DASHBOARD_PAGE_SIZE];
} flowie_control_dashboard_content_model;
// clang-format off
cmeta_reflect_data(flowie_control_dashboard_content_model, "flowie.control.dashboard.content",
    cmeta_data_field(flowie_control_dashboard_identity_model, identity, cmeta_reflected_data(flowie_control_dashboard_identity_model), cmeta_reflected_storage(flowie_control_dashboard_identity_model))
    cmeta_data_field(flowie_control_dashboard_navigation_model, navigation, cmeta_reflected_data(flowie_control_dashboard_navigation_model), cmeta_reflected_storage(flowie_control_dashboard_navigation_model))
    cmeta_data_field(flowie_control_dashboard_capabilities_model, capabilities, cmeta_reflected_data(flowie_control_dashboard_capabilities_model), cmeta_reflected_storage(flowie_control_dashboard_capabilities_model))
    cmeta_data_field(flowie_control_dashboard_sections_model, sections, cmeta_reflected_data(flowie_control_dashboard_sections_model), cmeta_reflected_storage(flowie_control_dashboard_sections_model))
    cmeta_data_field( flowie_control_dashboard_collections_model, collections, cmeta_reflected_data(flowie_control_dashboard_collections_model), cmeta_reflected_storage(flowie_control_dashboard_collections_model))
    cmeta_data_field( flowie_control_dashboard_pagination_model, pagination, cmeta_reflected_data(flowie_control_dashboard_pagination_model), cmeta_reflected_storage(flowie_control_dashboard_pagination_model)));
// clang-format on

static void
flowie_control_dashboard_content_model_init(flowie_control_dashboard_content_model *model) {
  model->collections.domains =
      (chttp_web_sequence_view){model->domains_storage, 0u, sizeof(model->domains_storage[0]),
                                cmeta_reflected_data(flowie_control_dashboard_domain_model)};
  model->collections.group_options = (chttp_web_sequence_view){
      model->group_options_storage, 0u, sizeof(model->group_options_storage[0]),
      cmeta_reflected_data(flowie_control_dashboard_group_model)};
  model->collections.user_options = (chttp_web_sequence_view){
      model->user_options_storage, 0u, sizeof(model->user_options_storage[0]),
      cmeta_reflected_data(flowie_control_dashboard_user_model)};
  model->collections.role_options = (chttp_web_sequence_view){
      model->role_options_storage, 0u, sizeof(model->role_options_storage[0]),
      cmeta_reflected_data(flowie_control_dashboard_role_model)};
  model->collections.users =
      (chttp_web_sequence_view){model->users_storage, 0u, sizeof(model->users_storage[0]),
                                cmeta_reflected_data(flowie_control_dashboard_user_model)};
  model->collections.roles =
      (chttp_web_sequence_view){model->roles_storage, 0u, sizeof(model->roles_storage[0]),
                                cmeta_reflected_data(flowie_control_dashboard_role_model)};
  model->collections.rules =
      (chttp_web_sequence_view){model->rules_storage, 0u, sizeof(model->rules_storage[0]),
                                cmeta_reflected_data(flowie_control_dashboard_rule_model)};
  model->collections.audits =
      (chttp_web_sequence_view){model->audits_storage, 0u, sizeof(model->audits_storage[0]),
                                cmeta_reflected_data(flowie_control_dashboard_audit_model)};
}

static void
flowie_control_dashboard_content_model_clear(flowie_control_dashboard_content_model *model) {
  flowie_control_dashboard_identity_model_clear(&model->identity);
  flowie_control_dashboard_navigation_model_clear(&model->navigation);
  flowie_control_dashboard_pagination_model_clear(&model->pagination);
  for (size_t index = 0u; index < FLOWIE_CONTROL_DASHBOARD_DOMAIN_LIMIT; ++index)
    flowie_control_dashboard_domain_model_clear(&model->domains_storage[index]);
  for (size_t index = 0u; index < FLOWIE_CONTROL_DASHBOARD_GROUP_SELECTOR_LIMIT; ++index)
    flowie_control_dashboard_group_model_clear(&model->group_options_storage[index]);
  for (size_t index = 0u; index < FLOWIE_CONTROL_PAGE_MAX; ++index)
    flowie_control_dashboard_user_model_clear(&model->user_options_storage[index]);
  for (size_t index = 0u; index < FLOWIE_CONTROL_DASHBOARD_ROLE_SELECTOR_LIMIT; ++index)
    flowie_control_dashboard_role_model_clear(&model->role_options_storage[index]);
  for (size_t index = 0u; index < FLOWIE_CONTROL_DASHBOARD_PAGE_SIZE; ++index)
    flowie_control_dashboard_user_model_clear(&model->users_storage[index]);
  for (size_t index = 0u; index < FLOWIE_CONTROL_DASHBOARD_PAGE_SIZE; ++index)
    flowie_control_dashboard_role_model_clear(&model->roles_storage[index]);
  for (size_t index = 0u; index < FLOWIE_CONTROL_DASHBOARD_PAGE_SIZE; ++index)
    flowie_control_dashboard_rule_model_clear(&model->rules_storage[index]);
  for (size_t index = 0u; index < FLOWIE_CONTROL_DASHBOARD_PAGE_SIZE; ++index)
    flowie_control_dashboard_audit_model_clear(&model->audits_storage[index]);
}

static int flowie_control_dashboard_model_string(tstr *out, const char *value) {
  tstr copy;
  if (!out || !value) return SALTS_EINVAL;
  copy = tstr_dup(value);
  if (!copy) return SALTS_ENOMEM;
  tstr_free(*out);
  *out = copy;
  return SALTS_OK;
}

#endif
