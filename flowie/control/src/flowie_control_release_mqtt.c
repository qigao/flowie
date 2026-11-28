#include "flowie_control_release_mqtt_internal.h"

#include "flowie_control_validation_internal.h"
#include "flowie_mqtt_security.h"

#include <core/error.h>
#include <core/tlog.h>
#include <crypto/sm3.h>
#include <cyaml.h>
#include <cyaml_json_adapter.h>
#include <json_parser.h>
#include <threads/threads.h>

#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct flowie_release_mqtt_candidate_s {
  flowie_release_archive_t *archive;
  flowie_security_rule_t *rules;
  size_t rule_count;
  uint8_t digest[FLOWIE_RELEASE_DIGEST_BYTES];
} flowie_release_mqtt_candidate_t;

struct flowie_control_release_mqtt_s {
  flowie_control_release_mqtt_config_t config;
  sync_mutex_t mutation;
  sync_rwlock_t gate;
  flowie_security_auth_provider_t authenticator;
  flowie_security_authorization_provider_t authorizer;
  flowie_endpoint_security_binding_t binding;
  flowie_security_realm_t *active_realm;
  void *active_processing;
  flowie_control_release_active_t active;
  int backend_started;
  int faulted;
};

static int flowie_release_processing_prepare(const flowie_control_release_mqtt_config_t *config,
    const flowie_release_archive_t *archive, void **out) {
  *out = NULL;
  const flowie_release_manifest_t *manifest = flowie_control_release_archive_manifest(archive);
  if (manifest->rulesforge_index == manifest->file_count) return SALTS_OK;
  if (!config->processing) return SALTS_ENOTSUP;
  const int rc = config->processing->prepare(config->processing->context, archive, out);
  return rc == SALTS_OK && !*out ? SALTS_EIO : rc;
}

static int flowie_release_processing_close(const flowie_control_release_mqtt_config_t *config,
    void *handle) {
  return handle ? config->processing->close(config->processing->context, handle) : SALTS_OK;
}

static void flowie_release_processing_discard(const flowie_control_release_mqtt_config_t *config,
    void *handle) {
  const int rc = flowie_release_processing_close(config, handle);
  if (rc != SALTS_OK)
    SALTS_LOG_WARNF(tlog_peek_default(), "Flowie.Control.Release",
        "processing-close domain={} status={} ({})", config->domain, rc, error_code_string(rc));
}

static void flowie_release_mqtt_candidate_clear(flowie_release_mqtt_candidate_t *candidate) {
  flowie_control_release_archive_destroy(candidate->archive);
  free(candidate->rules);
}

/* Previously signed ACL packages include an empty routes document. */
static int flowie_release_mqtt_legacy_routes(const void *body, size_t size) {
  if (size == 0 || size > FLOWIE_CONTROL_ACL_DOCUMENT_MAX || memchr(body, 0, size))
    return SALTS_EPROTO;
  cyaml_doc_t *yaml = cyaml_parse(body, size, NULL, NULL);
  if (!yaml) return SALTS_EPROTO;
  json_value_t *graph = json_value_from_cyaml(yaml);
  cyaml_free(yaml);
  if (!graph) return SALTS_EPROTO;
  const json_value_t *routes = json_object_get(graph, "routes");
  const int empty = json_type(graph) == JSON_OBJECT && json_object_size(graph) == 1u && routes &&
                    json_type(routes) == JSON_ARRAY && json_array_size(routes) == 0u;
  json_free(graph);
  return empty ? SALTS_OK : SALTS_EPROTONOSUPPORT;
}

static int flowie_release_mqtt_compile(const flowie_control_release_mqtt_config_t *config,
    const void *acl, size_t size, flowie_release_mqtt_candidate_t *candidate) {
  const int rc = flowie_control_acl_bundle_compile(config->domain, acl, size,
      &candidate->rules, &candidate->rule_count);
  if (rc != SALTS_OK) return rc;
  return salts_crypto_sm3(acl, size, candidate->digest) == SALTS_CRYPTO_OK ? SALTS_OK : SALTS_EIO;
}

