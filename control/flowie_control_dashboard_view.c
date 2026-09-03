#include "flowie_control_acl_internal.h"
#include "flowie_control_dashboard_view_internal.h"

#include "fmt.h"
#include "monocypher.h"
#include <mustache/mustache_json.h>
#include "salts_error.h"
#include "salts_fs.h"
#include <json_parser.h>
#include "salts_str.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  FLOWIE_CONTROL_DASHBOARD_PAGE_SIZE = 25,
  FLOWIE_CONTROL_DASHBOARD_DOMAIN_LIMIT = 100,
  FLOWIE_CONTROL_DASHBOARD_GROUP_SELECTOR_LIMIT = FLOWIE_CONTROL_PAGE_MAX,
  FLOWIE_CONTROL_DASHBOARD_ROLE_SELECTOR_LIMIT = FLOWIE_CONTROL_PAGE_MAX,
  FLOWIE_CONTROL_DASHBOARD_GROUP_LABEL_MAX =
      FLOWIE_SECURITY_ID_MAX + FLOWIE_CONTROL_GROUP_MAX_DEPTH * 2 + 2,
  FLOWIE_CONTROL_DASHBOARD_RESOURCE_PATH_MAX = 1024,
  FLOWIE_CONTROL_DASHBOARD_TEMPLATE_MAX = 512 * 1024,
  FLOWIE_CONTROL_DASHBOARD_ASSET_MAX = 1024 * 1024
};

static const char FLOWIE_CONTROL_DASHBOARD_SHELL_TEMPLATE[] = "templates/dashboard.mustache";
static const char FLOWIE_CONTROL_DASHBOARD_CONTENT_TEMPLATE[] =
    "templates/dashboard_content.mustache";
static const char FLOWIE_CONTROL_DASHBOARD_ERROR_TEMPLATE[] = "templates/dashboard_error.mustache";
static const char FLOWIE_CONTROL_DASHBOARD_LOGIN_TEMPLATE[] = "templates/login.mustache";
static const char FLOWIE_CONTROL_DASHBOARD_PASSWORD_TEMPLATE[] = "templates/password.mustache";
static const char FLOWIE_CONTROL_DASHBOARD_CSS_ASSET[] = "assets/control.css";
static const char FLOWIE_CONTROL_DASHBOARD_JS_ASSET[] = "assets/control.js";
static const char FLOWIE_CONTROL_DASHBOARD_HTMX_ASSET[] = "assets/htmx-2.0.9.min.js";

typedef enum flowie_control_dashboard_cursor_kind_e {
  FLOWIE_CONTROL_DASHBOARD_USERS_CURSOR = 0,
  FLOWIE_CONTROL_DASHBOARD_GROUPS_CURSOR,
  FLOWIE_CONTROL_DASHBOARD_ROLES_CURSOR,
  FLOWIE_CONTROL_DASHBOARD_POLICY_CURSOR,
  FLOWIE_CONTROL_DASHBOARD_AUDIT_CURSOR
} flowie_control_dashboard_cursor_kind_t;

static const char *
flowie_control_dashboard_section_name(flowie_control_dashboard_section_t section) {
  switch (section) {
  case FLOWIE_CONTROL_DASHBOARD_SECTION_OVERVIEW:
    return "overview";
  case FLOWIE_CONTROL_DASHBOARD_SECTION_USERS:
    return "users";
  case FLOWIE_CONTROL_DASHBOARD_SECTION_GROUPS:
    return "groups";
  case FLOWIE_CONTROL_DASHBOARD_SECTION_ROLES:
    return "roles";
  case FLOWIE_CONTROL_DASHBOARD_SECTION_ACLS:
    return "acls";
  case FLOWIE_CONTROL_DASHBOARD_SECTION_AUDIT:
    return "audit";
  case FLOWIE_CONTROL_DASHBOARD_SECTION_INTEGRATION:
    return "integration";
  default:
    return NULL;
  }
}

static const char *
flowie_control_dashboard_section_title(flowie_control_dashboard_section_t section) {
  switch (section) {
  case FLOWIE_CONTROL_DASHBOARD_SECTION_USERS:
    return "Users | Flowie Control";
  case FLOWIE_CONTROL_DASHBOARD_SECTION_GROUPS:
    return "Groups | Flowie Control";
  case FLOWIE_CONTROL_DASHBOARD_SECTION_ROLES:
    return "Roles | Flowie Control";
  case FLOWIE_CONTROL_DASHBOARD_SECTION_ACLS:
    return "ACL rules | Flowie Control";
  case FLOWIE_CONTROL_DASHBOARD_SECTION_AUDIT:
    return "Audit | Flowie Control";
  case FLOWIE_CONTROL_DASHBOARD_SECTION_INTEGRATION:
    return "Integration | Flowie Control";
  default:
    return "Flowie Control";
  }
}

static const char *
flowie_control_dashboard_subject_kind_label(flowie_security_subject_kind_t subject_kind) {
  switch (subject_kind) {
  case FLOWIE_SECURITY_SUBJECT_PRINCIPAL:
    return "User";
  case FLOWIE_SECURITY_SUBJECT_ROLE:
    return "Role";
  case FLOWIE_SECURITY_SUBJECT_GROUP:
    return "Group";
  default:
    return NULL;
  }
}

