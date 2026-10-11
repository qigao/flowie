#include "cmeta_thread.h"

enum { FLOWIE_SUPERVISOR_TEST_WORKER_RUN_MS = 30000U };

int main(void) {
  cmeta_sleep_ms(FLOWIE_SUPERVISOR_TEST_WORKER_RUN_MS);
  return 0;
}
