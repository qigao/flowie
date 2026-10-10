#include "flowie_server_runtime_internal.h"
#include "flowie_server_http_security_internal.h"

#include <cmeta/data_reflect.h>
#include <salts/component.h>
#include <salts/error_codes.h>

#include <stdlib.h>

enum {
  FLOWIE_SERVER_SECURITY_COMPONENT,
  FLOWIE_SERVER_REPOSITORY_COMPONENT,
  FLOWIE_SERVER_ENDPOINT_COMPONENT,
  FLOWIE_SERVER_COMPONENT_COUNT,
  FLOWIE_SERVER_DEPENDENCY_COUNT = 2
};

typedef struct flowie_server_runtime_config {
  const flowie_server_config_t *server;
  const flowie_endpoint_config_t *endpoint;
  const orm_config_t *database;
  const flowie_endpoint_core_options_t *options;
  int check_only;
} flowie_server_runtime_config;

/* VIEW describes native configuration without claiming copy/value ownership. */
cmeta_reflect_data(flowie_server_runtime_config, "flowie.server.RuntimeConfig",
    cmeta_field(int, check_only));

typedef struct flowie_server_component_state {
  int kind;
  int status;
  const char *operation;
  const flowie_server_runtime_config *config;
  union {
    struct {
      flowie_server_http_security_t *http;
      flowie_security_realm_t *realm;
      flowie_endpoint_security_binding_t binding;
    } security;
    flowie_protocol_repository_t *repository;
    flowie_endpoint_core_t *endpoint;
  } resource;
} flowie_server_component_state;

cmeta_reflect_data(flowie_server_component_state, "flowie.server.ComponentState",
    cmeta_field(int, kind) cmeta_field(int, status));

#define FLOWIE_SERVER_SECURITY_METHODS(X, I) \
  X(I, R0, const flowie_endpoint_security_binding_t *, binding, _)
#define FLOWIE_SERVER_REPOSITORY_METHODS(X, I) \
  X(I, R0, flowie_protocol_repository_t *, repository, _)
#define FLOWIE_SERVER_ENDPOINT_METHODS(X, I) \
  X(I, R0, flowie_endpoint_core_t *, endpoint, _)

CMETA_INTERFACE(flowie_server_security_service, FLOWIE_SERVER_SECURITY_METHODS);
CMETA_INTERFACE(flowie_server_repository_service, FLOWIE_SERVER_REPOSITORY_METHODS);
CMETA_INTERFACE(flowie_server_endpoint_service, FLOWIE_SERVER_ENDPOINT_METHODS);
CMETA_OBJECT_INTERFACE_ADAPTER(flowie_server_security_service);
CMETA_OBJECT_INTERFACE_ADAPTER(flowie_server_repository_service);
CMETA_OBJECT_INTERFACE_ADAPTER(flowie_server_endpoint_service);

cmeta_component_configured(FlowieServerSecurity,
    cmeta_reflected_data(flowie_server_runtime_config),
    cmeta_provides(flowie_server_security_service));
cmeta_component_configured(FlowieServerRepository,
    cmeta_reflected_data(flowie_server_runtime_config),
    cmeta_provides(flowie_server_repository_service));
cmeta_component_configured(FlowieServerEndpoint,
    cmeta_reflected_data(flowie_server_runtime_config),
    cmeta_provides(flowie_server_endpoint_service)
    cmeta_requires(flowie_server_security_service)
    cmeta_requires(flowie_server_repository_service));

static const flowie_endpoint_security_binding_t *flowie_server_security_binding(void *self) {
  flowie_server_component_state *state = self;
  return state->resource.security.realm ? &state->resource.security.binding : NULL;
}

static flowie_protocol_repository_t *flowie_server_repository(void *self) {
  return ((flowie_server_component_state *)self)->resource.repository;
}

static flowie_endpoint_core_t *flowie_server_endpoint(void *self) {
  return ((flowie_server_component_state *)self)->resource.endpoint;
}

CMETA_IMPLEMENTS(flowie_server_security_service, flowie_server_security_impl, 0u,
    .binding = flowie_server_security_binding);
CMETA_IMPLEMENTS(flowie_server_repository_service, flowie_server_repository_impl, 0u,
    .repository = flowie_server_repository);
CMETA_IMPLEMENTS(flowie_server_endpoint_service, flowie_server_endpoint_impl, 0u,
    .endpoint = flowie_server_endpoint);