static int flowie_release_mqtt_prepare(const flowie_control_release_mqtt_config_t *config,
    const uint8_t *digest, flowie_release_mqtt_candidate_t *candidate) {
  const time_t now = time(NULL);
  if (now == (time_t)-1) return SALTS_EIO;
  salts_crypto_sm2_trust_policy trust = *config->signer->trust;
  salts_crypto_sm2_cms_verify_config signer = *config->signer;
  trust.validation_time = (int64_t)now;
  signer.trust = &trust;
  void *bytes = NULL;
  size_t size = 0;
  int rc = flowie_control_release_store_read(config->store, config->environment, config->domain,
                                              digest, &bytes, &size);
  if (rc != SALTS_OK) return rc;
  rc = flowie_control_release_archive_verify(&signer, config->domain, config->environment,
                                              bytes, size, &candidate->archive);
  free(bytes);
  if (rc != SALTS_OK) return rc;

  const flowie_release_manifest_t *manifest = flowie_control_release_archive_manifest(candidate->archive);
  const void *body = NULL;
  if (manifest->routes_index != manifest->file_count) {
    rc = flowie_control_release_archive_file(candidate->archive,
        manifest->files[manifest->routes_index].path, &body, &size);
    if (rc == SALTS_OK) rc = flowie_release_mqtt_legacy_routes(body, size);
    if (rc != SALTS_OK) return rc;
  }
  rc = flowie_control_release_archive_file(candidate->archive, manifest->files[manifest->acl_index].path,
                                          &body, &size);
  return rc == SALTS_OK ? flowie_release_mqtt_compile(config, body, size, candidate) : rc;
}

static int flowie_release_mqtt_realm(const flowie_control_release_mqtt_config_t *config,
    const flowie_release_mqtt_candidate_t *candidate, uint64_t version, flowie_security_realm_t **out) {
  flowie_security_realm_config_t realm = FLOWIE_SECURITY_REALM_CONFIG_INIT;
  realm.resource_uid = config->resource_uid;
  realm.owner_name = config->owner_name;
  realm.policy_version = version;
  if (candidate) {
    realm.rules = candidate->rules;
    realm.rule_count = candidate->rule_count;
    const int rc = flowie_mqtt_security_matcher_init(&realm.matcher);
    if (rc != SALTS_OK) return rc;
  } else {
    realm.policy_source = config->realm_name;
  }
  return flowie_security_realm_create(&realm, out);
}

static int flowie_release_mqtt_authorize(void *context, const flowie_security_request_t *request,
    uint64_t now, flowie_security_decision_t *out) {
  flowie_control_release_mqtt_t *runtime = context;
  sync_rwlock_rdlock(&runtime->gate);
  int rc = runtime->faulted || !runtime->active_realm ? SALTS_ESHUTDOWN :
           flowie_security_realm_evaluate(runtime->active_realm, request, now, out);
  /* The provider contract requires a successful result to echo the principal's version. */
  if (rc == SALTS_OK && out->reason == FLOWIE_SECURITY_REASON_POLICY_VERSION_MISMATCH)
    rc = SALTS_EPERM;
  sync_rwlock_rdunlock(&runtime->gate);
  return rc;
}

static int flowie_release_mqtt_authenticate(void *context, const flowie_security_auth_request_t *request,
                                             flowie_security_principal_t *principal) {
  flowie_control_release_mqtt_t *runtime = context;
  flowie_security_principal_t identity = FLOWIE_SECURITY_PRINCIPAL_INIT;
  int rc = flowie_security_authenticate(runtime->config.identity_provider, request, &identity);
  if (rc != SALTS_OK) return rc;
  if (strcmp(identity.domain_id, runtime->config.domain) != 0) return SALTS_EPERM;
  sync_rwlock_rdlock(&runtime->gate);
  if (runtime->faulted || !runtime->active_realm) {
    /* The broker maps backend unavailability to a CONNACK refusal. */
    rc = SALTS_EIO;
  } else {
    identity.policy_version = runtime->active.acl_version;
    *principal = identity;
  }
  sync_rwlock_rdunlock(&runtime->gate);
  return rc;
}

