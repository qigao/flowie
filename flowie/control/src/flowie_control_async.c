#include "flowie_control_async_internal.h"

#include <core/platform.h>
#include <salts/coro.h>
#include <core/error.h>
#include <threads/threads.h>

int flowie_control_async_wait(const atomic_int *completed, uint32_t timeout_ms) {
  uint64_t deadline_ms;
  if (!completed || timeout_ms == 0u) return SALTS_EINVAL;
  deadline_ms = monotonic_ms() + timeout_ms;
  while (!atomic_load_explicit(completed, memory_order_acquire)) {
    if (monotonic_ms() >= deadline_ms) return SALTS_ETIMEDOUT;
    if (coro_current_scheduler()) {
      if (coro_yield() != SALTS_OK) return SALTS_EIO;
    } else {
      sleep_ms(1u);
    }
  }
  return SALTS_OK;
}
