#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#if defined(_WIN32) && !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0601
#endif

#include "flowie_affinity.h"
#include <salts/error_codes.h>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <errno.h>
#include <sched.h>
#endif

int flowie_affinity_bind(uint32_t cpu) {
#if defined(_WIN32)
  GROUP_AFFINITY affinity = {0};
  const uint32_t group = cpu / 64u;
  const uint32_t processor = cpu % 64u;
  DWORD error;
  if (group >= GetActiveProcessorGroupCount() ||
      processor >= sizeof(KAFFINITY) * 8u ||
      processor >= GetActiveProcessorCount((WORD)group)) return SALTS_ERANGE;
  affinity.Group = (WORD)group;
  affinity.Mask = (KAFFINITY)1u << processor;
  if (SetThreadGroupAffinity(GetCurrentThread(), &affinity, NULL)) return SALTS_OK;
  error = GetLastError();
  if (error == ERROR_INVALID_PARAMETER) return SALTS_EINVAL;
  if (error == ERROR_ACCESS_DENIED) return SALTS_EPERM;
  return SALTS_EIO;
#elif defined(__linux__)
  cpu_set_t mask;
  if (cpu >= CPU_SETSIZE) return SALTS_ERANGE;
  CPU_ZERO(&mask);
  CPU_SET(cpu, &mask);
  if (sched_setaffinity(0, sizeof(mask), &mask) == 0) return SALTS_OK;
  if (errno == EINVAL) return SALTS_EINVAL;
  if (errno == EPERM) return SALTS_EPERM;
  return SALTS_EIO;
#else
  (void)cpu;
  return SALTS_ENOTSUP;
#endif
}