int flowie_control_release_mqtt_open(const flowie_control_release_mqtt_config_t *config,
                                     flowie_control_release_mqtt_t **out) {
  if (!out) return SALTS_EINVAL;
  *out = NULL;
  if (!config || !config->store || !config->signer || !config->signer->trust ||
      !config->identity_provider || config->identity_provider->size < sizeof(*config->identity_provider) ||
      !config->identity_provider->authenticate ||
      !config->realm_name || !config->resource_uid || !config->owner_name || !config->auth_method ||
      (config->processing && (!config->processing->prepare || !config->processing->close ||
          (config->processing->activate && !config->journal))))
    return SALTS_EINVAL;

  flowie_control_release_mqtt_t *runtime = calloc(1u, sizeof(*runtime));
  if (!runtime) return SALTS_ENOMEM;
  int rc = sync_rwlock_init(&runtime->gate);
  if (rc != SALTS_OK) {
    free(runtime);
    return rc;
  }
  runtime->config = *config;
  sync_mutex_init(&runtime->mutation);
  runtime->binding = (flowie_endpoint_security_binding_t)FLOWIE_ENDPOINT_SECURITY_BINDING_INIT;
  runtime->binding.realm_channel = config->realm_name;
  runtime->binding.auth_method = config->auth_method;
  runtime->authenticator = (flowie_security_auth_provider_t){sizeof(runtime->authenticator),
      runtime, flowie_release_mqtt_authenticate};
  runtime->authorizer = (flowie_security_authorization_provider_t){sizeof(runtime->authorizer),
      runtime, flowie_release_mqtt_authorize};
  runtime->binding.auth_provider = &runtime->authenticator;
  rc = flowie_release_mqtt_realm(config, NULL, 0, &runtime->binding.realm);
  if (rc == SALTS_OK)
    rc = flowie_security_realm_bind_authorization_provider(runtime->binding.realm, &runtime->authorizer);
  if (rc == SALTS_OK)
    rc = flowie_control_release_store_current(config->store, config->environment, config->domain,
                                               &runtime->active);
  if (rc == SALTS_OK && config->journal) {
    const flowie_release_journal_scope_t scope = {config->environment, config->domain,
        runtime->active.generation, runtime->active.digest};
    rc = flowie_control_release_journal_verify(config->journal, &scope);
  }
  if (rc == SALTS_OK && runtime->active.generation) {
    flowie_release_mqtt_candidate_t candidate = {0};
    rc = flowie_release_mqtt_prepare(config, runtime->active.digest, &candidate);
    if (rc == SALTS_OK && memcmp(candidate.digest, runtime->active.acl_digest, sizeof(candidate.digest)) != 0)
      rc = SALTS_EIO;
    if (rc == SALTS_OK) rc = flowie_release_mqtt_realm(config, &candidate,
        runtime->active.acl_version, &runtime->active_realm);
    if (rc == SALTS_OK)
      rc = flowie_release_processing_prepare(config, candidate.archive, &runtime->active_processing);
    flowie_release_mqtt_candidate_clear(&candidate);
  }
  if (rc != SALTS_OK) {
    flowie_control_release_mqtt_close(runtime);
    return rc;
  }
  *out = runtime;
  return SALTS_OK;
}

void flowie_control_release_mqtt_close(flowie_control_release_mqtt_t *runtime) {
  if (!runtime) return;
  flowie_release_processing_discard(&runtime->config, runtime->active_processing);
  flowie_security_realm_destroy(runtime->binding.realm);
  flowie_security_realm_destroy(runtime->active_realm);
  sync_rwlock_destroy(&runtime->gate);
  sync_mutex_destroy(&runtime->mutation);
  free(runtime);
}

const flowie_endpoint_security_binding_t *flowie_control_release_mqtt_binding(
    const flowie_control_release_mqtt_t *runtime) {
  return runtime ? &runtime->binding : NULL;
}

const flowie_control_release_mqtt_config_t *flowie_control_release_mqtt_config(
    const flowie_control_release_mqtt_t *runtime) {
  return runtime ? &runtime->config : NULL;
}

int flowie_control_release_mqtt_status(flowie_control_release_mqtt_t *runtime,
    flowie_control_release_active_t *committed, int *ready) {
  if (!runtime || !committed || !ready) return SALTS_EINVAL;
  const flowie_control_release_mqtt_config_t *config = &runtime->config;
  sync_rwlock_rdlock(&runtime->gate);
  const int rc = flowie_control_release_store_current(config->store, config->environment,
      config->domain, committed);
  *ready = rc == SALTS_OK && !runtime->faulted && runtime->active_realm &&
      runtime->active.generation == committed->generation &&
      memcmp(runtime->active.digest, committed->digest, sizeof(runtime->active.digest)) == 0;
  sync_rwlock_rdunlock(&runtime->gate);
  return rc;
}

