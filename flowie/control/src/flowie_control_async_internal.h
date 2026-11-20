#ifndef FLOWIE_CONTROL_ASYNC_INTERNAL_H
#define FLOWIE_CONTROL_ASYNC_INTERNAL_H

#include <stdatomic.h>
#include <stdint.h>

int flowie_control_async_wait(const atomic_int *completed, uint32_t timeout_ms);

#endif
