#ifndef FLOWIE_TASK_GROUP_INTERNAL_H
#define FLOWIE_TASK_GROUP_INTERNAL_H

#include "salts_error.h"
#include "salts_thread.h"

#include <stddef.h>

typedef struct flowie_task_group_s {
  salts_mutex_t mutex;
  salts_cond_t drained;
  size_t count;
  int admission_open;
} flowie_task_group_t;

static inline void flowie_task_group_init(flowie_task_group_t *group) {
  salts_mutex_init(&group->mutex);
  salts_cond_init(&group->drained);
  group->count = 0u;
  group->admission_open = 0;
}

static inline void flowie_task_group_destroy(flowie_task_group_t *group) {
  salts_cond_destroy(&group->drained);
  salts_mutex_destroy(&group->mutex);
}

static inline int flowie_task_group_open(flowie_task_group_t *group) {
  int rc = SALTS_OK;
  salts_mutex_lock(&group->mutex);
  if (group->count != 0u) {
    rc = SALTS_EBUSY;
  } else {
    group->admission_open = 1;
  }
  salts_mutex_unlock(&group->mutex);
  return rc;
}

static inline void flowie_task_group_close(flowie_task_group_t *group) {
  salts_mutex_lock(&group->mutex);
  group->admission_open = 0;
  salts_mutex_unlock(&group->mutex);
}

static inline int flowie_task_group_try_begin(flowie_task_group_t *group) {
  int rc = SALTS_OK;
  salts_mutex_lock(&group->mutex);
  if (!group->admission_open) {
    rc = SALTS_ESHUTDOWN;
  } else {
    group->count += 1u;
  }
  salts_mutex_unlock(&group->mutex);
  return rc;
}

static inline void flowie_task_group_end(flowie_task_group_t *group) {
  salts_mutex_lock(&group->mutex);
  if (group->count > 0u) group->count -= 1u;
  if (group->count == 0u) salts_cond_broadcast(&group->drained);
  salts_mutex_unlock(&group->mutex);
}

static inline void flowie_task_group_wait(flowie_task_group_t *group) {
  salts_mutex_lock(&group->mutex);
  while (group->count != 0u) salts_cond_wait(&group->drained, &group->mutex);
  salts_mutex_unlock(&group->mutex);
}

#endif
