#ifndef FLOWIE_CONTROL_RELEASE_MQTT_INTERNAL_H
#define FLOWIE_CONTROL_RELEASE_MQTT_INTERNAL_H

#include "flowie.h"
#include "flowie_control_release_store_internal.h"

typedef struct flowie_control_release_mqtt_s flowie_control_release_mqtt_t;

/* Deployment-owned execution provider. The release owner takes any non-NULL handle returned
 * by prepare and closes it exactly once. A successful prepare returns a ready, quiescent
 * handle. Close releases all ownership even if it reports a shutdown or cleanup error.
 * Both calls have bounded deadlines and must not call back into release management. */
typedef struct flowie_control_release_processing_s {
  void *context;
  int (*prepare)(void *context, const flowie_release_archive_t *archive, void **out);
  int (*close)(void *context, void *handle);
  /* Durably accept one authenticated publish. Leave handled zero outside the pipeline. */
  int (*publish)(void *handle, const flowie_control_release_active_t *active,
      const flowie_security_principal_t *principal, const flowie_mqtt_publish_view_t *message,
      const uint8_t *exchange, int *handled);
  /* Notify the backend of a published generation without running work on the caller. */
  void (*activate)(void *handle, const flowie_control_release_active_t *active, int recovered);
} flowie_control_release_processing_t;

typedef struct flowie_control_release_mqtt_config_s {
  flowie_control_release_store_t *store;
  const char *environment;
  const char *domain;
  const char *realm_name;
  const char *resource_uid;
  const char *owner_name;
  const char *auth_method;
  const flowie_security_auth_provider_t *identity_provider;
  const salts_crypto_sm2_cms_verify_config *signer;
  const flowie_control_release_processing_t *processing;
  flowie_control_release_store_t *journal;
} flowie_control_release_mqtt_config_t;

/* Own the live ACL and optional RulesForge candidate for one MQTT Domain. Configuration and providers
 * are borrowed through close. Run just one serving authority for each repository scope.
 * Revalidates the committed release during open. An empty repository denies MQTT admission. */
int flowie_control_release_mqtt_open(const flowie_control_release_mqtt_config_t *config,
                                     flowie_control_release_mqtt_t **out);
void flowie_control_release_mqtt_close(flowie_control_release_mqtt_t *runtime);

/* Borrow until close. Stop and destroy the endpoint before closing its policy owner. */
const flowie_endpoint_security_binding_t *flowie_control_release_mqtt_binding(
    const flowie_control_release_mqtt_t *runtime);

/* Borrow the exact store, trust and scope bound to live MQTT admission. */
const flowie_control_release_mqtt_config_t *flowie_control_release_mqtt_config(
    const flowie_control_release_mqtt_t *runtime);
int flowie_control_release_mqtt_status(flowie_control_release_mqtt_t *runtime,
    flowie_control_release_active_t *committed, int *ready);
void flowie_control_release_mqtt_resume(flowie_control_release_mqtt_t *runtime);

/* Verify the ACL version under the release gate and complete durable admission. */
int flowie_control_release_mqtt_publish(flowie_control_release_mqtt_t *runtime,
    const flowie_security_principal_t *principal, const flowie_mqtt_publish_view_t *message,
    const uint8_t *exchange, int *handled);

/* Reverify and prepare against the live base without publishing it. Record a
 * receipt for the exact base generation and trust/resource policy revision. */
int flowie_control_release_mqtt_validate(flowie_control_release_mqtt_t *runtime,
    const uint8_t *digest, const char *revision, uint64_t *base_generation, int *acl_changed);

/* Activate an uploaded package and switch the ACL and execution handle after the durable commit.
 * The authenticated management caller provides actor, idempotency key, expected generation and
 * the validation policy revision to check in the transaction.
 * Requires an execution provider for packages declaring RulesForge. */
int flowie_control_release_mqtt_activate(flowie_control_release_mqtt_t *runtime,
    const uint8_t *digest, const char *actor, const char *key, uint64_t expected_generation,
    const char *revision, flowie_control_release_active_t *out, int *replayed);

#endif