void flowie_control_release_mqtt_resume(flowie_control_release_mqtt_t *runtime) {
  sync_rwlock_wrlock(&runtime->gate);
  runtime->backend_started = 1;
  if (runtime->active_processing && runtime->config.processing->activate)
    runtime->config.processing->activate(runtime->active_processing, &runtime->active, 1);
  sync_rwlock_wrunlock(&runtime->gate);
}

int flowie_control_release_mqtt_publish(flowie_control_release_mqtt_t *runtime,
    const flowie_security_principal_t *principal, const flowie_mqtt_publish_view_t *message,
    const uint8_t *exchange, int *handled) {
  if (!runtime || !message || !handled) return SALTS_EINVAL;
  *handled = 0;
  int rc = SALTS_OK;
  sync_rwlock_rdlock(&runtime->gate);
  if (runtime->faulted || !runtime->active_realm) {
    rc = SALTS_ESHUTDOWN;
  } else if (runtime->config.journal ||
             (runtime->active_processing && runtime->config.processing->publish)) {
    if (!principal || principal->policy_version != runtime->active.acl_version ||
        strcmp(principal->domain_id, runtime->config.domain) != 0) {
      rc = SALTS_EPERM;
    } else {
      /* Core checked the topic against this ACL version before application dispatch. */
      if (runtime->config.journal && exchange && message->duplicate) {
        rc = flowie_control_release_admission_find(runtime->config.journal,
            runtime->config.environment, runtime->config.domain, exchange, handled);
      }
      if (rc == SALTS_OK && !*handled && runtime->active_processing &&
          runtime->config.processing->publish)
        rc = runtime->config.processing->publish(runtime->active_processing, &runtime->active,
            principal, message, exchange, handled);
    }
  }
  sync_rwlock_rdunlock(&runtime->gate);
  return rc;
}

int flowie_control_release_mqtt_validate(flowie_control_release_mqtt_t *runtime,
    const uint8_t *digest, const char *revision, uint64_t *base_generation, int *acl_changed) {
  if (!runtime || !digest || !revision || !base_generation || !acl_changed) return SALTS_EINVAL;
  const flowie_control_release_mqtt_config_t *config = &runtime->config;
  flowie_release_mqtt_candidate_t candidate = {0};
  sync_mutex_lock(&runtime->mutation);
  int rc = flowie_release_mqtt_prepare(config, digest, &candidate);
  if (rc == SALTS_OK) {
    const flowie_release_manifest_t *manifest = flowie_control_release_archive_manifest(candidate.archive);
    const uint64_t generation = runtime->active.generation;
    const int changed = !generation ||
        memcmp(runtime->active.acl_digest, candidate.digest, sizeof(candidate.digest)) != 0;
    flowie_security_realm_t *prepared = NULL;
    if (runtime->faulted) rc = SALTS_ESHUTDOWN;
    else if (!!generation != manifest->has_base_release ||
        (generation && memcmp(runtime->active.digest, manifest->base_release, sizeof(manifest->base_release))))
      rc = SALTS_ECANCELED;
    else if (changed && runtime->active.acl_version == INT64_MAX) rc = SALTS_ERANGE;
    else rc = flowie_release_mqtt_realm(config, &candidate, runtime->active.acl_version + changed, &prepared);
    if (rc == SALTS_OK) {
      void *processing = NULL;
      rc = flowie_release_processing_prepare(config, candidate.archive, &processing);
      if (rc == SALTS_OK) rc = flowie_release_processing_close(config, processing);
      else flowie_release_processing_discard(config, processing);
    }
    if (rc == SALTS_OK)
      rc = flowie_control_release_store_validation(config->store, config->environment, config->domain,
          digest, generation, revision);
    flowie_security_realm_destroy(prepared);
    if (rc == SALTS_OK) {
      *base_generation = generation;
      *acl_changed = changed;
    }
  }
  flowie_release_mqtt_candidate_clear(&candidate);
  sync_mutex_unlock(&runtime->mutation);
  return rc;
}

