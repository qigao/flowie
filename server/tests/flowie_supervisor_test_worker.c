#include "salts_thread.h"

enum { FLOWIE_SUPERVISOR_TEST_WORKER_RUN_MS = 30000U };

int main(void) {
  salts_sleep_ms(FLOWIE_SUPERVISOR_TEST_WORKER_RUN_MS);
  return 0;
}