static cmeta_status flowie_server_service_project(void *context,
    const cmeta_object_ref *object, const cmeta_interface_desc *expected,
    cmeta_interface_projection *out) {
  const flowie_server_component_state *state = object->object;
  (void)context;
  if (state->kind == FLOWIE_SERVER_SECURITY_COMPONENT &&
      cmeta_interface_desc_equal(expected, flowie_server_security_service_interface()))
    out->dispatch = &flowie_server_security_impl_vtable;
  else if (state->kind == FLOWIE_SERVER_REPOSITORY_COMPONENT &&
           cmeta_interface_desc_equal(expected, flowie_server_repository_service_interface()))
    out->dispatch = &flowie_server_repository_impl_vtable;
  else if (state->kind == FLOWIE_SERVER_ENDPOINT_COMPONENT &&
           cmeta_interface_desc_equal(expected, flowie_server_endpoint_service_interface()))
    out->dispatch = &flowie_server_endpoint_impl_vtable;
  else
    return CMETA_TRAIT_MISSING;
  out->size = sizeof(*out);
  out->interface = expected;
  out->self = object->object;
  return CMETA_OK;
}

static const cmeta_object_interface_provider flowie_server_services = {
    sizeof(cmeta_object_interface_provider), NULL, flowie_server_service_project};

/* ObjectRef owns these resources, while the address-stable state storage belongs
 * to the enclosing context. This also handles partial create/activate failure. */
static void flowie_server_component_destroy(void *context, void *object) {
  flowie_server_component_state *state = object;
  (void)context;
  if (state->kind == FLOWIE_SERVER_ENDPOINT_COMPONENT) {
    flowie_endpoint_core_destroy(state->resource.endpoint);
    state->resource.endpoint = NULL;
  } else if (state->kind == FLOWIE_SERVER_REPOSITORY_COMPONENT) {
    flowie_protocol_repository_close(state->resource.repository);
    state->resource.repository = NULL;
  } else {
    flowie_security_realm_destroy(state->resource.security.realm);
    flowie_server_http_security_destroy(state->resource.security.http);
    state->resource.security.realm = NULL;
    state->resource.security.http = NULL;
  }
}

static const cmeta_object_lifecycle flowie_server_component_lifecycle = {
    .size = sizeof(cmeta_object_lifecycle), .destroy = flowie_server_component_destroy};

static int flowie_server_security_create(flowie_server_component_state *state) {
  const flowie_server_config_t *config = state->config->server;
  flowie_security_realm_config_t realm = FLOWIE_SECURITY_REALM_CONFIG_INIT;
  flowie_endpoint_security_binding_t *binding = &state->resource.security.binding;
  int rc;
  state->operation = "security-runtime-create";
  if (!config || !flowie_server_config_realm_name(config)) return SALTS_OK;
  if (state->config->endpoint->max_connections == 0u) return SALTS_EINVAL;
  rc = flowie_server_http_security_create(flowie_server_config_auth(config),
                                         flowie_server_config_acl(config),
                                         &state->resource.security.http);
  if (rc != SALTS_OK) return rc;
  realm.resource_uid = flowie_server_config_realm_resource_uid(config);
  realm.owner_name = flowie_server_config_realm_owner_name(config);
  realm.policy_source = flowie_server_config_acl_provider_name(config);
  rc = flowie_security_realm_create(&realm, &state->resource.security.realm);
  if (rc != SALTS_OK) return rc;
  rc = flowie_security_realm_bind_authorization_provider(state->resource.security.realm,
      flowie_server_http_security_acl_provider(state->resource.security.http));
  if (rc != SALTS_OK) return rc;
  *binding = (flowie_endpoint_security_binding_t)FLOWIE_ENDPOINT_SECURITY_BINDING_INIT;
  binding->realm_channel = flowie_server_config_realm_name(config);
  binding->auth_method = flowie_server_config_auth_method(config);
  binding->auth_provider = flowie_server_http_security_auth_provider(state->resource.security.http);
  binding->realm = state->resource.security.realm;
  return SALTS_OK;
}

static int flowie_server_repository_create(flowie_server_component_state *state) {
  const flowie_endpoint_config_t *endpoint = state->config->endpoint;
  flowie_protocol_repository_config_t config = FLOWIE_PROTOCOL_REPOSITORY_CONFIG_INIT;
  state->operation = "protocol-store-open";
  config.database = state->config->database;
  config.namespace_name = "flowie_server";
  config.create_schema = 1;
  config.limits.max_sessions = endpoint->max_sessions;
  config.limits.max_subscriptions_per_session = endpoint->max_subscriptions_per_session;
  config.limits.max_inflight_per_session = endpoint->max_inflight_per_session;
  config.limits.max_retained_messages = endpoint->max_retained_messages;
  config.limits.max_client_id_size = FLOWIE_MQTT_MAX_UTF8_SIZE;
  config.limits.max_topic_size = FLOWIE_MQTT_MAX_UTF8_SIZE;
  config.limits.max_packet_size = endpoint->max_packet_size;
  return flowie_protocol_repository_open(&config, &state->resource.repository);
}