static const char *
flowie_control_dashboard_subject_kind_value(flowie_security_subject_kind_t subject_kind) {
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

struct flowie_control_dashboard_view_s {
  MUSTACHE_TEMPLATE *shell_template;
  MUSTACHE_TEMPLATE *content_template;
  MUSTACHE_TEMPLATE *error_template;
  MUSTACHE_TEMPLATE *login_template;
  MUSTACHE_TEMPLATE *password_template;
  salts_fs_buf_t css;
  salts_fs_buf_t javascript;
  salts_fs_buf_t htmx;
};

static void flowie_control_dashboard_json_free(json_value_t *value) {
  json_value_t *document = value;
  json_free(document);
}

static int flowie_control_dashboard_json_take(json_value_t *object, const char *key,
                                              json_value_t *value) {
  if (!object || !key || !value) {
    flowie_control_dashboard_json_free(value);
    return SALTS_ENOMEM;
  }
  if (!json_object_add_checked(object, key, value)) {
    flowie_control_dashboard_json_free(value);
    return SALTS_ENOMEM;
  }
  return SALTS_OK;
}

static int flowie_control_dashboard_json_array_take(json_value_t *array, json_value_t *value) {
  if (!array || !value) {
    flowie_control_dashboard_json_free(value);
    return SALTS_ENOMEM;
  }
  if (!json_array_add_checked(array, value)) {
    flowie_control_dashboard_json_free(value);
    return SALTS_ENOMEM;
  }
  return SALTS_OK;
}

static int flowie_control_dashboard_json_string(json_value_t *object, const char *key,
                                                const char *value) {
  if (!value) return SALTS_EINVAL;
  return flowie_control_dashboard_json_take(object, key, json_create_string(value));
}

static int flowie_control_dashboard_json_u64(json_value_t *object, const char *key,
                                             uint64_t value) {
  return flowie_control_dashboard_json_take(object, key, json_create_uint64(value));
}

static int flowie_control_dashboard_json_bool(json_value_t *object, const char *key, int value) {
  return flowie_control_dashboard_json_take(object, key, json_create_bool(value != 0));
}

static int flowie_control_dashboard_read(const char *resource_directory, const char *relative_path,
                                         size_t maximum, salts_fs_buf_t *out) {
  char path[FLOWIE_CONTROL_DASHBOARD_RESOURCE_PATH_MAX];
  int rc;
  if (!resource_directory || !relative_path || !out) return SALTS_EINVAL;
  memset(out, 0, sizeof(*out));
  rc = salts_fs_path_join(path, sizeof(path), resource_directory, relative_path);
  if (rc != SALTS_OK) return rc;
  rc = salts_fs_read_file(path, out);
  if (rc != SALTS_OK) return rc;
  if (!out->base || out->len == 0u || out->len > maximum) {
    salts_fs_buf_free(out);
    return SALTS_EPROTO;
  }
  return SALTS_OK;
}

static int flowie_control_dashboard_compile(const char *resource_directory,
                                            const char *relative_path,
                                            MUSTACHE_TEMPLATE **template_out) {
  salts_fs_buf_t source = {0};
  MUSTACHE_TEMPLATE *compiled;
  int rc;
  if (template_out) *template_out = NULL;
  if (!template_out) return SALTS_EINVAL;
  rc = flowie_control_dashboard_read(resource_directory, relative_path,
                                     FLOWIE_CONTROL_DASHBOARD_TEMPLATE_MAX, &source);
  if (rc != SALTS_OK) return rc;
  compiled = mustache_compile(source.base, source.len, NULL, NULL, 0u);
  salts_fs_buf_free(&source);
  if (!compiled) return SALTS_EPROTO;
  *template_out = compiled;
  return SALTS_OK;
}

static int flowie_control_dashboard_render_template(const MUSTACHE_TEMPLATE *template_value,
                                                    json_value_t *model, char **html_out,
                                                    size_t *html_size_out) {
  MUSTACHE_STRING_RENDERER renderer = {0};
  char *html = NULL;
  int rc = SALTS_ENOMEM;
  if (html_out) *html_out = NULL;
  if (html_size_out) *html_size_out = 0u;
  if (!template_value || !model || !html_out || !html_size_out) return SALTS_EINVAL;
  if (mustache_string_renderer_init(&renderer) != 0) return SALTS_ENOMEM;
  if (mustache_render_json(template_value, model, &renderer.base, &renderer, NULL, NULL) != 0)
    goto done;
  html = mustache_string_renderer_get(&renderer);
  if (!html) goto done;
  *html_size_out = strlen(html);
  *html_out = html;
  rc = SALTS_OK;
done:
  mustache_string_renderer_free(&renderer);
  return rc;
}

static int flowie_control_dashboard_page_has_cursor(const flowie_control_dashboard_page_t *page,
                                                    flowie_control_dashboard_cursor_kind_t kind) {
  switch (kind) {
  case FLOWIE_CONTROL_DASHBOARD_USERS_CURSOR:
    return page->users_after[0] != '\0';
  case FLOWIE_CONTROL_DASHBOARD_GROUPS_CURSOR:
    return page->groups_after[0] != '\0';
  case FLOWIE_CONTROL_DASHBOARD_ROLES_CURSOR:
    return page->roles_after[0] != '\0';
  case FLOWIE_CONTROL_DASHBOARD_POLICY_CURSOR:
    return page->policy_has_after;
  case FLOWIE_CONTROL_DASHBOARD_AUDIT_CURSOR:
    return page->audit_has_after;
  default:
    return 0;
  }
}

static void
flowie_control_dashboard_page_clear_cursor(flowie_control_dashboard_page_t *page,
                                           flowie_control_dashboard_cursor_kind_t kind) {
  switch (kind) {
  case FLOWIE_CONTROL_DASHBOARD_USERS_CURSOR:
    page->users_after[0] = '\0';
    break;
  case FLOWIE_CONTROL_DASHBOARD_GROUPS_CURSOR:
    page->groups_after[0] = '\0';
    break;
  case FLOWIE_CONTROL_DASHBOARD_ROLES_CURSOR:
    page->roles_after[0] = '\0';
    break;
  case FLOWIE_CONTROL_DASHBOARD_POLICY_CURSOR:
    page->policy_after = 0u;
    page->policy_has_after = 0;
    break;
  case FLOWIE_CONTROL_DASHBOARD_AUDIT_CURSOR:
    page->audit_after = 0u;
    page->audit_has_after = 0;
    break;
  }
}

static int flowie_control_dashboard_page_set_cursor(flowie_control_dashboard_page_t *page,
                                                    flowie_control_dashboard_cursor_kind_t kind,
                                                    const char *next_text, uint64_t next_number) {
  switch (kind) {
  case FLOWIE_CONTROL_DASHBOARD_USERS_CURSOR:
    if (!next_text) return SALTS_EINVAL;
    (void)snprintf(page->users_after, sizeof(page->users_after), "%s", next_text);
    break;
  case FLOWIE_CONTROL_DASHBOARD_GROUPS_CURSOR:
    if (!next_text) return SALTS_EINVAL;
    (void)snprintf(page->groups_after, sizeof(page->groups_after), "%s", next_text);
    break;
  case FLOWIE_CONTROL_DASHBOARD_ROLES_CURSOR:
    if (!next_text) return SALTS_EINVAL;
    (void)snprintf(page->roles_after, sizeof(page->roles_after), "%s", next_text);
    break;
  case FLOWIE_CONTROL_DASHBOARD_POLICY_CURSOR:
    page->policy_after = (uint32_t)next_number;
    page->policy_has_after = 1;
    break;
  case FLOWIE_CONTROL_DASHBOARD_AUDIT_CURSOR:
    page->audit_after = next_number;
    page->audit_has_after = 1;
    break;
  default:
    return SALTS_EINVAL;
  }
  return SALTS_OK;
}

static int flowie_control_dashboard_url_pair(tstr *url, int *has_query, const char *key,
                                             const char *value) {
  char *encoded;
  tstr next;
  if (!url || !*url || !has_query || !key || !value) return SALTS_EINVAL;
  encoded = flowie_control_http_url_encode(value);
  if (!encoded) return SALTS_ENOMEM;
  next = tstr_append_format(*url, "{}{}={}", *has_query ? "&" : "?", key, encoded);
  free(encoded);
  if (!next) {
    *url = NULL;
    return SALTS_ENOMEM;
  }
  *url = next;
  *has_query = 1;
  return SALTS_OK;
}

static int flowie_control_dashboard_url(const char *base,
                                        const flowie_control_dashboard_page_t *page,
                                        char **url_out) {
  char number[32];
  tstr url;
  int has_query = 0;
  int rc = SALTS_OK;
  if (url_out) *url_out = NULL;
  if (!base || !page || !url_out) return SALTS_EINVAL;
  url = tstr_dup(base);
  if (!url) return SALTS_ENOMEM;
  if (page->domain_id[0])
    rc = flowie_control_dashboard_url_pair(&url, &has_query, "domain_id", page->domain_id);
  if (page->section != FLOWIE_CONTROL_DASHBOARD_SECTION_ALL) {
    const char *section = flowie_control_dashboard_section_name(page->section);
    if (!section) {
      tstr_free(url);
      return SALTS_EINVAL;
    }
    if (rc == SALTS_OK)
      rc = flowie_control_dashboard_url_pair(&url, &has_query, "section", section);
  }
  if (rc == SALTS_OK && page->users_after[0])
    rc = flowie_control_dashboard_url_pair(&url, &has_query, "users_after", page->users_after);
  if (rc == SALTS_OK && page->groups_after[0])
    rc = flowie_control_dashboard_url_pair(&url, &has_query, "groups_after", page->groups_after);
  if (rc == SALTS_OK && page->roles_after[0])
    rc = flowie_control_dashboard_url_pair(&url, &has_query, "roles_after", page->roles_after);
  if (rc == SALTS_OK && page->policy_has_after) {
    (void)snprintf(number, sizeof(number), "%u", page->policy_after);
    rc = flowie_control_dashboard_url_pair(&url, &has_query, "policy_after", number);
  }
  if (rc == SALTS_OK && page->audit_has_after) {
    (void)snprintf(number, sizeof(number), "%llu", (unsigned long long)page->audit_after);
    rc = flowie_control_dashboard_url_pair(&url, &has_query, "audit_after", number);
  }
  if (rc != SALTS_OK) {
    tstr_free(url);
    return rc;
  }
  *url_out = url;
  return SALTS_OK;
}

static int flowie_control_dashboard_navigation_url(const char *base,
                                                   const flowie_control_dashboard_page_t *page,
                                                   char **url_out) {
  flowie_control_dashboard_page_t target;
  if (!base || !page || !url_out) return SALTS_EINVAL;
  target = (flowie_control_dashboard_page_t)FLOWIE_CONTROL_DASHBOARD_PAGE_INIT;
  memcpy(target.domain_id, page->domain_id, sizeof(target.domain_id));
  return flowie_control_dashboard_url(base, &target, url_out);
}

static int
flowie_control_dashboard_add_domains(json_value_t *model,
                                     flowie_control_management_service_t *service,
                                     const flowie_control_management_caller_t *authority_caller,
                                     const flowie_control_management_caller_t *scoped_caller) {
  flowie_control_domain_view_t roots[FLOWIE_CONTROL_DASHBOARD_DOMAIN_LIMIT];
  json_value_t *array = json_create_array();
  size_t count = 0u;
  int has_more = 0;
  int rc;
  if (!array) return SALTS_ENOMEM;
  for (size_t index = 0u; index < FLOWIE_CONTROL_DASHBOARD_DOMAIN_LIMIT; ++index)
    roots[index] = (flowie_control_domain_view_t)FLOWIE_CONTROL_DOMAIN_VIEW_INIT;
  rc = flowie_control_management_domain_list(service, authority_caller, NULL, roots,
                                             FLOWIE_CONTROL_DASHBOARD_DOMAIN_LIMIT, &count,
                                             &has_more);
  (void)has_more;
  for (size_t index = 0u; rc == SALTS_OK && index < count; ++index) {
    json_value_t *item = json_create_object();
    if (strcmp(roots[index].domain_id, FLOWIE_CONTROL_MANAGEMENT_SYSTEM_DOMAIN) == 0) {
      flowie_control_dashboard_json_free(item);
      continue;
    }
    if (!item) {
      rc = SALTS_ENOMEM;
      break;
    }
    rc = flowie_control_dashboard_json_string(item, "domain_id", roots[index].domain_id);
    if (rc == SALTS_OK)
      rc = flowie_control_dashboard_json_bool(
          item, "selected", strcmp(roots[index].domain_id, scoped_caller->domain_id) == 0);
    if (rc == SALTS_OK) rc = flowie_control_dashboard_json_array_take(array, item);
    else flowie_control_dashboard_json_free(item);
  }
  if (rc == SALTS_OK) rc = flowie_control_dashboard_json_take(model, "domains", array);
  else flowie_control_dashboard_json_free(array);
  return rc;
}

static int flowie_control_dashboard_add_pager(json_value_t *model, const char *key,
                                              const flowie_control_dashboard_page_t *page,
                                              flowie_control_dashboard_cursor_kind_t kind,
                                              size_t count, int has_more, const char *next_text,
                                              uint64_t next_number) {
  flowie_control_dashboard_page_t target = *page;
  json_value_t *pager = json_create_object();
  char *url = NULL;
  int has_first = flowie_control_dashboard_page_has_cursor(page, kind);
  int rc;
  if (!pager) return SALTS_ENOMEM;
  rc = flowie_control_dashboard_json_u64(pager, "count", count);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_url(FLOWIE_CONTROL_DASHBOARD_CONTENT_PATH, page, &url);
  if (rc == SALTS_OK) rc = flowie_control_dashboard_json_string(pager, "refresh_url", url);
  tstr_free(url);
  url = NULL;
  target = *page;
  flowie_control_dashboard_page_clear_cursor(&target, kind);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_url(FLOWIE_CONTROL_DASHBOARD_CONTENT_PATH, &target, &url);
  if (rc == SALTS_OK) rc = flowie_control_dashboard_json_string(pager, "query_url", url);
  if (rc == SALTS_OK) rc = flowie_control_dashboard_json_bool(pager, "first", has_first);
  if (rc == SALTS_OK && has_first)
    rc = flowie_control_dashboard_json_string(pager, "first_url", url);
  tstr_free(url);
  url = NULL;
  if (rc == SALTS_OK) rc = flowie_control_dashboard_json_bool(pager, "more", has_more);
  if (rc == SALTS_OK && has_more) {
    target = *page;
    rc = flowie_control_dashboard_page_set_cursor(&target, kind, next_text, next_number);
    if (rc == SALTS_OK)
      rc = flowie_control_dashboard_url(FLOWIE_CONTROL_DASHBOARD_CONTENT_PATH, &target, &url);
    if (rc == SALTS_OK) rc = flowie_control_dashboard_json_string(pager, "more_url", url);
    tstr_free(url);
  }
  if (rc == SALTS_OK) rc = flowie_control_dashboard_json_take(model, key, pager);
  else flowie_control_dashboard_json_free(pager);
  return rc;
}

static int
flowie_control_dashboard_group_label(const flowie_control_group_view_t *group,
                                     char label[FLOWIE_CONTROL_DASHBOARD_GROUP_LABEL_MAX]) {
  size_t group_id_size;
  size_t offset = 0u;
  if (!group || !label || group->depth > FLOWIE_CONTROL_GROUP_MAX_DEPTH) return SALTS_EINVAL;
  group_id_size = strnlen(group->group_id, sizeof(group->group_id));
  if (group_id_size == 0u || group_id_size >= sizeof(group->group_id) ||
      group->depth * 2u + (group->depth ? 1u : 0u) + group_id_size + 1u >
          FLOWIE_CONTROL_DASHBOARD_GROUP_LABEL_MAX)
    return SALTS_EPROTO;
  for (uint32_t depth = 0u; depth < group->depth; ++depth) {
    label[offset++] = '-';
    label[offset++] = '-';
  }
  if (group->depth) label[offset++] = ' ';
  memcpy(label + offset, group->group_id, group_id_size + 1u);
  return SALTS_OK;
}

static int flowie_control_dashboard_add_group_option(json_value_t *array,
                                                     const flowie_control_group_view_t *group,
                                                     size_t row_index, int has_children) {
  char label[FLOWIE_CONTROL_DASHBOARD_GROUP_LABEL_MAX];
  json_value_t *item;
  int rc;
  if (!array || !group) return SALTS_EINVAL;
  rc = flowie_control_dashboard_group_label(group, label);
  if (rc != SALTS_OK) return rc;
  item = json_create_object();
  if (!item) return SALTS_ENOMEM;
  rc = flowie_control_dashboard_json_u64(item, "row_index", row_index);
  if (rc == SALTS_OK) rc = flowie_control_dashboard_json_string(item, "group_id", group->group_id);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_json_string(item, "parent_group_id", group->parent_group_id);
  if (rc == SALTS_OK) rc = flowie_control_dashboard_json_string(item, "tree_label", label);
  if (rc == SALTS_OK) rc = flowie_control_dashboard_json_u64(item, "depth", group->depth);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_json_u64(item, "aria_level", (uint64_t)group->depth + 1u);
  if (rc == SALTS_OK) rc = flowie_control_dashboard_json_bool(item, "enabled", group->enabled);
  if (rc == SALTS_OK) rc = flowie_control_dashboard_json_bool(item, "is_root", 0);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_json_bool(item, "member_allowed", group->enabled);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_json_bool(item, "delete_candidate", !has_children);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_json_bool(item, "add_disabled", !group->enabled);
  if (rc == SALTS_OK) rc = flowie_control_dashboard_json_bool(item, "remove_disabled", 0);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_json_bool(
        item, "parent_disabled", !group->enabled || group->depth >= FLOWIE_CONTROL_GROUP_MAX_DEPTH);
  if (rc == SALTS_OK) rc = flowie_control_dashboard_json_array_take(array, item);
  else flowie_control_dashboard_json_free(item);
  return rc;
}

static int flowie_control_dashboard_group_has_children(const flowie_control_group_view_t *groups,
                                                       size_t count, const char *group_id) {
  if (!groups || !group_id) return 0;
  for (size_t index = 0u; index < count; ++index) {
    if (strcmp(groups[index].parent_group_id, group_id) == 0) return 1;
  }
  return 0;
}

static int flowie_control_dashboard_add_group_children(json_value_t *array,
                                                       const flowie_control_group_view_t *groups,
                                                       size_t count, const char *parent_group_id,
                                                       uint32_t depth, size_t *emitted) {
  int rc = SALTS_OK;
  if (!array || !groups || !parent_group_id || !emitted || depth > FLOWIE_CONTROL_GROUP_MAX_DEPTH)
    return SALTS_EINVAL;
  for (size_t index = 0u; rc == SALTS_OK && index < count; ++index) {
    if (groups[index].depth != depth || strcmp(groups[index].parent_group_id, parent_group_id) != 0)
      continue;
    rc = flowie_control_dashboard_add_group_option(
        array, &groups[index], *emitted + 1u,
        flowie_control_dashboard_group_has_children(groups, count, groups[index].group_id));
    if (rc == SALTS_OK) ++*emitted;
    if (rc == SALTS_OK && depth < FLOWIE_CONTROL_GROUP_MAX_DEPTH)
      rc = flowie_control_dashboard_add_group_children(array, groups, count, groups[index].group_id,
                                                       depth + 1u, emitted);
  }
  return rc;
}

static int
flowie_control_dashboard_add_group_options(json_value_t *model,
                                           flowie_control_management_service_t *service,
                                           const flowie_control_management_caller_t *caller) {
  flowie_control_group_view_t groups[FLOWIE_CONTROL_DASHBOARD_GROUP_SELECTOR_LIMIT];
  json_value_t *array = json_create_array();
  size_t count = 0u;
  size_t emitted = 0u;
  int has_more = 0;
  int rc;
  if (!array) return SALTS_ENOMEM;
  memset(groups, 0, sizeof(groups));
  for (size_t index = 0u; index < FLOWIE_CONTROL_DASHBOARD_GROUP_SELECTOR_LIMIT; ++index)
    groups[index].size = sizeof(groups[index]);
  rc = flowie_control_management_group_list(service, caller, NULL, groups,
                                            FLOWIE_CONTROL_DASHBOARD_GROUP_SELECTOR_LIMIT, &count,
                                            &has_more);
  if (rc == SALTS_OK && has_more) rc = SALTS_ENOSPC;
  for (size_t index = 0u; rc == SALTS_OK && index < count; ++index) {
    if (groups[index].depth != 0u || groups[index].parent_group_id[0]) continue;
    rc = flowie_control_dashboard_add_group_option(
        array, &groups[index], emitted + 1u,
        flowie_control_dashboard_group_has_children(groups, count, groups[index].group_id));
    if (rc == SALTS_OK) ++emitted;
    if (rc == SALTS_OK)
      rc = flowie_control_dashboard_add_group_children(array, groups, count, groups[index].group_id,
                                                       1u, &emitted);
  }
  if (rc == SALTS_OK && emitted != count) rc = SALTS_EPROTO;
  if (rc == SALTS_OK) rc = flowie_control_dashboard_json_take(model, "group_options", array);
  else flowie_control_dashboard_json_free(array);
  return rc;
}

static int
flowie_control_dashboard_add_user_options(json_value_t *model,
                                          flowie_control_management_service_t *service,
                                          const flowie_control_management_caller_t *caller) {
  flowie_control_user_view_t users[FLOWIE_CONTROL_PAGE_MAX];
  json_value_t *array = json_create_array();
  size_t count = 0u;
  int has_more = 0;
  int rc;
  if (!array) return SALTS_ENOMEM;
  for (size_t index = 0u; index < FLOWIE_CONTROL_PAGE_MAX; ++index)
    users[index] = (flowie_control_user_view_t)FLOWIE_CONTROL_USER_VIEW_INIT;
  rc = flowie_control_management_user_list(service, caller, NULL, users, FLOWIE_CONTROL_PAGE_MAX,
                                           &count, &has_more);
  for (size_t index = 0u; rc == SALTS_OK && index < count; ++index) {
    json_value_t *item = json_create_object();
    if (!item) {
      rc = SALTS_ENOMEM;
      break;
    }
    rc = flowie_control_dashboard_json_string(item, "principal_id", users[index].principal_id);
    if (rc == SALTS_OK)
      rc =
          flowie_control_dashboard_json_string(item, "principal_type", users[index].principal_type);
    if (rc == SALTS_OK)
      rc = flowie_control_dashboard_json_bool(item, "enabled", users[index].enabled);
    if (rc == SALTS_OK) rc = flowie_control_dashboard_json_array_take(array, item);
    else flowie_control_dashboard_json_free(item);
  }
  if (rc == SALTS_OK) rc = flowie_control_dashboard_json_take(model, "user_options", array);
  else flowie_control_dashboard_json_free(array);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_json_bool(model, "user_options_truncated", has_more);
  return rc;
}

static int
flowie_control_dashboard_add_role_options(json_value_t *model,
                                          flowie_control_management_service_t *service,
                                          const flowie_control_management_caller_t *caller) {
  flowie_control_role_view_t roles[FLOWIE_CONTROL_DASHBOARD_ROLE_SELECTOR_LIMIT];
  json_value_t *array = json_create_array();
  size_t count = 0u;
  int has_more = 0;
  int rc;
  if (!array) return SALTS_ENOMEM;
  for (size_t index = 0u; index < FLOWIE_CONTROL_DASHBOARD_ROLE_SELECTOR_LIMIT; ++index)
    roles[index] = (flowie_control_role_view_t)FLOWIE_CONTROL_ROLE_VIEW_INIT;
  rc = flowie_control_management_role_list(service, caller, NULL, roles,
                                           FLOWIE_CONTROL_DASHBOARD_ROLE_SELECTOR_LIMIT, &count,
                                           &has_more);
  for (size_t index = 0u; rc == SALTS_OK && index < count; ++index) {
    json_value_t *item = json_create_object();
    if (!item) {
      rc = SALTS_ENOMEM;
      break;
    }
    rc = flowie_control_dashboard_json_string(item, "role_id", roles[index].role_id);
    if (rc == SALTS_OK)
      rc = flowie_control_dashboard_json_bool(item, "enabled", roles[index].enabled);
    if (rc == SALTS_OK) rc = flowie_control_dashboard_json_array_take(array, item);
    else flowie_control_dashboard_json_free(item);
  }
  if (rc == SALTS_OK) rc = flowie_control_dashboard_json_take(model, "role_options", array);
  else flowie_control_dashboard_json_free(array);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_json_bool(model, "role_options_truncated", has_more);
  return rc;
}

static int flowie_control_dashboard_add_users(json_value_t *model,
                                              flowie_control_management_service_t *service,
                                              const flowie_control_management_caller_t *caller,
                                              const flowie_control_dashboard_page_t *page) {
  flowie_control_user_view_t users[FLOWIE_CONTROL_DASHBOARD_PAGE_SIZE];
  json_value_t *array = json_create_array();
  size_t count = 0u;
  int has_more = 0;
  int rc;
  if (!array) return SALTS_ENOMEM;
  for (size_t index = 0u; index < FLOWIE_CONTROL_DASHBOARD_PAGE_SIZE; ++index)
    users[index] = (flowie_control_user_view_t)FLOWIE_CONTROL_USER_VIEW_INIT;
  rc = flowie_control_management_user_list(service, caller,
                                           page->users_after[0] ? page->users_after : NULL, users,
                                           FLOWIE_CONTROL_DASHBOARD_PAGE_SIZE, &count, &has_more);
  for (size_t index = 0u; rc == SALTS_OK && index < count; ++index) {
    json_value_t *item = json_create_object();
    if (!item) {
      rc = SALTS_ENOMEM;
      break;
    }
    rc = flowie_control_dashboard_json_u64(item, "row_index", index + 1u);
    if (rc == SALTS_OK)
      rc = flowie_control_dashboard_json_string(item, "principal_id", users[index].principal_id);
    if (rc == SALTS_OK)
      rc =
          flowie_control_dashboard_json_string(item, "principal_type", users[index].principal_type);
    if (rc == SALTS_OK)
      rc = flowie_control_dashboard_json_bool(item, "enabled", users[index].enabled);
    if (rc == SALTS_OK)
      rc = flowie_control_dashboard_json_bool(item, "is_service",
                                              strcmp(users[index].principal_type, "service") == 0);
    if (rc == SALTS_OK)
      rc = flowie_control_dashboard_json_bool(item, "is_human",
                                              strcmp(users[index].principal_type, "human") == 0);
    if (rc == SALTS_OK) rc = flowie_control_dashboard_json_array_take(array, item);
    else flowie_control_dashboard_json_free(item);
  }
  if (rc == SALTS_OK) rc = flowie_control_dashboard_json_take(model, "users", array);
  else flowie_control_dashboard_json_free(array);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_add_pager(
        model, "users_pager", page, FLOWIE_CONTROL_DASHBOARD_USERS_CURSOR, count,
        has_more && count > 0u, count > 0u ? users[count - 1u].principal_id : NULL, 0u);
  return rc;
}

static int flowie_control_dashboard_add_roles(json_value_t *model,
                                              flowie_control_management_service_t *service,
                                              const flowie_control_management_caller_t *caller,
                                              const flowie_control_dashboard_page_t *page) {
  flowie_control_role_view_t roles[FLOWIE_CONTROL_DASHBOARD_PAGE_SIZE];
  json_value_t *array = json_create_array();
  size_t count = 0u;
  int has_more = 0;
  int rc;
  if (!array) return SALTS_ENOMEM;
  for (size_t index = 0u; index < FLOWIE_CONTROL_DASHBOARD_PAGE_SIZE; ++index)
    roles[index] = (flowie_control_role_view_t)FLOWIE_CONTROL_ROLE_VIEW_INIT;
  rc = flowie_control_management_role_list(service, caller,
                                           page->roles_after[0] ? page->roles_after : NULL, roles,
                                           FLOWIE_CONTROL_DASHBOARD_PAGE_SIZE, &count, &has_more);
  for (size_t index = 0u; rc == SALTS_OK && index < count; ++index) {
    json_value_t *item = json_create_object();
    if (!item) {
      rc = SALTS_ENOMEM;
      break;
    }
    rc = flowie_control_dashboard_json_u64(item, "row_index", index + 1u);
    if (rc == SALTS_OK)
      rc = flowie_control_dashboard_json_string(item, "role_id", roles[index].role_id);
    if (rc == SALTS_OK)
      rc = flowie_control_dashboard_json_bool(item, "enabled", roles[index].enabled);
    if (rc == SALTS_OK) rc = flowie_control_dashboard_json_array_take(array, item);
    else flowie_control_dashboard_json_free(item);
  }
  if (rc == SALTS_OK) rc = flowie_control_dashboard_json_take(model, "roles", array);
  else flowie_control_dashboard_json_free(array);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_add_pager(
        model, "roles_pager", page, FLOWIE_CONTROL_DASHBOARD_ROLES_CURSOR, count,
        has_more && count > 0u, count > 0u ? roles[count - 1u].role_id : NULL, 0u);
  return rc;
}

static int flowie_control_dashboard_add_rules(json_value_t *model,
                                              flowie_control_management_service_t *service,
                                              const flowie_control_management_caller_t *caller,
                                              const flowie_control_dashboard_page_t *page) {
  flowie_control_policy_subject_rule_view_t *rules = NULL;
  json_value_t *array = json_create_array();
  size_t count = 0u;
  uint64_t last_ordinal = 0u;
  int has_more = 0;
  int rc = SALTS_OK;
  if (!array) return SALTS_ENOMEM;
  rules = (flowie_control_policy_subject_rule_view_t *)calloc(FLOWIE_CONTROL_DASHBOARD_PAGE_SIZE,
                                                              sizeof(*rules));
  if (!rules) {
    flowie_control_dashboard_json_free(array);
    return SALTS_ENOMEM;
  }
  for (size_t index = 0u; index < FLOWIE_CONTROL_DASHBOARD_PAGE_SIZE; ++index)
    rules[index] =
        (flowie_control_policy_subject_rule_view_t)FLOWIE_CONTROL_POLICY_SUBJECT_RULE_VIEW_INIT;
  rc = flowie_control_management_policy_subject_rule_list(
      service, caller, FLOWIE_SECURITY_SUBJECT_ANY, page->policy_after, page->policy_has_after,
      rules, FLOWIE_CONTROL_DASHBOARD_PAGE_SIZE, &count, &has_more);
  if (rc == SALTS_ENOENT) rc = SALTS_OK;
  for (size_t index = 0u; rc == SALTS_OK && index < count; ++index) {
    const flowie_control_acl_document_t *document = &rules[index].document;
    const char *subject_kind_label = NULL;
    const char *subject_kind_value = NULL;
    char rule_document[FLOWIE_CONTROL_ACL_DOCUMENT_MAX + 1u];
    size_t rule_document_size = 0u;
    size_t expanded_topic_count = 0u;
    int uses_username = 0;
    int uses_client_id = 0;
    json_value_t *item = json_create_object();
    if (!item) {
      rc = SALTS_ENOMEM;
      break;
    }
    subject_kind_label = flowie_control_dashboard_subject_kind_label(document->subject_kind);
    subject_kind_value = flowie_control_dashboard_subject_kind_value(document->subject_kind);
    if (!subject_kind_label || !subject_kind_value) rc = SALTS_EPROTO;
    else
      rc = flowie_control_acl_format(document, rule_document, sizeof(rule_document),
                                     &rule_document_size);
    for (size_t entry = 0u; rc == SALTS_OK && entry < document->entry_count; ++entry) {
      if (document->entries[entry].alternative_count == 0u ||
          expanded_topic_count > SIZE_MAX - document->entries[entry].alternative_count) {
        rc = SALTS_EPROTO;
        break;
      }
      expanded_topic_count += document->entries[entry].alternative_count;
      uses_username |= document->entries[entry].uses_username;
      uses_client_id |= document->entries[entry].uses_client_id;
    }
    if (rc == SALTS_OK) rc = flowie_control_dashboard_json_u64(item, "row_index", index + 1u);
    if (rc == SALTS_OK)
      rc = flowie_control_dashboard_json_u64(item, "ordinal", rules[index].ordinal);
    if (rc == SALTS_OK)
      rc = flowie_control_dashboard_json_string(item, "rule_document", rule_document);
    if (rc == SALTS_OK)
      rc = flowie_control_dashboard_json_string(
          item, "connection_label",
          document->connection_effect == FLOWIE_SECURITY_ALLOW ? "Allow" : "Deny");
    if (rc == SALTS_OK)
      rc = flowie_control_dashboard_json_string(item, "subject_label", document->subject);
    if (rc == SALTS_OK)
      rc = flowie_control_dashboard_json_string(item, "subject_kind_label", subject_kind_label);
    if (rc == SALTS_OK)
      rc = flowie_control_dashboard_json_string(item, "subject_kind", subject_kind_value);
    if (rc == SALTS_OK)
      rc = flowie_control_dashboard_json_u64(item, "entry_count", document->entry_count);
    if (rc == SALTS_OK)
      rc = flowie_control_dashboard_json_u64(item, "expanded_topic_count", expanded_topic_count);
    if (rc == SALTS_OK)
      rc = flowie_control_dashboard_json_bool(item, "uses_username", uses_username);
    if (rc == SALTS_OK)
      rc = flowie_control_dashboard_json_bool(item, "uses_client_id", uses_client_id);
    if (rc == SALTS_OK) rc = flowie_control_dashboard_json_array_take(array, item);
    else flowie_control_dashboard_json_free(item);
  }
  if (count > 0u) last_ordinal = rules[count - 1u].ordinal;
  if (rc == SALTS_OK) rc = flowie_control_dashboard_json_take(model, "rules", array);
  else flowie_control_dashboard_json_free(array);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_add_pager(model, "policy_pager", page,
                                            FLOWIE_CONTROL_DASHBOARD_POLICY_CURSOR, count,
                                            has_more && count > 0u, NULL, last_ordinal);
  free(rules);
  return rc;
}

static int flowie_control_dashboard_add_audits(json_value_t *model,
                                               flowie_control_management_service_t *service,
                                               const flowie_control_management_caller_t *caller,
                                               const flowie_control_dashboard_page_t *page) {
  flowie_control_audit_view_t *audits = NULL;
  json_value_t *array = json_create_array();
  size_t count = 0u;
  int has_more = 0;
  int rc = SALTS_OK;
  if (!array) return SALTS_ENOMEM;
  audits =
      (flowie_control_audit_view_t *)calloc(FLOWIE_CONTROL_DASHBOARD_PAGE_SIZE, sizeof(*audits));
  if (!audits) {
    flowie_control_dashboard_json_free(array);
    return SALTS_ENOMEM;
  }
  for (size_t index = 0u; index < FLOWIE_CONTROL_DASHBOARD_PAGE_SIZE; ++index)
    audits[index] = (flowie_control_audit_view_t)FLOWIE_CONTROL_AUDIT_VIEW_INIT;
  rc = flowie_control_management_audit_list(service, caller, page->audit_after, audits,
                                            FLOWIE_CONTROL_DASHBOARD_PAGE_SIZE, &count, &has_more);
  for (size_t index = 0u; rc == SALTS_OK && index < count; ++index) {
    json_value_t *item = json_create_object();
    if (!item) {
      rc = SALTS_ENOMEM;
      break;
    }
    rc = flowie_control_dashboard_json_u64(item, "cursor", audits[index].revision);
    if (rc == SALTS_OK)
      rc = flowie_control_dashboard_json_string(item, "actor", audits[index].actor);
    if (rc == SALTS_OK)
      rc = flowie_control_dashboard_json_string(item, "operation", audits[index].operation);
    if (rc == SALTS_OK)
      rc = flowie_control_dashboard_json_string(item, "target_id", audits[index].target_id);
    if (rc == SALTS_OK) rc = flowie_control_dashboard_json_array_take(array, item);
    else flowie_control_dashboard_json_free(item);
  }
  if (rc == SALTS_OK) rc = flowie_control_dashboard_json_take(model, "audits", array);
  else flowie_control_dashboard_json_free(array);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_add_pager(
        model, "audit_pager", page, FLOWIE_CONTROL_DASHBOARD_AUDIT_CURSOR, count,
        has_more && count > 0u, NULL, count > 0u ? audits[count - 1u].revision : 0u);
  free(audits);
  return rc;
}

int flowie_control_dashboard_view_create(const char *resource_directory,
                                         flowie_control_dashboard_view_t **out) {
  flowie_control_dashboard_view_t *view;
  int rc;
  if (out) *out = NULL;
  if (!resource_directory || !out) return SALTS_EINVAL;
  view = (flowie_control_dashboard_view_t *)calloc(1u, sizeof(*view));
  if (!view) return SALTS_ENOMEM;
  rc = flowie_control_dashboard_compile(resource_directory, FLOWIE_CONTROL_DASHBOARD_SHELL_TEMPLATE,
                                        &view->shell_template);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_compile(
        resource_directory, FLOWIE_CONTROL_DASHBOARD_CONTENT_TEMPLATE, &view->content_template);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_compile(
        resource_directory, FLOWIE_CONTROL_DASHBOARD_ERROR_TEMPLATE, &view->error_template);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_compile(
        resource_directory, FLOWIE_CONTROL_DASHBOARD_LOGIN_TEMPLATE, &view->login_template);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_compile(
        resource_directory, FLOWIE_CONTROL_DASHBOARD_PASSWORD_TEMPLATE, &view->password_template);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_read(resource_directory, FLOWIE_CONTROL_DASHBOARD_CSS_ASSET,
                                       FLOWIE_CONTROL_DASHBOARD_ASSET_MAX, &view->css);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_read(resource_directory, FLOWIE_CONTROL_DASHBOARD_JS_ASSET,
                                       FLOWIE_CONTROL_DASHBOARD_ASSET_MAX, &view->javascript);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_read(resource_directory, FLOWIE_CONTROL_DASHBOARD_HTMX_ASSET,
                                       FLOWIE_CONTROL_DASHBOARD_ASSET_MAX, &view->htmx);
  if (rc != SALTS_OK) {
    flowie_control_dashboard_view_destroy(view);
    return rc;
  }
  *out = view;
  return SALTS_OK;
}

void flowie_control_dashboard_view_destroy(flowie_control_dashboard_view_t *view) {
  if (!view) return;
  mustache_release(view->error_template);
  mustache_release(view->password_template);
  mustache_release(view->login_template);
  mustache_release(view->content_template);
  mustache_release(view->shell_template);
  salts_fs_buf_free(&view->htmx);
  salts_fs_buf_free(&view->javascript);
  salts_fs_buf_free(&view->css);
  memset(view, 0, sizeof(*view));
  free(view);
}

int flowie_control_dashboard_view_render_shell(flowie_control_dashboard_view_t *view,
                                               const flowie_control_dashboard_page_t *page,
                                               char **html_out, size_t *html_size_out) {
  json_value_t *model = NULL;
  char *content_url = NULL;
  int rc;
  if (!view || !page) return SALTS_EINVAL;
  rc = flowie_control_dashboard_url(FLOWIE_CONTROL_DASHBOARD_CONTENT_PATH, page, &content_url);
  if (rc != SALTS_OK) return rc;
  model = json_create_object();
  if (!model) rc = SALTS_ENOMEM;
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_json_string(
        model, "page_title", flowie_control_dashboard_section_title(page->section));
  if (rc == SALTS_OK) rc = flowie_control_dashboard_json_string(model, "content_url", content_url);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_render_template(view->shell_template, model, html_out,
                                                  html_size_out);
  tstr_free(content_url);
  flowie_control_dashboard_json_free(model);
  return rc;
}

int flowie_control_dashboard_view_render_login(flowie_control_dashboard_view_t *view,
                                               int group_mode, int show_error, char **html_out,
                                               size_t *html_size_out) {
  json_value_t *model;
  int rc;
  if (!view || (group_mode != 0 && group_mode != 1) || (show_error != 0 && show_error != 1))
    return SALTS_EINVAL;
  model = json_create_object();
  if (!model) return SALTS_ENOMEM;
  rc = flowie_control_dashboard_json_bool(model, "system_mode", !group_mode);
  if (rc == SALTS_OK) rc = flowie_control_dashboard_json_bool(model, "group_mode", group_mode);
  if (rc == SALTS_OK) rc = flowie_control_dashboard_json_bool(model, "error", show_error);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_render_template(view->login_template, model, html_out,
                                                  html_size_out);
  flowie_control_dashboard_json_free(model);
  return rc;
}

int flowie_control_dashboard_view_render_password(
    flowie_control_dashboard_view_t *view,
    const char csrf_token[FLOWIE_CONTROL_DASHBOARD_CSRF_SIZE + 1u], char **html_out,
    size_t *html_size_out) {
  json_value_t *model;
  int rc;
  if (!view || !csrf_token) return SALTS_EINVAL;
  model = json_create_object();
  if (!model) return SALTS_ENOMEM;
  rc = flowie_control_dashboard_json_string(model, "csrf", csrf_token);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_render_template(view->password_template, model, html_out,
                                                  html_size_out);
  flowie_control_dashboard_json_free(model);
  return rc;
}

int flowie_control_dashboard_view_render_content(
    flowie_control_dashboard_view_t *view, flowie_control_management_service_t *service,
    const flowie_control_management_caller_t *authority_caller,
    const flowie_control_management_caller_t *caller,
    const char csrf_token[FLOWIE_CONTROL_DASHBOARD_CSRF_SIZE + 1u], const char *rpc_path,
    const flowie_control_dashboard_page_t *page,
    const flowie_control_dashboard_action_result_t *action_result, char **html_out,
    size_t *html_size_out) {
  flowie_control_management_status_t status = FLOWIE_CONTROL_MANAGEMENT_STATUS_INIT;
  json_value_t *model = NULL;
  char *action_url = NULL;
  char *overview_url = NULL;
  char *users_url = NULL;
  char *groups_url = NULL;
  char *roles_url = NULL;
  char *acls_url = NULL;
  char *audit_url = NULL;
  char *integration_url = NULL;
  int can_create_domain;
  int can_user_admin;
  int can_security_admin;
  int can_policy_admin;
  int can_audit_read;
  int show_overview;
  int show_users;
  int show_groups;
  int show_roles;
  int show_acls;
  int show_audit;
  int show_integration;
  int is_platform_workspace;
  int is_domain_workspace;
  int rc;
  if (!view || !service || !authority_caller || !authority_caller->domain_id || !caller ||
      !caller->domain_id || !csrf_token || !rpc_path || !page)
    return SALTS_EINVAL;
  is_platform_workspace = strcmp(caller->domain_id, FLOWIE_CONTROL_MANAGEMENT_SYSTEM_DOMAIN) == 0;
  is_domain_workspace = !is_platform_workspace;
  can_create_domain =
      is_platform_workspace && (caller->permissions & FLOWIE_CONTROL_MANAGEMENT_SYSTEM_ADMIN) != 0u;
  can_user_admin = is_domain_workspace &&
                   (caller->permissions & (FLOWIE_CONTROL_MANAGEMENT_USER_ADMIN |
                                           FLOWIE_CONTROL_MANAGEMENT_SECURITY_ADMIN)) != 0u;
  can_security_admin =
      is_domain_workspace && (caller->permissions & FLOWIE_CONTROL_MANAGEMENT_SECURITY_ADMIN) != 0u;
  can_policy_admin = is_domain_workspace &&
                     (caller->permissions & (FLOWIE_CONTROL_MANAGEMENT_POLICY_ADMIN |
                                             FLOWIE_CONTROL_MANAGEMENT_SECURITY_ADMIN)) != 0u;
  can_audit_read = can_security_admin;
  if (is_platform_workspace && page->section != FLOWIE_CONTROL_DASHBOARD_SECTION_ALL &&
      page->section != FLOWIE_CONTROL_DASHBOARD_SECTION_OVERVIEW &&
      page->section != FLOWIE_CONTROL_DASHBOARD_SECTION_INTEGRATION)
    return SALTS_EPERM;
  if (page->section == FLOWIE_CONTROL_DASHBOARD_SECTION_AUDIT && !can_audit_read)
    return SALTS_EPERM;
  show_overview = page->section == FLOWIE_CONTROL_DASHBOARD_SECTION_ALL ||
                  page->section == FLOWIE_CONTROL_DASHBOARD_SECTION_OVERVIEW;
  show_users = is_domain_workspace && (page->section == FLOWIE_CONTROL_DASHBOARD_SECTION_ALL ||
                                       page->section == FLOWIE_CONTROL_DASHBOARD_SECTION_USERS);
  show_groups = is_domain_workspace && (page->section == FLOWIE_CONTROL_DASHBOARD_SECTION_ALL ||
                                        page->section == FLOWIE_CONTROL_DASHBOARD_SECTION_GROUPS);
  show_roles = is_domain_workspace && (page->section == FLOWIE_CONTROL_DASHBOARD_SECTION_ALL ||
                                       page->section == FLOWIE_CONTROL_DASHBOARD_SECTION_ROLES);
  show_acls = is_domain_workspace && (page->section == FLOWIE_CONTROL_DASHBOARD_SECTION_ALL ||
                                      page->section == FLOWIE_CONTROL_DASHBOARD_SECTION_ACLS);
  show_audit = can_audit_read && (page->section == FLOWIE_CONTROL_DASHBOARD_SECTION_ALL ||
                                  page->section == FLOWIE_CONTROL_DASHBOARD_SECTION_AUDIT);
  show_integration = page->section == FLOWIE_CONTROL_DASHBOARD_SECTION_INTEGRATION;
  rc = flowie_control_management_system_status(service, caller, &status);
  if (rc != SALTS_OK) return rc;
  rc = flowie_control_dashboard_url(FLOWIE_CONTROL_DASHBOARD_ACTION_PATH, page, &action_url);
  if (rc == SALTS_OK)
    rc =
        flowie_control_dashboard_navigation_url(FLOWIE_CONTROL_DASHBOARD_PATH, page, &overview_url);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_navigation_url(FLOWIE_CONTROL_DASHBOARD_USERS_PATH, page,
                                                 &users_url);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_navigation_url(FLOWIE_CONTROL_DASHBOARD_GROUPS_PATH, page,
                                                 &groups_url);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_navigation_url(FLOWIE_CONTROL_DASHBOARD_ROLES_PATH, page,
                                                 &roles_url);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_navigation_url(FLOWIE_CONTROL_DASHBOARD_ACLS_PATH, page,
                                                 &acls_url);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_navigation_url(FLOWIE_CONTROL_DASHBOARD_AUDIT_PATH, page,
                                                 &audit_url);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_navigation_url(FLOWIE_CONTROL_DASHBOARD_INTEGRATION_PATH, page,
                                                 &integration_url);
  if (rc != SALTS_OK) goto done;
  model = json_create_object();
  if (!model) rc = SALTS_ENOMEM;
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_json_string(model, "domain_id", caller->domain_id);
  if (rc == SALTS_OK) rc = flowie_control_dashboard_json_string(model, "actor", caller->actor);
  if (rc == SALTS_OK) rc = flowie_control_dashboard_json_string(model, "csrf", csrf_token);
  if (rc == SALTS_OK) rc = flowie_control_dashboard_json_string(model, "rpc_path", rpc_path);
  if (rc == SALTS_OK) rc = flowie_control_dashboard_json_string(model, "action_url", action_url);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_json_u64(model, "policy_version", status.policy.policy_version);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_json_u64(model, "draft_rule_count",
                                           status.policy.draft_rule_count);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_json_u64(model, "published_rule_count",
                                           status.policy.published_rule_count);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_json_bool(model, "can_user_admin", can_user_admin);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_json_bool(model, "can_security_admin", can_security_admin);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_json_bool(model, "can_policy_admin", can_policy_admin);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_json_bool(model, "can_manage_access",
                                            can_user_admin || can_security_admin);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_json_bool(model, "can_audit_read", can_audit_read);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_json_bool(model, "can_create_domain", can_create_domain);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_json_bool(model, "is_platform_workspace", is_platform_workspace);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_json_bool(model, "is_domain_workspace", is_domain_workspace);
  if (rc == SALTS_OK && can_create_domain)
    rc = flowie_control_dashboard_add_domains(model, service, authority_caller, caller);
  if (rc == SALTS_OK && action_result &&
      action_result->kind == FLOWIE_CONTROL_DASHBOARD_ACTION_CREDENTIAL_ISSUED) {
    if (strcmp(action_result->domain_id, caller->domain_id) != 0) rc = SALTS_EPROTO;
    else {
      rc = flowie_control_dashboard_json_bool(model, "credential_issued", 1);
      if (rc == SALTS_OK)
        rc = flowie_control_dashboard_json_string(model, "credential_domain",
                                                  action_result->domain_id);
      if (rc == SALTS_OK)
        rc = flowie_control_dashboard_json_string(model, "credential_principal",
                                                  action_result->principal_id);
      if (rc == SALTS_OK)
        rc = flowie_control_dashboard_json_string(model, "credential_token", action_result->token);
    }
  }
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_json_bool(model, "show_overview", show_overview);
  if (rc == SALTS_OK) rc = flowie_control_dashboard_json_bool(model, "show_users", show_users);
  if (rc == SALTS_OK) rc = flowie_control_dashboard_json_bool(model, "show_groups", show_groups);
  if (rc == SALTS_OK) rc = flowie_control_dashboard_json_bool(model, "show_roles", show_roles);
  if (rc == SALTS_OK) rc = flowie_control_dashboard_json_bool(model, "show_acls", show_acls);
  if (rc == SALTS_OK) rc = flowie_control_dashboard_json_bool(model, "show_audit", show_audit);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_json_bool(model, "show_integration", show_integration);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_json_bool(
        model, "is_overview", page->section == FLOWIE_CONTROL_DASHBOARD_SECTION_OVERVIEW);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_json_bool(
        model, "is_users", page->section == FLOWIE_CONTROL_DASHBOARD_SECTION_USERS);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_json_bool(
        model, "is_groups", page->section == FLOWIE_CONTROL_DASHBOARD_SECTION_GROUPS);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_json_bool(
        model, "is_roles", page->section == FLOWIE_CONTROL_DASHBOARD_SECTION_ROLES);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_json_bool(model, "is_acls",
                                            page->section == FLOWIE_CONTROL_DASHBOARD_SECTION_ACLS);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_json_bool(
        model, "is_audit", page->section == FLOWIE_CONTROL_DASHBOARD_SECTION_AUDIT);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_json_bool(
        model, "is_integration", page->section == FLOWIE_CONTROL_DASHBOARD_SECTION_INTEGRATION);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_json_string(model, "overview_path", overview_url);
  if (rc == SALTS_OK) rc = flowie_control_dashboard_json_string(model, "users_path", users_url);
  if (rc == SALTS_OK) rc = flowie_control_dashboard_json_string(model, "groups_path", groups_url);
  if (rc == SALTS_OK) rc = flowie_control_dashboard_json_string(model, "roles_path", roles_url);
  if (rc == SALTS_OK) rc = flowie_control_dashboard_json_string(model, "acls_path", acls_url);
  if (rc == SALTS_OK) rc = flowie_control_dashboard_json_string(model, "audit_path", audit_url);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_json_string(model, "integration_path", integration_url);
  if (rc == SALTS_OK && (show_groups || show_acls || (show_users && can_user_admin)))
    rc = flowie_control_dashboard_add_group_options(model, service, caller);
  if (rc == SALTS_OK &&
      (show_acls || ((show_groups || show_roles) && (can_user_admin || can_security_admin))))
    rc = flowie_control_dashboard_add_user_options(model, service, caller);
  if (rc == SALTS_OK && ((show_users && can_security_admin) || show_acls))
    rc = flowie_control_dashboard_add_role_options(model, service, caller);
  if (rc == SALTS_OK && show_users)
    rc = flowie_control_dashboard_add_users(model, service, caller, page);
  if (rc == SALTS_OK && show_roles)
    rc = flowie_control_dashboard_add_roles(model, service, caller, page);
  if (rc == SALTS_OK && show_acls)
    rc = flowie_control_dashboard_add_rules(model, service, caller, page);
  if (rc == SALTS_OK && show_audit)
    rc = flowie_control_dashboard_add_audits(model, service, caller, page);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_render_template(view->content_template, model, html_out,
                                                  html_size_out);
done:
  if (model) {
    json_value_t *secret = json_object_get(model, "credential_token");
    const char *secret_text = secret ? json_string(secret) : NULL;
    if (secret_text) crypto_wipe((void *)secret_text, json_string_len(secret));
  }
  tstr_free(action_url);
  tstr_free(overview_url);
  tstr_free(users_url);
  tstr_free(groups_url);
  tstr_free(roles_url);
  tstr_free(acls_url);
  tstr_free(audit_url);
  tstr_free(integration_url);
  flowie_control_dashboard_json_free(model);
  return rc;
}

int flowie_control_dashboard_view_render_error(flowie_control_dashboard_view_t *view,
                                               const char *message, char **html_out,
                                               size_t *html_size_out) {
  json_value_t *model;
  int rc;
  if (!view || !message) return SALTS_EINVAL;
  model = json_create_object();
  if (!model) return SALTS_ENOMEM;
  rc = flowie_control_dashboard_json_string(model, "message", message);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_render_template(view->error_template, model, html_out,
                                                  html_size_out);
  flowie_control_dashboard_json_free(model);
  return rc;
}

int flowie_control_dashboard_view_asset(const flowie_control_dashboard_view_t *view,
                                        flowie_control_dashboard_asset_t asset,
                                        const void **data_out, size_t *size_out) {
  const salts_fs_buf_t *resource;
  if (data_out) *data_out = NULL;
  if (size_out) *size_out = 0u;
  if (!view || !data_out || !size_out) return SALTS_EINVAL;
  switch (asset) {
  case FLOWIE_CONTROL_DASHBOARD_ASSET_CSS:
    resource = &view->css;
    break;
  case FLOWIE_CONTROL_DASHBOARD_ASSET_JS:
    resource = &view->javascript;
    break;
  case FLOWIE_CONTROL_DASHBOARD_ASSET_HTMX:
    resource = &view->htmx;
    break;
  default:
    return SALTS_EINVAL;
  }
  if (!resource->base || resource->len == 0u) return SALTS_EPROTO;
  *data_out = resource->base;
  *size_out = resource->len;
  return SALTS_OK;
}
