#ifndef FLOWIE_TASK_GROUP_INTERNAL_H
#define FLOWIE_TASK_GROUP_INTERNAL_H

#include <core/error.h>
#include <threads/threads.h>

#include <stddef.h>

typedef struct flowie_task_group_s {
  sync_mutex_t mutex;
  sync_cond_t drained;
  size_t count;
  int admission_open;
} flowie_task_group_t;

static inline void flowie_task_group_init(flowie_task_group_t *group) {
  sync_mutex_init(&group->mutex);
  sync_cond_init(&group->drained);
  group->count = 0u;
  group->admission_open = 0;
}

static inline void flowie_task_group_destroy(flowie_task_group_t *group) {
  sync_cond_destroy(&group->drained);
  sync_mutex_destroy(&group->mutex);
}

static inline int flowie_task_group_open(flowie_task_group_t *group) {
  int rc = SALTS_OK;
  sync_mutex_lock(&group->mutex);
  if (group->count != 0u) {
    rc = SALTS_EBUSY;
  } else {
    group->admission_open = 1;
  }
  sync_mutex_unlock(&group->mutex);
  return rc;
}

static inline void flowie_task_group_close(flowie_task_group_t *group) {
  sync_mutex_lock(&group->mutex);
  group->admission_open = 0;
  sync_mutex_unlock(&group->mutex);
}

static inline int flowie_task_group_try_begin(flowie_task_group_t *group) {
  int rc = SALTS_OK;
  sync_mutex_lock(&group->mutex);
  if (!group->admission_open) {
    rc = SALTS_ESHUTDOWN;
  } else {
    group->count += 1u;
  }
  sync_mutex_unlock(&group->mutex);
  return rc;
}

static inline void flowie_task_group_end(flowie_task_group_t *group) {
  sync_mutex_lock(&group->mutex);
  if (group->count > 0u) group->count -= 1u;
  if (group->count == 0u) sync_cond_broadcast(&group->drained);
  sync_mutex_unlock(&group->mutex);
}

static inline void flowie_task_group_wait(flowie_task_group_t *group) {
  sync_mutex_lock(&group->mutex);
  while (group->count != 0u) sync_cond_wait(&group->drained, &group->mutex);
  sync_mutex_unlock(&group->mutex);
}

#endif