static cmeta_status flowie_server_endpoint_create(flowie_server_component_state *state,
    const salts_component_dependency *dependencies, size_t dependency_count) {
  const salts_component_dependency *security = NULL, *repository = NULL;
  flowie_server_security_service security_service;
  flowie_server_repository_service repository_service;
  flowie_endpoint_bindings_t bindings = FLOWIE_ENDPOINT_BINDINGS_INIT;
  flowie_endpoint_persistence_binding_t persistence = FLOWIE_ENDPOINT_PERSISTENCE_BINDING_INIT;
  flowie_execution_binding_t execution = FLOWIE_EXECUTION_BINDING_INIT;
  cmeta_status status;
  state->operation = "endpoint-create";
  if (salts_component_dependency_find(dependencies, dependency_count,
          flowie_server_security_service_interface(), &security) != SALTS_COMPONENT_OK ||
      salts_component_dependency_find(dependencies, dependency_count,
          flowie_server_repository_service_interface(), &repository) != SALTS_COMPONENT_OK)
    return CMETA_TRAIT_MISSING;
  status = flowie_server_security_service_borrow_from_object(security->provider_instance,
      security->provider_interfaces, &security_service);
  if (status != CMETA_OK) return status;
  status = flowie_server_repository_service_borrow_from_object(repository->provider_instance,
      repository->provider_interfaces, &repository_service);
  if (status != CMETA_OK) return status;
  bindings.security = flowie_server_security_service_binding(&security_service);
  persistence.repository = flowie_server_repository_service_repository(&repository_service);
  bindings.persistence = &persistence;
  state->status = flowie_endpoint_core_create_ex("mqtt", state->config->endpoint,
      state->config->options, &execution, &bindings, &state->resource.endpoint);
  return state->status == SALTS_OK ? CMETA_OK : CMETA_CALLBACK_ERROR;
}

static cmeta_status SALTS_COMPONENT_CALL flowie_server_component_create(void *context,
    const cmeta_data_desc *config_data, const void *config_value,
    const salts_component_dependency *dependencies, size_t dependency_count,
    cmeta_object_ref *out) {
  flowie_server_component_state *state = context;
  cmeta_status status;
  if (!cmeta_data_desc_equal(config_data, cmeta_reflected_data(flowie_server_runtime_config)) ||
      !config_value || dependency_count !=
          (state->kind == FLOWIE_SERVER_ENDPOINT_COMPONENT ? FLOWIE_SERVER_DEPENDENCY_COUNT : 0u))
    return CMETA_INVALID_ARGUMENT;
  state->config = config_value;
  status = cmeta_object_borrow(out, state, cmeta_reflected_data(flowie_server_component_state), NULL);
  if (status != CMETA_OK) return status;
  status = cmeta_object_take(out, &flowie_server_component_lifecycle);
  if (status != CMETA_OK) return status;
  if (state->kind == FLOWIE_SERVER_ENDPOINT_COMPONENT)
    return flowie_server_endpoint_create(state, dependencies, dependency_count);
  state->status = state->kind == FLOWIE_SERVER_SECURITY_COMPONENT
      ? flowie_server_security_create(state) : flowie_server_repository_create(state);
  return state->status == SALTS_OK ? CMETA_OK : CMETA_CALLBACK_ERROR;
}

static cmeta_status SALTS_COMPONENT_CALL flowie_server_endpoint_activate(void *context,
    const cmeta_object_ref *object) {
  flowie_server_component_state *state = context;
  (void)object;
  if (state->config->check_only) return CMETA_OK;
  state->operation = "endpoint-start";
  state->status = flowie_endpoint_core_start(state->resource.endpoint);
  return state->status == SALTS_OK ? CMETA_OK : CMETA_CALLBACK_ERROR;
}

static void SALTS_COMPONENT_CALL flowie_server_endpoint_deactivate(void *context,
    const cmeta_object_ref *object) {
  flowie_server_component_state *state = context;
  (void)object;
  state->status = flowie_endpoint_core_stop(state->resource.endpoint);
}

struct flowie_server_runtime_s {
  flowie_server_runtime_config config;
  salts_component_context context;
  salts_component_provider_binding providers[FLOWIE_SERVER_COMPONENT_COUNT];
  salts_component_deployment deployments[FLOWIE_SERVER_COMPONENT_COUNT];
  salts_component_instance instances[FLOWIE_SERVER_COMPONENT_COUNT];
  salts_component_dependency dependencies[FLOWIE_SERVER_DEPENDENCY_COUNT];
  size_t order[FLOWIE_SERVER_COMPONENT_COUNT];
  flowie_server_component_state states[FLOWIE_SERVER_COMPONENT_COUNT];
  flowie_server_endpoint_service endpoint;
};

