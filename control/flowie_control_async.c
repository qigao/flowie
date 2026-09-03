#include "flowie_control_async_internal.h"

#include "platform.h"
#include "salts_coro.h"
#include "salts_error.h"
#include "salts_thread.h"

int flowie_control_async_wait(const atomic_int *completed, uint32_t timeout_ms) {
  uint64_t deadline_ms;
  if (!completed || timeout_ms == 0u) return SALTS_EINVAL;
  deadline_ms = salts_monotonic_ms() + timeout_ms;
  while (!atomic_load_explicit(completed, memory_order_acquire)) {
    if (salts_monotonic_ms() >= deadline_ms) return SALTS_ETIMEDOUT;
    if (coro_current_scheduler()) {
      if (coro_yield() != SALTS_OK) return SALTS_EIO;
    } else {
      salts_sleep_ms(1u);
    }
  }
  return SALTS_OK;
}
