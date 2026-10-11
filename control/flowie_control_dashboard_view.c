#include "flowie_control_acl_internal.h"
#include "flowie_control_dashboard_view_internal.h"

#include "cmeta_cmeta_data.h"
#include "cmeta_error.h"
#include "cmeta_fs.h"
#include "cmeta_thread.h"
#include "fmt.h"
#include "monocypher.h"
#include "tlog.h"
#include "tstr.h"
#include <chttp_app/app.h>
#include <cmeta/data_reflect.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flowie_control_dashboard_model_internal.h"

static const char FLOWIE_CONTROL_DASHBOARD_SHELL_TEMPLATE[] = "templates/dashboard.html";
static const char FLOWIE_CONTROL_DASHBOARD_CONTENT_TEMPLATE[] = "templates/dashboard_content.html";
static const char FLOWIE_CONTROL_DASHBOARD_ERROR_TEMPLATE[] = "templates/dashboard_error.html";
static const char FLOWIE_CONTROL_DASHBOARD_LOGIN_TEMPLATE[] = "templates/login.html";
static const char FLOWIE_CONTROL_DASHBOARD_PASSWORD_TEMPLATE[] = "templates/password.html";
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
  chttp_web_renderer renderer;
  cmeta_mutex_t render_lock;
  cmeta_fs_buf_t css;
  cmeta_fs_buf_t javascript;
  cmeta_fs_buf_t htmx;
};

/* Canonical Core borrowed-string operations; VIEW reflection grants no value
 * ownership. Model bytes stay live until the synchronous App render returns. */
static const cmeta_data_buffer_shape FLOWIE_CONTROL_DASHBOARD_STRING_SHAPE = {
    .ownership = CMETA_DATA_BUFFER_BORROWED};
static const cmeta_data_desc FLOWIE_CONTROL_DASHBOARD_STRING_DATA = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "flowie.control.dashboard.BorrowedString",
    .display_name = "Dashboard borrowed string",
    .kind = CMETA_DATA_STRING,
    .storage_type = &cmeta_vstr_cmeta_type,
    .shape = &FLOWIE_CONTROL_DASHBOARD_STRING_SHAPE,
    .buffer_ops = &cmeta_vstr_cmeta_buffer_ops};

typedef struct flowie_control_dashboard_shell_model {
  vstr page_title;
  vstr content_url;
} flowie_control_dashboard_shell_model;
cmeta_reflect_data(flowie_control_dashboard_shell_model, "flowie.control.dashboard.Shell",
                   cmeta_data_field(vstr, page_title, &FLOWIE_CONTROL_DASHBOARD_STRING_DATA,
                                    &cmeta_vstr_cmeta_type)
                       cmeta_data_field(vstr, content_url, &FLOWIE_CONTROL_DASHBOARD_STRING_DATA,
                                        &cmeta_vstr_cmeta_type));

typedef struct flowie_control_dashboard_login_model {
  bool system_mode;
  bool group_mode;
  bool error;
} flowie_control_dashboard_login_model;
cmeta_reflect_data(flowie_control_dashboard_login_model, "flowie.control.dashboard.Login",
                   cmeta_field(bool, system_mode) cmeta_field(bool, group_mode)
                       cmeta_field(bool, error));

typedef struct flowie_control_dashboard_password_model {
  vstr csrf;
} flowie_control_dashboard_password_model;
cmeta_reflect_data(flowie_control_dashboard_password_model, "flowie.control.dashboard.Password",
                   cmeta_data_field(vstr, csrf, &FLOWIE_CONTROL_DASHBOARD_STRING_DATA,
                                    &cmeta_vstr_cmeta_type));

typedef struct flowie_control_dashboard_error_model {
  vstr message;
} flowie_control_dashboard_error_model;
cmeta_reflect_data(flowie_control_dashboard_error_model, "flowie.control.dashboard.Error",
                   cmeta_data_field(vstr, message, &FLOWIE_CONTROL_DASHBOARD_STRING_DATA,
                                    &cmeta_vstr_cmeta_type));

static int flowie_control_dashboard_app_status(chttp_web_status status) {
  switch (status) {
  case CHTTP_WEB_OK:
    return SALTS_OK;
  case CHTTP_WEB_OUT_OF_MEMORY:
    return SALTS_ENOMEM;
  case CHTTP_WEB_CAPACITY:
    return SALTS_EMSGSIZE;
  case CHTTP_WEB_INVALID_ARGUMENT:
    return SALTS_EINVAL;
  default:
    return SALTS_EPROTO;
  }
}

