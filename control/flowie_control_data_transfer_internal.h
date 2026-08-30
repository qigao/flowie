#ifndef FLOWIE_CONTROL_DATA_TRANSFER_INTERNAL_H
#define FLOWIE_CONTROL_DATA_TRANSFER_INTERNAL_H

#include "flowie_control_repository_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct flowie_control_data_transfer_result_s {
  size_t size;
  uint64_t source_revision;
  uint64_t target_revision;
  size_t user_count;
  size_t group_count;
  size_t membership_count;
  size_t role_count;
  size_t assignment_count;
  size_t policy_rule_count;
  int policy_published;
  int mutated;
} flowie_control_data_transfer_result_t;

#define FLOWIE_CONTROL_DATA_TRANSFER_RESULT_INIT                                                   \
  {sizeof(flowie_control_data_transfer_result_t), 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0, 0}

int flowie_control_data_export(const flowie_control_repository_t *repository,
                               const char *domain_id, const char *output_path,
                               flowie_control_data_transfer_result_t *result);
int flowie_control_data_import(const flowie_control_repository_t *repository,
                               const char *input_path, int dry_run,
                               flowie_control_data_transfer_result_t *result);

#ifdef __cplusplus
}
#endif

#endif
