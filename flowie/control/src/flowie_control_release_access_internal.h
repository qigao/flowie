#ifndef FLOWIE_CONTROL_RELEASE_ACCESS_INTERNAL_H
#define FLOWIE_CONTROL_RELEASE_ACCESS_INTERNAL_H

#include "flowie_security.h"

/* Internal HTTPS authorization contract between a gateway and Control. */
#define FLOWIE_RELEASE_ACCESS_PATH "/internal/release/authorize"
#define FLOWIE_RELEASE_ACCESS_DOMAIN "X-Flowie-Target-Domain"
#define FLOWIE_RELEASE_ACCESS_ACTION "X-Flowie-Release-Action"
#define FLOWIE_RELEASE_ACCESS_ACTOR "X-Flowie-Release-Actor"
#define FLOWIE_RELEASE_ACCESS_READ "release.read"
#define FLOWIE_RELEASE_ACCESS_UPLOAD "release.upload"
#define FLOWIE_RELEASE_ACCESS_VALIDATE "release.validate"
#define FLOWIE_RELEASE_ACCESS_ACTIVATE "release.activate"
#define FLOWIE_RELEASE_ACCESS_PRAKTOR "release.praktor"
#define FLOWIE_RELEASE_ACCESS_ACTOR_MAX (2u * FLOWIE_SECURITY_ID_MAX + 5u)
#define FLOWIE_RELEASE_ACCESS_TOKEN_MAX 4096u

typedef struct flowie_release_access_request_s {
  const char *domain;
  const char *action;
  const char *authorization;
  const char *service_domain;
  const char *service_id;
} flowie_release_access_request_t;

typedef int (*flowie_release_authorize_fn)(void *context, const flowie_release_access_request_t *request,
    char actor[FLOWIE_RELEASE_ACCESS_ACTOR_MAX + 1u]);

#endif