static int flowie_control_dashboard_app_render(flowie_control_dashboard_view_t *view,
                                               const char *name, const cmeta_data_desc *descriptor,
                                               const void *model, char **html_out,
                                               size_t *html_size_out) {
  chttp_web_status status;
  if (html_out) *html_out = NULL;
  if (html_size_out) *html_size_out = 0u;
  if (!view || !name || !descriptor || !model || !html_out || !html_size_out) return SALTS_EINVAL;
  /* App's renderer is non-reentrant; request/model storage remains per worker. */
  cmeta_mutex_lock(&view->render_lock);
  status =
      chttp_web_render(&view->renderer, name, descriptor, model, html_out, html_size_out, NULL);
  cmeta_mutex_unlock(&view->render_lock);
  return flowie_control_dashboard_app_status(status);
}

static int flowie_control_dashboard_read(const char *resource_directory, const char *relative_path,
                                         size_t maximum, cmeta_fs_buf_t *out) {
  char path[FLOWIE_CONTROL_DASHBOARD_RESOURCE_PATH_MAX];
  int rc;
  if (!resource_directory || !relative_path || !out) return SALTS_EINVAL;
  memset(out, 0, sizeof(*out));
  rc = cmeta_fs_path_join(path, sizeof(path), resource_directory, relative_path);
  if (rc != SALTS_OK) return rc;
  rc = cmeta_fs_read_file(path, out);
  if (rc != SALTS_OK) return rc;
  if (!out->base || out->len == 0u || out->len > maximum) {
    cmeta_fs_buf_free(out);
    return SALTS_EPROTO;
  }
  return SALTS_OK;
}

