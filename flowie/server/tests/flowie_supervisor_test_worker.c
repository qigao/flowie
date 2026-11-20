#include <threads/threads.h>

enum { FLOWIE_SUPERVISOR_TEST_WORKER_RUN_MS = 30000U };

int main(void) {
  sleep_ms(FLOWIE_SUPERVISOR_TEST_WORKER_RUN_MS);
  return 0;
}