int flowie_server_runtime_create(const flowie_server_config_t *config,
    const flowie_endpoint_config_t *endpoint_config, const orm_config_t *database,
    const flowie_endpoint_core_options_t *options, int check_only,
    flowie_server_runtime_t **out, const char **operation) {
  flowie_server_runtime_t *runtime;
  salts_component_service service;
  salts_component_status status;
  const cmeta_component_desc *components[] = {
      cmeta_component_meta(FlowieServerSecurity), cmeta_component_meta(FlowieServerRepository),
      cmeta_component_meta(FlowieServerEndpoint)};
  int rc = SALTS_EIO;
  if (out) *out = NULL;
  if (operation) *operation = "component-configure";
  if (!out || !endpoint_config || !database || !options || (check_only != 0 && check_only != 1))
    return SALTS_EINVAL;
  runtime = calloc(1u, sizeof(*runtime));
  if (!runtime) return SALTS_ENOMEM;
  runtime->context = (salts_component_context)SALTS_COMPONENT_CONTEXT_INIT;
  runtime->config = (flowie_server_runtime_config){config, endpoint_config, database, options,
                                                  check_only};
  for (size_t index = 0u; index < FLOWIE_SERVER_COMPONENT_COUNT; ++index) {
    runtime->states[index].kind = (int)index;
    runtime->providers[index] = (salts_component_provider_binding){
        sizeof(salts_component_provider_binding), SALTS_COMPONENT_PROVIDER_BINDING_ABI_VERSION,
        components[index], &runtime->states[index], &flowie_server_services,
        flowie_server_component_create, NULL, NULL};
    runtime->deployments[index] = (salts_component_deployment){&runtime->providers[index],
        cmeta_reflected_data(flowie_server_runtime_config), &runtime->config};
  }
  runtime->providers[FLOWIE_SERVER_ENDPOINT_COMPONENT].activate = flowie_server_endpoint_activate;
  runtime->providers[FLOWIE_SERVER_ENDPOINT_COMPONENT].deactivate = flowie_server_endpoint_deactivate;
  status = salts_component_context_init(&runtime->context, runtime->deployments,
      FLOWIE_SERVER_COMPONENT_COUNT, NULL, 0u, runtime->instances, FLOWIE_SERVER_COMPONENT_COUNT,
      runtime->dependencies, FLOWIE_SERVER_DEPENDENCY_COUNT, runtime->order,
      FLOWIE_SERVER_COMPONENT_COUNT);
  if (status == SALTS_COMPONENT_OK) status = salts_component_context_resolve(&runtime->context);
  if (status == SALTS_COMPONENT_OK) status = salts_component_context_start(&runtime->context);
  if (status != SALTS_COMPONENT_OK) {
    const salts_component_failure *failure = salts_component_context_failure(&runtime->context);
    if (failure && failure->component_index < FLOWIE_SERVER_COMPONENT_COUNT) {
      const flowie_server_component_state *failed = &runtime->states[failure->component_index];
      if (failed->status != SALTS_OK) rc = failed->status;
      if (operation && failed->operation) *operation = failed->operation;
    }
    free(runtime); /* start rolls back valid partial ObjectRefs before returning. */
    return rc;
  }
  status = salts_component_context_find_service(&runtime->context,
      flowie_server_endpoint_service_interface(), &service);
  if (status != SALTS_COMPONENT_OK || flowie_server_endpoint_service_borrow_from_object(
          service.object, service.interfaces, &runtime->endpoint) != CMETA_OK) {
    (void)salts_component_context_stop(&runtime->context);
    free(runtime);
    return SALTS_EIO;
  }
  *out = runtime;
  return SALTS_OK;
}

flowie_endpoint_core_t *flowie_server_runtime_endpoint(const flowie_server_runtime_t *runtime) {
  return runtime ? flowie_server_endpoint_service_endpoint(&runtime->endpoint) : NULL;
}

int flowie_server_runtime_destroy(flowie_server_runtime_t **runtime) {
  int rc;
  if (!runtime) return SALTS_EINVAL;
  if (!*runtime) return SALTS_OK;
  if (salts_component_context_stop(&(*runtime)->context) != SALTS_COMPONENT_OK)
    return SALTS_EIO;
  rc = (*runtime)->states[FLOWIE_SERVER_ENDPOINT_COMPONENT].status;
  free(*runtime);
  *runtime = NULL;
  return rc;
}