static int flowie_control_dashboard_app_init(flowie_control_dashboard_view_t *view,
                                             const char *resource_directory) {
  const char *names[] = {
      FLOWIE_CONTROL_DASHBOARD_SHELL_TEMPLATE,   FLOWIE_CONTROL_DASHBOARD_ERROR_TEMPLATE,
      FLOWIE_CONTROL_DASHBOARD_LOGIN_TEMPLATE,   FLOWIE_CONTROL_DASHBOARD_PASSWORD_TEMPLATE,
      FLOWIE_CONTROL_DASHBOARD_CONTENT_TEMPLATE, "templates/dashboard_overview.html",
      "templates/dashboard_integration.html",    "templates/dashboard_users.html",
      "templates/dashboard_groups.html",         "templates/dashboard_roles.html",
      "templates/dashboard_acls.html",           "templates/dashboard_audit.html"};
  enum { TEMPLATE_COUNT = sizeof(names) / sizeof(names[0]) };
  cmeta_fs_buf_t sources[TEMPLATE_COUNT] = {0};
  chttp_web_template templates[TEMPLATE_COUNT] = {0};
  chttp_web_renderer_config config = CHTTP_WEB_RENDERER_CONFIG_INIT;
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  int rc = SALTS_OK;
  if (!cmeta_data_desc_valid(cmeta_reflected_data(flowie_control_dashboard_shell_model)) ||
      !cmeta_data_desc_valid(cmeta_reflected_data(flowie_control_dashboard_login_model)) ||
      !cmeta_data_desc_valid(cmeta_reflected_data(flowie_control_dashboard_password_model)) ||
      !cmeta_data_desc_valid(cmeta_reflected_data(flowie_control_dashboard_error_model)))
    return SALTS_EPROTO;
  if (!cmeta_data_desc_valid(cmeta_reflected_data(flowie_control_dashboard_content_model)))
    return SALTS_EPROTO;
  config.max_template_bytes = FLOWIE_CONTROL_DASHBOARD_TEMPLATE_MAX;
  config.max_output_bytes = FLOWIE_CONTROL_DASHBOARD_HTML_MAX;
  for (size_t index = 0u; rc == SALTS_OK && index < TEMPLATE_COUNT; ++index) {
    rc = flowie_control_dashboard_read(resource_directory, names[index],
                                       FLOWIE_CONTROL_DASHBOARD_TEMPLATE_MAX, &sources[index]);
    if (rc == SALTS_OK)
      templates[index] =
          (chttp_web_template){names[index], sources[index].base, sources[index].len};
  }
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_app_status(
        chttp_web_renderer_init(&view->renderer, templates, TEMPLATE_COUNT, &config, &error));
  if (error.status != CHTTP_WEB_OK)
    SALTS_LOG_ERRORF(tlog_get_default(), "flowie.control.dashboard",
                     "template-init status={} native={} template={} offset={} message={}",
                     (int)error.status, error.native_status, error.template_name, error.offset,
                     error.message);
  /* App copies the fixed bundle. No request-controlled template loading. */
  for (size_t index = 0u; index < TEMPLATE_COUNT; ++index)
    cmeta_fs_buf_free(&sources[index]);
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
flowie_control_dashboard_add_domains(flowie_control_dashboard_content_model *model,
                                     flowie_control_management_service_t *service,
                                     const flowie_control_management_caller_t *authority_caller,
                                     const flowie_control_management_caller_t *scoped_caller) {
  flowie_control_domain_view_t roots[FLOWIE_CONTROL_DASHBOARD_DOMAIN_LIMIT];
  flowie_control_dashboard_domain_model *array = model->domains_storage;
  size_t count = 0u;
  int has_more = 0;
  int rc;
  model->collections.domains = (chttp_web_sequence_view){
      array, 0u, sizeof(*array), cmeta_reflected_data(flowie_control_dashboard_domain_model)};
  for (size_t index = 0u; index < FLOWIE_CONTROL_DASHBOARD_DOMAIN_LIMIT; ++index)
    roots[index] = (flowie_control_domain_view_t)FLOWIE_CONTROL_DOMAIN_VIEW_INIT;
  rc = flowie_control_management_domain_list(service, authority_caller, NULL, roots,
                                             FLOWIE_CONTROL_DASHBOARD_DOMAIN_LIMIT, &count,
                                             &has_more);
  (void)has_more;
  for (size_t index = 0u; rc == SALTS_OK && index < count; ++index) {
    flowie_control_dashboard_domain_model *item = &array[model->collections.domains.count];
    if (strcmp(roots[index].domain_id, FLOWIE_CONTROL_MANAGEMENT_SYSTEM_DOMAIN) == 0) {
      continue;
    }

    rc = flowie_control_dashboard_model_string(&item->domain_id, roots[index].domain_id);
    if (rc == SALTS_OK)
      item->selected = (strcmp(roots[index].domain_id, scoped_caller->domain_id) == 0) != 0;
    if (rc == SALTS_OK) ++model->collections.domains.count;
  }

  return rc;
}

static int flowie_control_dashboard_add_pager(flowie_control_dashboard_pager_model *pager,
                                              const flowie_control_dashboard_page_t *page,
                                              flowie_control_dashboard_cursor_kind_t kind,
                                              size_t count, int has_more, const char *next_text,
                                              uint64_t next_number) {
  flowie_control_dashboard_page_t target = *page;
  char *url = NULL;
  int has_first = flowie_control_dashboard_page_has_cursor(page, kind);
  int rc = SALTS_OK;
  if (!pager) return SALTS_EINVAL;
  pager->count = count;
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_url(FLOWIE_CONTROL_DASHBOARD_CONTENT_PATH, page, &url);
  if (rc == SALTS_OK) rc = flowie_control_dashboard_model_string(&pager->refresh_url, url);
  tstr_free(url);
  url = NULL;
  target = *page;
  flowie_control_dashboard_page_clear_cursor(&target, kind);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_url(FLOWIE_CONTROL_DASHBOARD_CONTENT_PATH, &target, &url);
  if (rc == SALTS_OK) rc = flowie_control_dashboard_model_string(&pager->query_url, url);
  if (rc == SALTS_OK) pager->first = (has_first) != 0;
  if (rc == SALTS_OK && has_first)
    rc = flowie_control_dashboard_model_string(&pager->first_url, url);
  tstr_free(url);
  url = NULL;
  if (rc == SALTS_OK) pager->more = (has_more) != 0;
  if (rc == SALTS_OK && has_more) {
    target = *page;
    rc = flowie_control_dashboard_page_set_cursor(&target, kind, next_text, next_number);
    if (rc == SALTS_OK)
      rc = flowie_control_dashboard_url(FLOWIE_CONTROL_DASHBOARD_CONTENT_PATH, &target, &url);
    if (rc == SALTS_OK) rc = flowie_control_dashboard_model_string(&pager->more_url, url);
    tstr_free(url);
  }
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

static int flowie_control_dashboard_add_group_option(flowie_control_dashboard_content_model *model,
                                                     const flowie_control_group_view_t *group,
                                                     size_t row_index, int has_children) {
  char label[FLOWIE_CONTROL_DASHBOARD_GROUP_LABEL_MAX];
  flowie_control_dashboard_group_model *item;
  int rc;
  if (!model || !group) return SALTS_EINVAL;
  rc = flowie_control_dashboard_group_label(group, label);
  if (rc != SALTS_OK) return rc;
  if (model->collections.group_options.count >= FLOWIE_CONTROL_DASHBOARD_GROUP_SELECTOR_LIMIT)
    return SALTS_ENOBUFS;
  item = &model->group_options_storage[model->collections.group_options.count];
  item->row_index = row_index;
  if (rc == SALTS_OK) rc = flowie_control_dashboard_model_string(&item->group_id, group->group_id);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_model_string(&item->parent_group_id, group->parent_group_id);
  if (rc == SALTS_OK) rc = flowie_control_dashboard_model_string(&item->tree_label, label);
  if (rc == SALTS_OK) item->depth = group->depth;
  if (rc == SALTS_OK) item->aria_level = (uint64_t)group->depth + 1u;
  if (rc == SALTS_OK) item->enabled = (group->enabled) != 0;
  if (rc == SALTS_OK) item->is_root = (0) != 0;
  if (rc == SALTS_OK) item->member_allowed = (group->enabled) != 0;
  if (rc == SALTS_OK) item->delete_candidate = (!has_children) != 0;
  if (rc == SALTS_OK) item->add_disabled = (!group->enabled) != 0;
  if (rc == SALTS_OK) item->remove_disabled = (0) != 0;
  if (rc == SALTS_OK)
    item->parent_disabled =
        (!group->enabled || group->depth >= FLOWIE_CONTROL_GROUP_MAX_DEPTH) != 0;
  if (rc == SALTS_OK) ++model->collections.group_options.count;
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

static int flowie_control_dashboard_add_group_children(
    flowie_control_dashboard_content_model *model, const flowie_control_group_view_t *groups,
    size_t count, const char *parent_group_id, uint32_t depth, size_t *emitted) {
  int rc = SALTS_OK;
  if (!model || !groups || !parent_group_id || !emitted || depth > FLOWIE_CONTROL_GROUP_MAX_DEPTH)
    return SALTS_EINVAL;
  for (size_t index = 0u; rc == SALTS_OK && index < count; ++index) {
    if (groups[index].depth != depth || strcmp(groups[index].parent_group_id, parent_group_id) != 0)
      continue;
    rc = flowie_control_dashboard_add_group_option(
        model, &groups[index], *emitted + 1u,
        flowie_control_dashboard_group_has_children(groups, count, groups[index].group_id));
    if (rc == SALTS_OK) ++*emitted;
    if (rc == SALTS_OK && depth < FLOWIE_CONTROL_GROUP_MAX_DEPTH)
      rc = flowie_control_dashboard_add_group_children(model, groups, count, groups[index].group_id,
                                                       depth + 1u, emitted);
  }
  return rc;
}

static int
flowie_control_dashboard_add_group_options(flowie_control_dashboard_content_model *model,
                                           flowie_control_management_service_t *service,
                                           const flowie_control_management_caller_t *caller) {
  flowie_control_group_view_t groups[FLOWIE_CONTROL_DASHBOARD_GROUP_SELECTOR_LIMIT];
  flowie_control_dashboard_group_model *array = model->group_options_storage;
  size_t count = 0u;
  size_t emitted = 0u;
  int has_more = 0;
  int rc;
  model->collections.group_options = (chttp_web_sequence_view){
      array, 0u, sizeof(*array), cmeta_reflected_data(flowie_control_dashboard_group_model)};
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
        model, &groups[index], emitted + 1u,
        flowie_control_dashboard_group_has_children(groups, count, groups[index].group_id));
    if (rc == SALTS_OK) ++emitted;
    if (rc == SALTS_OK)
      rc = flowie_control_dashboard_add_group_children(model, groups, count, groups[index].group_id,
                                                       1u, &emitted);
  }
  if (rc == SALTS_OK && emitted != count) rc = SALTS_EPROTO;

  return rc;
}

static int
flowie_control_dashboard_add_user_options(flowie_control_dashboard_content_model *model,
                                          flowie_control_management_service_t *service,
                                          const flowie_control_management_caller_t *caller) {
  flowie_control_user_view_t users[FLOWIE_CONTROL_PAGE_MAX];
  flowie_control_dashboard_user_model *array = model->user_options_storage;
  size_t count = 0u;
  int has_more = 0;
  int rc;
  model->collections.user_options = (chttp_web_sequence_view){
      array, 0u, sizeof(*array), cmeta_reflected_data(flowie_control_dashboard_user_model)};
  for (size_t index = 0u; index < FLOWIE_CONTROL_PAGE_MAX; ++index)
    users[index] = (flowie_control_user_view_t)FLOWIE_CONTROL_USER_VIEW_INIT;
  rc = flowie_control_management_user_list(service, caller, NULL, users, FLOWIE_CONTROL_PAGE_MAX,
                                           &count, &has_more);
  for (size_t index = 0u; rc == SALTS_OK && index < count; ++index) {
    flowie_control_dashboard_user_model *item = &array[model->collections.user_options.count];

    rc = flowie_control_dashboard_model_string(&item->principal_id, users[index].principal_id);
    if (rc == SALTS_OK)
      rc =
          flowie_control_dashboard_model_string(&item->principal_type, users[index].principal_type);
    if (rc == SALTS_OK) item->enabled = (users[index].enabled) != 0;
    if (rc == SALTS_OK) ++model->collections.user_options.count;
  }

  if (rc == SALTS_OK) model->capabilities.user_options_truncated = (has_more) != 0;
  return rc;
}

static int
flowie_control_dashboard_add_role_options(flowie_control_dashboard_content_model *model,
                                          flowie_control_management_service_t *service,
                                          const flowie_control_management_caller_t *caller) {
  flowie_control_role_view_t roles[FLOWIE_CONTROL_DASHBOARD_ROLE_SELECTOR_LIMIT];
  flowie_control_dashboard_role_model *array = model->role_options_storage;
  size_t count = 0u;
  int has_more = 0;
  int rc;
  model->collections.role_options = (chttp_web_sequence_view){
      array, 0u, sizeof(*array), cmeta_reflected_data(flowie_control_dashboard_role_model)};
  for (size_t index = 0u; index < FLOWIE_CONTROL_DASHBOARD_ROLE_SELECTOR_LIMIT; ++index)
    roles[index] = (flowie_control_role_view_t)FLOWIE_CONTROL_ROLE_VIEW_INIT;
  rc = flowie_control_management_role_list(service, caller, NULL, roles,
                                           FLOWIE_CONTROL_DASHBOARD_ROLE_SELECTOR_LIMIT, &count,
                                           &has_more);
  for (size_t index = 0u; rc == SALTS_OK && index < count; ++index) {
    flowie_control_dashboard_role_model *item = &array[model->collections.role_options.count];

    rc = flowie_control_dashboard_model_string(&item->role_id, roles[index].role_id);
    if (rc == SALTS_OK) item->enabled = (roles[index].enabled) != 0;
    if (rc == SALTS_OK) ++model->collections.role_options.count;
  }

  if (rc == SALTS_OK) model->capabilities.role_options_truncated = (has_more) != 0;
  return rc;
}

static int flowie_control_dashboard_add_users(flowie_control_dashboard_content_model *model,
                                              flowie_control_management_service_t *service,
                                              const flowie_control_management_caller_t *caller,
                                              const flowie_control_dashboard_page_t *page) {
  flowie_control_user_view_t users[FLOWIE_CONTROL_DASHBOARD_PAGE_SIZE];
  flowie_control_dashboard_user_model *array = model->users_storage;
  size_t count = 0u;
  int has_more = 0;
  int rc;
  model->collections.users = (chttp_web_sequence_view){
      array, 0u, sizeof(*array), cmeta_reflected_data(flowie_control_dashboard_user_model)};
  for (size_t index = 0u; index < FLOWIE_CONTROL_DASHBOARD_PAGE_SIZE; ++index)
    users[index] = (flowie_control_user_view_t)FLOWIE_CONTROL_USER_VIEW_INIT;
  rc = flowie_control_management_user_list(service, caller,
                                           page->users_after[0] ? page->users_after : NULL, users,
                                           FLOWIE_CONTROL_DASHBOARD_PAGE_SIZE, &count, &has_more);
  for (size_t index = 0u; rc == SALTS_OK && index < count; ++index) {
    flowie_control_dashboard_user_model *item = &array[model->collections.users.count];

    item->row_index = index + 1u;
    if (rc == SALTS_OK)
      rc = flowie_control_dashboard_model_string(&item->principal_id, users[index].principal_id);
    if (rc == SALTS_OK)
      rc =
          flowie_control_dashboard_model_string(&item->principal_type, users[index].principal_type);
    if (rc == SALTS_OK) item->enabled = (users[index].enabled) != 0;
    if (rc == SALTS_OK)
      item->is_service = (strcmp(users[index].principal_type, "service") == 0) != 0;
    if (rc == SALTS_OK) item->is_human = (strcmp(users[index].principal_type, "human") == 0) != 0;
    if (rc == SALTS_OK) ++model->collections.users.count;
  }

  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_add_pager(
        &model->pagination.users_pager, page, FLOWIE_CONTROL_DASHBOARD_USERS_CURSOR, count,
        has_more && count > 0u, count > 0u ? users[count - 1u].principal_id : NULL, 0u);
  return rc;
}

static int flowie_control_dashboard_add_roles(flowie_control_dashboard_content_model *model,
                                              flowie_control_management_service_t *service,
                                              const flowie_control_management_caller_t *caller,
                                              const flowie_control_dashboard_page_t *page) {
  flowie_control_role_view_t roles[FLOWIE_CONTROL_DASHBOARD_PAGE_SIZE];
  flowie_control_dashboard_role_model *array = model->roles_storage;
  size_t count = 0u;
  int has_more = 0;
  int rc;
  model->collections.roles = (chttp_web_sequence_view){
      array, 0u, sizeof(*array), cmeta_reflected_data(flowie_control_dashboard_role_model)};
  for (size_t index = 0u; index < FLOWIE_CONTROL_DASHBOARD_PAGE_SIZE; ++index)
    roles[index] = (flowie_control_role_view_t)FLOWIE_CONTROL_ROLE_VIEW_INIT;
  rc = flowie_control_management_role_list(service, caller,
                                           page->roles_after[0] ? page->roles_after : NULL, roles,
                                           FLOWIE_CONTROL_DASHBOARD_PAGE_SIZE, &count, &has_more);
  for (size_t index = 0u; rc == SALTS_OK && index < count; ++index) {
    flowie_control_dashboard_role_model *item = &array[model->collections.roles.count];

    item->row_index = index + 1u;
    if (rc == SALTS_OK)
      rc = flowie_control_dashboard_model_string(&item->role_id, roles[index].role_id);
    if (rc == SALTS_OK) item->enabled = (roles[index].enabled) != 0;
    if (rc == SALTS_OK) ++model->collections.roles.count;
  }

  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_add_pager(
        &model->pagination.roles_pager, page, FLOWIE_CONTROL_DASHBOARD_ROLES_CURSOR, count,
        has_more && count > 0u, count > 0u ? roles[count - 1u].role_id : NULL, 0u);
  return rc;
}

static int flowie_control_dashboard_add_rules(flowie_control_dashboard_content_model *model,
                                              flowie_control_management_service_t *service,
                                              const flowie_control_management_caller_t *caller,
                                              const flowie_control_dashboard_page_t *page) {
  flowie_control_policy_subject_rule_view_t *rules = NULL;
  flowie_control_dashboard_rule_model *array = model->rules_storage;
  size_t count = 0u;
  uint64_t last_ordinal = 0u;
  int has_more = 0;
  int rc = SALTS_OK;
  model->collections.rules = (chttp_web_sequence_view){
      array, 0u, sizeof(*array), cmeta_reflected_data(flowie_control_dashboard_rule_model)};
  rules = (flowie_control_policy_subject_rule_view_t *)calloc(FLOWIE_CONTROL_DASHBOARD_PAGE_SIZE,
                                                              sizeof(*rules));
  if (!rules) {
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
    flowie_control_dashboard_rule_model *item = &array[model->collections.rules.count];

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
    if (rc == SALTS_OK) item->row_index = index + 1u;
    if (rc == SALTS_OK) item->ordinal = rules[index].ordinal;
    if (rc == SALTS_OK)
      rc = flowie_control_dashboard_model_string(&item->rule_document, rule_document);
    if (rc == SALTS_OK)
      rc = flowie_control_dashboard_model_string(
          &item->connection_label,
          document->connection_effect == FLOWIE_SECURITY_ALLOW ? "Allow" : "Deny");
    if (rc == SALTS_OK)
      rc = flowie_control_dashboard_model_string(&item->subject_label, document->subject);
    if (rc == SALTS_OK)
      rc = flowie_control_dashboard_model_string(&item->subject_kind_label, subject_kind_label);
    if (rc == SALTS_OK)
      rc = flowie_control_dashboard_model_string(&item->subject_kind, subject_kind_value);
    if (rc == SALTS_OK) item->entry_count = document->entry_count;
    if (rc == SALTS_OK) item->expanded_topic_count = expanded_topic_count;
    if (rc == SALTS_OK) item->uses_username = (uses_username) != 0;
    if (rc == SALTS_OK) item->uses_client_id = (uses_client_id) != 0;
    if (rc == SALTS_OK) ++model->collections.rules.count;
  }
  if (count > 0u) last_ordinal = rules[count - 1u].ordinal;

  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_add_pager(&model->pagination.policy_pager, page,
                                            FLOWIE_CONTROL_DASHBOARD_POLICY_CURSOR, count,
                                            has_more && count > 0u, NULL, last_ordinal);
  free(rules);
  return rc;
}

static int flowie_control_dashboard_add_audits(flowie_control_dashboard_content_model *model,
                                               flowie_control_management_service_t *service,
                                               const flowie_control_management_caller_t *caller,
                                               const flowie_control_dashboard_page_t *page) {
  flowie_control_audit_view_t *audits = NULL;
  flowie_control_dashboard_audit_model *array = model->audits_storage;
  size_t count = 0u;
  int has_more = 0;
  int rc = SALTS_OK;
  model->collections.audits = (chttp_web_sequence_view){
      array, 0u, sizeof(*array), cmeta_reflected_data(flowie_control_dashboard_audit_model)};
  audits =
      (flowie_control_audit_view_t *)calloc(FLOWIE_CONTROL_DASHBOARD_PAGE_SIZE, sizeof(*audits));
  if (!audits) {
    return SALTS_ENOMEM;
  }
  for (size_t index = 0u; index < FLOWIE_CONTROL_DASHBOARD_PAGE_SIZE; ++index)
    audits[index] = (flowie_control_audit_view_t)FLOWIE_CONTROL_AUDIT_VIEW_INIT;
  rc = flowie_control_management_audit_list(service, caller, page->audit_after, audits,
                                            FLOWIE_CONTROL_DASHBOARD_PAGE_SIZE, &count, &has_more);
  for (size_t index = 0u; rc == SALTS_OK && index < count; ++index) {
    flowie_control_dashboard_audit_model *item = &array[model->collections.audits.count];

    item->cursor = audits[index].revision;
    if (rc == SALTS_OK)
      rc = flowie_control_dashboard_model_string(&item->actor, audits[index].actor);
    if (rc == SALTS_OK)
      rc = flowie_control_dashboard_model_string(&item->operation, audits[index].operation);
    if (rc == SALTS_OK)
      rc = flowie_control_dashboard_model_string(&item->target_id, audits[index].target_id);
    if (rc == SALTS_OK) ++model->collections.audits.count;
  }

  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_add_pager(
        &model->pagination.audit_pager, page, FLOWIE_CONTROL_DASHBOARD_AUDIT_CURSOR, count,
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
  cmeta_mutex_init(&view->render_lock);
  if (!view->render_lock) {
    free(view);
    return SALTS_ENOMEM;
  }
  rc = flowie_control_dashboard_app_init(view, resource_directory);
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
  chttp_web_renderer_destroy(&view->renderer);
  cmeta_mutex_destroy(&view->render_lock);
  cmeta_fs_buf_free(&view->htmx);
  cmeta_fs_buf_free(&view->javascript);
  cmeta_fs_buf_free(&view->css);
  memset(view, 0, sizeof(*view));
  free(view);
}

int flowie_control_dashboard_view_render_shell(flowie_control_dashboard_view_t *view,
                                               const flowie_control_dashboard_page_t *page,
                                               char **html_out, size_t *html_size_out) {
  flowie_control_dashboard_shell_model model;
  char *content_url = NULL;
  int rc;
  if (!view || !page) return SALTS_EINVAL;
  rc = flowie_control_dashboard_url(FLOWIE_CONTROL_DASHBOARD_CONTENT_PATH, page, &content_url);
  if (rc != SALTS_OK) return rc;
  model.page_title = vstr_from_cstr(flowie_control_dashboard_section_title(page->section));
  model.content_url = vstr_from_cstr(content_url);
  rc = flowie_control_dashboard_app_render(
      view, FLOWIE_CONTROL_DASHBOARD_SHELL_TEMPLATE,
      cmeta_reflected_data(flowie_control_dashboard_shell_model), &model, html_out, html_size_out);
  tstr_free(content_url);
  return rc;
}

int flowie_control_dashboard_view_render_login(flowie_control_dashboard_view_t *view,
                                               int group_mode, int show_error, char **html_out,
                                               size_t *html_size_out) {
  flowie_control_dashboard_login_model model;
  if (!view || (group_mode != 0 && group_mode != 1) || (show_error != 0 && show_error != 1))
    return SALTS_EINVAL;
  model = (flowie_control_dashboard_login_model){!group_mode, group_mode != 0, show_error != 0};
  return flowie_control_dashboard_app_render(
      view, FLOWIE_CONTROL_DASHBOARD_LOGIN_TEMPLATE,
      cmeta_reflected_data(flowie_control_dashboard_login_model), &model, html_out, html_size_out);
}

int flowie_control_dashboard_view_render_password(
    flowie_control_dashboard_view_t *view,
    const char csrf_token[FLOWIE_CONTROL_DASHBOARD_CSRF_SIZE + 1u], char **html_out,
    size_t *html_size_out) {
  flowie_control_dashboard_password_model model;
  if (!view || !csrf_token) return SALTS_EINVAL;
  model.csrf = vstr_from_cstr(csrf_token);
  return flowie_control_dashboard_app_render(
      view, FLOWIE_CONTROL_DASHBOARD_PASSWORD_TEMPLATE,
      cmeta_reflected_data(flowie_control_dashboard_password_model), &model, html_out,
      html_size_out);
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
  flowie_control_dashboard_content_model *model = NULL;
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
  model = calloc(1u, sizeof(*model));
  if (!model) rc = SALTS_ENOMEM;
  else flowie_control_dashboard_content_model_init(model);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_model_string(&model->identity.domain_id, caller->domain_id);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_model_string(&model->identity.actor, caller->actor);
  if (rc == SALTS_OK) rc = flowie_control_dashboard_model_string(&model->identity.csrf, csrf_token);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_model_string(&model->navigation.rpc_path, rpc_path);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_model_string(&model->navigation.action_url, action_url);
  if (rc == SALTS_OK) model->identity.policy_version = status.policy.policy_version;
  if (rc == SALTS_OK) model->identity.draft_rule_count = status.policy.draft_rule_count;
  if (rc == SALTS_OK) model->identity.published_rule_count = status.policy.published_rule_count;
  if (rc == SALTS_OK) model->capabilities.can_user_admin = (can_user_admin) != 0;
  if (rc == SALTS_OK) model->capabilities.can_security_admin = (can_security_admin) != 0;
  if (rc == SALTS_OK) model->capabilities.can_policy_admin = (can_policy_admin) != 0;
  if (rc == SALTS_OK)
    model->capabilities.can_manage_access = (can_user_admin || can_security_admin) != 0;
  if (rc == SALTS_OK) model->capabilities.can_audit_read = (can_audit_read) != 0;
  if (rc == SALTS_OK) model->capabilities.can_create_domain = (can_create_domain) != 0;
  if (rc == SALTS_OK) model->capabilities.is_platform_workspace = (is_platform_workspace) != 0;
  if (rc == SALTS_OK) model->capabilities.is_domain_workspace = (is_domain_workspace) != 0;
  if (rc == SALTS_OK && can_create_domain)
    rc = flowie_control_dashboard_add_domains(model, service, authority_caller, caller);
  if (rc == SALTS_OK && action_result &&
      action_result->kind == FLOWIE_CONTROL_DASHBOARD_ACTION_CREDENTIAL_ISSUED) {
    if (strcmp(action_result->domain_id, caller->domain_id) != 0) rc = SALTS_EPROTO;
    else {
      model->capabilities.credential_issued = (1) != 0;
      if (rc == SALTS_OK)
        rc = flowie_control_dashboard_model_string(&model->identity.credential_domain,
                                                   action_result->domain_id);
      if (rc == SALTS_OK)
        rc = flowie_control_dashboard_model_string(&model->identity.credential_principal,
                                                   action_result->principal_id);
      if (rc == SALTS_OK)
        rc = flowie_control_dashboard_model_string(&model->identity.credential_token,
                                                   action_result->token);
    }
  }
  if (rc == SALTS_OK) model->sections.show_overview = (show_overview) != 0;
  if (rc == SALTS_OK) model->sections.show_users = (show_users) != 0;
  if (rc == SALTS_OK) model->sections.show_groups = (show_groups) != 0;
  if (rc == SALTS_OK) model->sections.show_roles = (show_roles) != 0;
  if (rc == SALTS_OK) model->sections.show_acls = (show_acls) != 0;
  if (rc == SALTS_OK) model->sections.show_audit = (show_audit) != 0;
  if (rc == SALTS_OK) model->sections.show_integration = (show_integration) != 0;
  if (rc == SALTS_OK)
    model->sections.is_overview = (page->section == FLOWIE_CONTROL_DASHBOARD_SECTION_OVERVIEW) != 0;
  if (rc == SALTS_OK)
    model->sections.is_users = (page->section == FLOWIE_CONTROL_DASHBOARD_SECTION_USERS) != 0;
  if (rc == SALTS_OK)
    model->sections.is_groups = (page->section == FLOWIE_CONTROL_DASHBOARD_SECTION_GROUPS) != 0;
  if (rc == SALTS_OK)
    model->sections.is_roles = (page->section == FLOWIE_CONTROL_DASHBOARD_SECTION_ROLES) != 0;
  if (rc == SALTS_OK)
    model->sections.is_acls = (page->section == FLOWIE_CONTROL_DASHBOARD_SECTION_ACLS) != 0;
  if (rc == SALTS_OK)
    model->sections.is_audit = (page->section == FLOWIE_CONTROL_DASHBOARD_SECTION_AUDIT) != 0;
  if (rc == SALTS_OK)
    model->sections.is_integration =
        (page->section == FLOWIE_CONTROL_DASHBOARD_SECTION_INTEGRATION) != 0;
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_model_string(&model->navigation.overview_path, overview_url);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_model_string(&model->navigation.users_path, users_url);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_model_string(&model->navigation.groups_path, groups_url);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_model_string(&model->navigation.roles_path, roles_url);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_model_string(&model->navigation.acls_path, acls_url);
  if (rc == SALTS_OK)
    rc = flowie_control_dashboard_model_string(&model->navigation.audit_path, audit_url);
  if (rc == SALTS_OK)
    rc =
        flowie_control_dashboard_model_string(&model->navigation.integration_path, integration_url);
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
    rc = flowie_control_dashboard_app_render(
        view, FLOWIE_CONTROL_DASHBOARD_CONTENT_TEMPLATE,
        cmeta_reflected_data(flowie_control_dashboard_content_model), model, html_out,
        html_size_out);
done:

  tstr_free(action_url);
  tstr_free(overview_url);
  tstr_free(users_url);
  tstr_free(groups_url);
  tstr_free(roles_url);
  tstr_free(acls_url);
  tstr_free(audit_url);
  tstr_free(integration_url);
  if (model) {
    flowie_control_dashboard_content_model_clear(model);
    free(model);
  }
  return rc;
}

int flowie_control_dashboard_view_render_error(flowie_control_dashboard_view_t *view,
                                               const char *message, char **html_out,
                                               size_t *html_size_out) {
  flowie_control_dashboard_error_model model;
  if (!view || !message) return SALTS_EINVAL;
  model.message = vstr_from_cstr(message);
  return flowie_control_dashboard_app_render(
      view, FLOWIE_CONTROL_DASHBOARD_ERROR_TEMPLATE,
      cmeta_reflected_data(flowie_control_dashboard_error_model), &model, html_out, html_size_out);
}

int flowie_control_dashboard_view_asset(const flowie_control_dashboard_view_t *view,
                                        flowie_control_dashboard_asset_t asset,
                                        const void **data_out, size_t *size_out) {
  const cmeta_fs_buf_t *resource;
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