int flowie_control_release_mqtt_activate(flowie_control_release_mqtt_t *runtime,
    const uint8_t *digest, const char *actor, const char *key, uint64_t expected_generation,
    const char *revision, flowie_control_release_active_t *out, int *replayed) {
  if (!runtime || !digest || !revision || !out || !replayed) return SALTS_EINVAL;
  *out = (flowie_control_release_active_t){0};
  *replayed = 0;
  flowie_release_mqtt_candidate_t candidate = {0};
  const flowie_control_release_mqtt_config_t *config = &runtime->config;
  sync_mutex_lock(&runtime->mutation);
  flowie_security_realm_t *next = NULL, *retired = NULL;
  void *next_processing = NULL, *retired_processing = NULL;
  int rc = flowie_release_mqtt_prepare(config, digest, &candidate);
  if (rc != SALTS_OK) goto done;
  if (runtime->faulted) {
    rc = SALTS_ESHUTDOWN;
    goto done;
  }
  rc = flowie_control_release_store_activation_check(config->store, config->environment,
      config->domain, actor, key, expected_generation, revision, candidate.archive, out, replayed);
  if (rc != SALTS_OK || *replayed) goto done;

  const uint64_t version = runtime->active.acl_version +
      (!runtime->active.generation || memcmp(runtime->active.acl_digest, candidate.digest,
                                             sizeof(candidate.digest)) != 0);
  rc = flowie_release_mqtt_realm(config, &candidate, version, &next);
  if (rc == SALTS_OK)
    rc = flowie_release_processing_prepare(config, candidate.archive, &next_processing);
  if (rc != SALTS_OK) goto done;

  sync_rwlock_wrlock(&runtime->gate);
  if (runtime->active_processing && config->processing->activate) {
    const flowie_release_journal_scope_t scope = {config->environment, config->domain,
        runtime->active.generation, runtime->active.digest};
    int pending = 0;
    rc = flowie_control_release_pending(config->journal, &scope, &pending);
    if (rc == SALTS_OK && pending) rc = SALTS_EBUSY;
    if (rc != SALTS_OK) {
      sync_rwlock_wrunlock(&runtime->gate);
      goto done;
    }
  }
  if (config->journal) {
    const flowie_release_journal_scope_t scope = {config->environment, config->domain,
        runtime->active.generation + 1u, digest};
    rc = flowie_control_release_journal_verify(config->journal, &scope);
    if (rc != SALTS_OK) {
      sync_rwlock_wrunlock(&runtime->gate);
      goto done;
    }
  }
  rc = flowie_control_release_store_commit(config->store, config->environment, config->domain,
      actor, key, expected_generation, revision, candidate.archive, candidate.digest, out, replayed);
  if (rc == SALTS_OK && !*replayed && out->acl_version == version) {
    retired = runtime->active_realm;
    retired_processing = runtime->active_processing;
    runtime->active_realm = next;
    runtime->active_processing = next_processing;
    runtime->active = *out;
    if (runtime->backend_started && next_processing && config->processing->activate)
      config->processing->activate(next_processing, &runtime->active, 0);
    next = NULL;
    next_processing = NULL;
  } else if (rc == SALTS_OK && !*replayed) {
    runtime->faulted = 1;
    *out = (flowie_control_release_active_t){0};
    rc = SALTS_EIO;
  } else if (rc != SALTS_OK) {
    flowie_control_release_active_t committed;
    if (flowie_control_release_store_current(config->store, config->environment, config->domain,
        &committed) != SALTS_OK || committed.generation != runtime->active.generation ||
        memcmp(committed.digest, runtime->active.digest, sizeof(committed.digest)) != 0)
      runtime->faulted = 1;
  }
  sync_rwlock_wrunlock(&runtime->gate);
done:
  flowie_release_processing_discard(config, retired_processing);
  flowie_release_processing_discard(config, next_processing);
  flowie_security_realm_destroy(retired);
  flowie_security_realm_destroy(next);
  flowie_release_mqtt_candidate_clear(&candidate);
  sync_mutex_unlock(&runtime->mutation);
  return rc;
}
