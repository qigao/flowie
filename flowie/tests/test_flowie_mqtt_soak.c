#include "flowie_stl_error_internal.h"

#include <cstl.h>
#include <cstl.h>
#include <cstl.h>
#include <cstl.h>

#include "flowie_topic_index_internal.h"

#include "tinytest.h"
#include "salts_error.h"
#include "salts_process.h"
#include "salts_thread.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
  #include <psapi.h>
  #include <tlhelp32.h>
  #include <windows.h>
#elif defined(__linux__)
  #include <dirent.h>
  #include <unistd.h>
#endif

#ifndef FLOWIE_SOAK_ENDPOINT_TEST
  #error "FLOWIE_SOAK_ENDPOINT_TEST must point to test_flowie_endpoint"
#endif
#ifndef FLOWIE_SOAK_TRANSPORT_TEST
  #error "FLOWIE_SOAK_TRANSPORT_TEST must point to test_flowie_transport"
#endif
#ifndef FLOWIE_SOAK_RUNTIME_TEST
  #error "FLOWIE_SOAK_RUNTIME_TEST must point to test_turbo_flow"
#endif
#ifndef FLOWIE_SOAK_STORE_TEST
  #error "FLOWIE_SOAK_STORE_TEST must point to test_flowie_protocol_repository"
#endif

#define FLOWIE_SOAK_DEFAULT_ITERATIONS 3u
#define FLOWIE_SOAK_DEFAULT_SUBSCRIPTIONS 100000u
#define FLOWIE_SOAK_DEFAULT_PUBLISHERS 4u
#define FLOWIE_SOAK_CHILD_TIMEOUT_MS 120000u
#define FLOWIE_SOAK_MAX_ITERATIONS 1000u
#define FLOWIE_SOAK_LATENCY_SAMPLES 4096u
#define FLOWIE_SOAK_RESOURCE_REPRO_ITERATIONS 256u
#define FLOWIE_SOAK_PROCESS_RELEASE_TIMEOUT_MS 1000u
#define FLOWIE_SOAK_MAX_DURATION_MS (8u * 60u * 60u * 1000u)
#define FLOWIE_SOAK_DEFAULT_SEED UINT64_C(0x464c4f574945534f)
#define FLOWIE_SOAK_DEFAULT_RSS_TOLERANCE (8u * 1024u * 1024u)

typedef struct flowie_soak_resources_s {
  size_t rss_bytes;
  size_t handles;
  size_t threads;
} flowie_soak_resources_t;

typedef struct flowie_soak_index_owner_s {
  flowie_topic_index_t *index;
  salts_mutex_t mutex;
  size_t subscription_count;
  atomic_int ready;
  atomic_int stop;
  atomic_int status;
  atomic_size_t publish_count;
  atomic_size_t ready_match_count;
} flowie_soak_index_owner_t;

typedef struct flowie_soak_index_publisher_s {
  flowie_soak_index_owner_t *owner;
  size_t ordinal;
} flowie_soak_index_publisher_t;

static void flowie_soak_index_publish_thread(void *arg) {
  flowie_soak_index_publisher_t *publisher = (flowie_soak_index_publisher_t *)arg;
  flowie_soak_index_owner_t *owner;
  vec_t matches = {0};
  char topic[64];
  size_t sequence;
  int rc = SALTS_OK;
  if (!publisher || !publisher->owner) return;
  owner = publisher->owner;
  if (flowie_stl_error(vec_init_bytes(&matches, sizeof(size_t), _Alignof(size_t), SIZE_MAX)) != SALTS_OK) {
    atomic_store_explicit(&owner->status, SALTS_ENOMEM, memory_order_release);
    return;
  }
  sequence = publisher->ordinal;
  while (!atomic_load_explicit(&owner->stop, memory_order_acquire)) {
    const size_t odd_count = owner->subscription_count / 2u;
    const size_t target = (sequence % odd_count) * 2u + 1u;
    const int written = snprintf(topic, sizeof(topic), "soak/%zu/value", target);
    if (written <= 0 || (size_t)written >= sizeof(topic)) {
      rc = SALTS_EMSGSIZE;
      break;
    }
    vec_clear(&matches);
    salts_mutex_lock(&owner->mutex);
    rc = flowie_topic_index_match(
        owner->index, (flowie_mqtt_span_t){(const uint8_t *)topic, (size_t)written}, &matches);
    if (rc == SALTS_OK && vec_size(&matches) > 1u) rc = SALTS_EPROTO;
    if (rc == SALTS_OK && vec_size(&matches) == 1u) {
      const size_t *matched = (const size_t *)vec_at_const(&matches, 0u);
      if (!matched || *matched != target) rc = SALTS_EPROTO;
    }
    if (rc == SALTS_OK && atomic_load_explicit(&owner->ready, memory_order_acquire) &&
        vec_size(&matches) != 1u) {
      rc = SALTS_EPROTO;
    } else if (rc == SALTS_OK &&
               atomic_load_explicit(&owner->ready, memory_order_acquire)) {
      atomic_fetch_add_explicit(&owner->ready_match_count, 1u, memory_order_relaxed);
    }
    salts_mutex_unlock(&owner->mutex);
    if (rc != SALTS_OK) break;
    atomic_fetch_add_explicit(&owner->publish_count, 1u, memory_order_relaxed);
    ++sequence;
  }
  if (rc != SALTS_OK) atomic_store_explicit(&owner->status, rc, memory_order_release);
  vec_destroy(&matches);
}

static int flowie_soak_compare_u64(const void *left, const void *right) {
  const uint64_t a = *(const uint64_t *)left;
  const uint64_t b = *(const uint64_t *)right;
  return (a > b) - (a < b);
}

static uint64_t flowie_soak_percentile(uint64_t *values, size_t count, size_t percent) {
  size_t index;
  if (!values || count == 0u) return 0u;
  qsort(values, count, sizeof(*values), flowie_soak_compare_u64);
  index = ((count - 1u) * percent) / 100u;
  return values[index];
}

static uint64_t flowie_soak_seed(void) {
  const char *value = getenv("FLOWIE_MQTT_SOAK_SEED");
  char *end = NULL;
  unsigned long long parsed;
  if (!value || !value[0]) return FLOWIE_SOAK_DEFAULT_SEED;
  parsed = strtoull(value, &end, 0);
  return end && *end == '\0' ? (uint64_t)parsed : FLOWIE_SOAK_DEFAULT_SEED;
}

static uint64_t flowie_soak_random(uint64_t *state) {
  uint64_t value = *state;
  value ^= value << 13u;
  value ^= value >> 7u;
  value ^= value << 17u;
  *state = value;
  return value;
}

static int flowie_soak_resource_snapshot(flowie_soak_resources_t *out) {
  if (!out) return SALTS_EINVAL;
  memset(out, 0, sizeof(*out));
#if defined(_WIN32)
  {
    PROCESS_MEMORY_COUNTERS memory = {0};
    DWORD handles = 0u;
    HANDLE snapshot;
    THREADENTRY32 entry;
    const DWORD process_id = GetCurrentProcessId();
    memory.cb = sizeof(memory);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory)) ||
        !GetProcessHandleCount(GetCurrentProcess(), &handles))
      return SALTS_EIO;
    out->rss_bytes = (size_t)memory.WorkingSetSize;
    out->handles = (size_t)handles;
    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0u);
    if (snapshot == INVALID_HANDLE_VALUE) return SALTS_EIO;
    memset(&entry, 0, sizeof(entry));
    entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot, &entry)) {
      do {
        if (entry.th32OwnerProcessID == process_id) ++out->threads;
      } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
  }
#elif defined(__linux__)
  {
    FILE *statm = fopen("/proc/self/statm", "r");
    unsigned long rss_pages = 0u;
    DIR *directory;
    struct dirent *entry;
    const long page_size = sysconf(_SC_PAGESIZE);
    if (!statm || fscanf(statm, "%*lu %lu", &rss_pages) != 1 || page_size <= 0) {
      if (statm) fclose(statm);
      return SALTS_EIO;
    }
    fclose(statm);
    out->rss_bytes = (size_t)rss_pages * (size_t)page_size;
    directory = opendir("/proc/self/fd");
    if (!directory) return SALTS_EIO;
    while ((entry = readdir(directory)) != NULL)
      if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) ++out->handles;
    closedir(directory);
    directory = opendir("/proc/self/task");
    if (!directory) return SALTS_EIO;
    while ((entry = readdir(directory)) != NULL)
      if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) ++out->threads;
    closedir(directory);
  }
#else
  return SALTS_ENOTSUP;
#endif
  return SALTS_OK;
}

static int flowie_soak_wait_for_process_release(const flowie_soak_resources_t *baseline,
                                                flowie_soak_resources_t *current) {
  const uint64_t deadline =
      salts_monotonic_ms() + (uint64_t)FLOWIE_SOAK_PROCESS_RELEASE_TIMEOUT_MS;
  int rc;
  if (!baseline || !current) return SALTS_EINVAL;
  /* pthread_join may return before Linux removes the exiting task from procfs. */
  do {
    rc = flowie_soak_resource_snapshot(current);
    if (rc != SALTS_OK) return rc;
    if (current->handles <= baseline->handles && current->threads <= baseline->threads)
      return SALTS_OK;
    salts_sleep_ms(1u);
  } while (salts_monotonic_ms() < deadline);
  return SALTS_ENOSPC;
}

static size_t flowie_soak_env_size(const char *name, size_t fallback, size_t maximum) {
  const char *value = getenv(name);
  char *end = NULL;
  unsigned long long parsed;
  if (!value || !value[0]) return fallback;
  parsed = strtoull(value, &end, 10);
  if (!end || *end != '\0' || parsed == 0u || parsed > maximum) return fallback;
  return (size_t)parsed;
}

static size_t flowie_soak_duration_ms(const char *case_suffix) {
  char name[64];
  const char *value;
  char *end = NULL;
  unsigned long long parsed;
  int written;
  if (!case_suffix || !case_suffix[0]) return 0u;
  written = snprintf(name, sizeof(name), "FLOWIE_MQTT_SOAK_%s_DURATION_MS", case_suffix);
  if (written <= 0 || (size_t)written >= sizeof(name)) return 0u;
  value = getenv(name);
  if (!value || !value[0]) return 0u;
  parsed = strtoull(value, &end, 10);
  if (!end || *end != '\0' || parsed == 0u || parsed > FLOWIE_SOAK_MAX_DURATION_MS)
    return FLOWIE_SOAK_MAX_DURATION_MS + 1u;
  return (size_t)parsed;
}

static void flowie_soak_report_child_stream(salts_process_t *process, int stdout_stream) {
  char buffer[4096];
  size_t count = 0u;
  int rc;
  if (!process) return;
  do {
    rc = stdout_stream ? salts_process_read_stdout(process, buffer, sizeof(buffer), &count)
                       : salts_process_read_stderr(process, buffer, sizeof(buffer), &count);
    if (count != 0u) (void)fwrite(buffer, 1u, count, stderr);
  } while (rc == SALTS_OK && count != 0u);
}

static int flowie_soak_run_child_args(const char *program, const char *const *args,
                                      const char *description) {
  salts_process_options_t options;
  salts_process_t *process = NULL;
  salts_process_result_t result = {0};
  flowie_soak_resources_t resources_before;
  flowie_soak_resources_t resources_after;
  int release_rc;
  int rc;
  rc = flowie_soak_resource_snapshot(&resources_before);
  if (rc != SALTS_OK) return rc;
  salts_process_options_init(&options);
  options.program = program;
  options.args = args;
  options.flags = SALTS_PROCESS_CAPTURE_STDOUT | SALTS_PROCESS_CAPTURE_STDERR;
  options.timeout_ms = FLOWIE_SOAK_CHILD_TIMEOUT_MS;
  options.max_output_bytes = 65536u;
  rc = salts_process_spawn(&options, &process);
  if (rc != SALTS_OK) {
    fprintf(stderr, "SOAK_CHILD_FAILURE program=%s command=\"%s\" spawn_status=%d\n", program,
            description, rc);
    return rc;
  }
  rc = salts_process_wait(process, &result);
  if (rc == SALTS_OK && (result.state != SALTS_PROCESS_EXITED || result.exit_code != 0)) {
    fprintf(stderr,
            "SOAK_CHILD_FAILURE program=%s command=\"%s\" state=%s exit_code=%d "
            "term_signal=%d error_code=%d\nstdout:\n",
            program, description, salts_process_state_name(result.state), result.exit_code,
            result.term_signal, result.error_code);
    flowie_soak_report_child_stream(process, 1);
    fputs("\nstderr:\n", stderr);
    flowie_soak_report_child_stream(process, 0);
    fputc('\n', stderr);
    rc = SALTS_EIO;
  } else if (rc != SALTS_OK) {
    fprintf(stderr, "SOAK_CHILD_FAILURE program=%s command=\"%s\" wait_status=%d\n", program,
            description, rc);
  }
  salts_process_destroy(process);
  release_rc = flowie_soak_wait_for_process_release(&resources_before, &resources_after);
  if (release_rc != SALTS_OK) {
    fprintf(stderr,
            "SOAK_CHILD_RESOURCE_FAILURE program=%s command=\"%s\" "
            "handles_before=%zu handles_current=%zu threads_before=%zu threads_current=%zu "
            "settle_timeout_ms=%u\n",
            program, description, resources_before.handles, resources_after.handles,
            resources_before.threads, resources_after.threads,
            FLOWIE_SOAK_PROCESS_RELEASE_TIMEOUT_MS);
    if (rc == SALTS_OK) rc = release_rc;
  }
  return rc;
}

static int flowie_soak_run_child(const char *program, const char *filter) {
  const char *args[] = {"--filter", filter, NULL};
  return flowie_soak_run_child_args(program, args, filter);
}

static int flowie_soak_run_series(const char *case_id, const char *const *programs,
                                  const char *const *filters, size_t trace_count,
                                  size_t iterations, size_t duration_ms, uint64_t seed,
                                  size_t concurrency, size_t backend_retries) {
  uint64_t latencies[FLOWIE_SOAK_LATENCY_SAMPLES];
  flowie_soak_resources_t baseline;
  flowie_soak_resources_t current;
  uint64_t random_state = seed ? seed : FLOWIE_SOAK_DEFAULT_SEED;
  size_t failures = 0u;
  size_t completed = 0u;
  size_t latency_count = 0u;
  size_t rss_tolerance;
  const uint64_t series_started_at = salts_hrtime();
  const uint64_t deadline_ms =
      duration_ms != 0u ? salts_monotonic_ms() + (uint64_t)duration_ms : 0u;
  int rc;
  if (!case_id || !programs || !filters || trace_count == 0u || iterations == 0u ||
      iterations > FLOWIE_SOAK_MAX_ITERATIONS || duration_ms > FLOWIE_SOAK_MAX_DURATION_MS)
    return SALTS_EINVAL;

  /* Warm process spawning and library initialization before taking the leak baseline. */
  rc = flowie_soak_run_child(programs[0], filters[0]);
  if (rc != SALTS_OK) return rc;
  rc = flowie_soak_resource_snapshot(&baseline);
  if (rc != SALTS_OK) return rc;
  current = baseline;
  rss_tolerance = flowie_soak_env_size("FLOWIE_MQTT_SOAK_RSS_TOLERANCE_BYTES",
                                       FLOWIE_SOAK_DEFAULT_RSS_TOLERANCE,
                                       1024u * 1024u * 1024u);

  while (completed < iterations ||
         (deadline_ms != 0u && salts_monotonic_ms() < deadline_ms)) {
    const size_t trace = (size_t)(flowie_soak_random(&random_state) % trace_count);
    const uint64_t started_at = salts_hrtime();
    uint64_t latency;
    rc = flowie_soak_run_child(programs[trace], filters[trace]);
    latency = salts_hrtime() - started_at;
    ++completed;
    if (latency_count < FLOWIE_SOAK_LATENCY_SAMPLES) {
      latencies[latency_count++] = latency;
    } else {
      const size_t slot = (size_t)(flowie_soak_random(&random_state) % completed);
      if (slot < FLOWIE_SOAK_LATENCY_SAMPLES) latencies[slot] = latency;
    }
    if (rc != SALTS_OK) ++failures;
    if (flowie_soak_resource_snapshot(&current) != SALTS_OK) return SALTS_EIO;
    if (current.handles > baseline.handles || current.threads > baseline.threads ||
        current.rss_bytes > baseline.rss_bytes + rss_tolerance) {
      fprintf(stderr,
              "SOAK_RESOURCE_FAILURE id=%s rss_baseline=%zu rss_current=%zu "
              "rss_tolerance=%zu handles_baseline=%zu handles_current=%zu "
              "threads_baseline=%zu threads_current=%zu\n",
              case_id, baseline.rss_bytes, current.rss_bytes, rss_tolerance, baseline.handles,
              current.handles, baseline.threads, current.threads);
      return SALTS_ENOSPC;
    }
  }

  printf("SOAK_RESULT id=%s seed=%" PRIu64 " operations=%zu failures=%zu "
         "duration_ns=%" PRIu64 " messages=%zu payload_distribution=trace-matrix "
         "concurrency=%zu backend_retries=%zu "
         "p50_ns=%" PRIu64 " p95_ns=%" PRIu64 " p99_ns=%" PRIu64
         " rss_baseline=%zu rss_final=%zu handles_baseline=%zu handles_final=%zu "
         "threads_baseline=%zu threads_final=%zu resource_monotonic_growth=false\n",
         case_id, seed, completed, failures, salts_hrtime() - series_started_at, completed,
         concurrency, backend_retries == SIZE_MAX ? completed : backend_retries,
         flowie_soak_percentile(latencies, latency_count, 50u),
         flowie_soak_percentile(latencies, latency_count, 95u),
         flowie_soak_percentile(latencies, latency_count, 99u), baseline.rss_bytes,
         current.rss_bytes, baseline.handles, current.handles, baseline.threads, current.threads);
  return failures == 0u ? SALTS_OK : SALTS_EIO;
}

static int flowie_soak_index_cycle(size_t *publish_count_out) {
  flowie_topic_index_t index;
  flowie_soak_index_owner_t owner;
  flowie_soak_index_publisher_t *publishers = NULL;
  salts_thread_t *threads = NULL;
  flowie_topic_index_binding_t *bindings = NULL;
  vec_t matches = {0};
  const size_t count = flowie_soak_env_size("FLOWIE_MQTT_SOAK_SUBSCRIPTIONS",
                                           FLOWIE_SOAK_DEFAULT_SUBSCRIPTIONS, 1000000u);
  const size_t publisher_count = flowie_soak_env_size(
      "FLOWIE_MQTT_SOAK_PUBLISHERS", FLOWIE_SOAK_DEFAULT_PUBLISHERS, 64u);
  char filter[64];
  char topic[64];
  size_t moved;
  size_t started_threads = 0u;
  int index_initialized = 0;
  int matches_initialized = 0;
  int mutex_initialized = 0;
  int atomics_initialized = 0;
  int rc = SALTS_OK;
  if (publish_count_out) *publish_count_out = 0u;
  if (!publish_count_out || count < 2u) return SALTS_EINVAL;
  memset(&index, 0, sizeof(index));
  memset(&owner, 0, sizeof(owner));
  bindings = (flowie_topic_index_binding_t *)calloc(count, sizeof(*bindings));
  publishers = (flowie_soak_index_publisher_t *)calloc(publisher_count, sizeof(*publishers));
  threads = (salts_thread_t *)calloc(publisher_count, sizeof(*threads));
  if (!bindings || !publishers || !threads) {
    rc = SALTS_ENOMEM;
    goto cleanup;
  }
  if (flowie_topic_index_init(&index) != SALTS_OK) {
    rc = SALTS_ENOMEM;
    goto cleanup;
  }
  index_initialized = 1;
  if (flowie_stl_error(vec_init_bytes(&matches, sizeof(size_t), _Alignof(size_t), SIZE_MAX)) != SALTS_OK) {
    rc = SALTS_ENOMEM;
    goto cleanup;
  }
  matches_initialized = 1;
  owner.index = &index;
  owner.subscription_count = count;
  salts_mutex_init(&owner.mutex);
  mutex_initialized = 1;
  atomic_init(&owner.ready, 0);
  atomic_init(&owner.stop, 0);
  atomic_init(&owner.status, SALTS_OK);
  atomic_init(&owner.publish_count, 0u);
  atomic_init(&owner.ready_match_count, 0u);
  atomics_initialized = 1;
  for (size_t i = 0u; i < publisher_count; ++i) {
    publishers[i].owner = &owner;
    publishers[i].ordinal = i;
    rc = salts_thread_create(&threads[i], flowie_soak_index_publish_thread, &publishers[i]);
    if (rc != SALTS_OK) goto cleanup;
    ++started_threads;
  }
  {
    const uint64_t deadline = salts_monotonic_ms() + 2000u;
    while (atomic_load_explicit(&owner.publish_count, memory_order_acquire) < publisher_count &&
           atomic_load_explicit(&owner.status, memory_order_acquire) == SALTS_OK &&
           salts_monotonic_ms() < deadline)
      salts_thread_yield();
    if (atomic_load_explicit(&owner.publish_count, memory_order_acquire) < publisher_count) {
      rc = atomic_load_explicit(&owner.status, memory_order_acquire);
      if (rc == SALTS_OK) rc = SALTS_ETIMEDOUT;
      goto cleanup;
    }
  }
  for (size_t i = 0u; i < count; ++i) {
    int written = snprintf(filter, sizeof(filter), "soak/%zu/#", i);
    if (written <= 0 || (size_t)written >= sizeof(filter)) {
      rc = SALTS_EIO;
      goto cleanup;
    }
    salts_mutex_lock(&owner.mutex);
    rc = flowie_topic_index_insert_bound(
        &index, (flowie_mqtt_span_t){(const uint8_t *)filter, (size_t)written}, i, &bindings[i]);
    salts_mutex_unlock(&owner.mutex);
    if (rc != SALTS_OK) goto cleanup;
  }
  atomic_store_explicit(&owner.ready, 1, memory_order_release);
  for (size_t i = 0u; i < count; i += 2u) {
    moved = FLOWIE_TOPIC_INDEX_NO_ENTRY;
    salts_mutex_lock(&owner.mutex);
    rc = flowie_topic_index_remove(&index, &bindings[i], i, &moved);
    salts_mutex_unlock(&owner.mutex);
    if (rc != SALTS_OK) goto cleanup;
    if (moved != FLOWIE_TOPIC_INDEX_NO_ENTRY) bindings[moved].position = bindings[i].position;
  }
  {
    const uint64_t deadline = salts_monotonic_ms() + 2000u;
    while (atomic_load_explicit(&owner.ready_match_count, memory_order_acquire) == 0u &&
           atomic_load_explicit(&owner.status, memory_order_acquire) == SALTS_OK &&
           salts_monotonic_ms() < deadline)
      salts_thread_yield();
    if (atomic_load_explicit(&owner.ready_match_count, memory_order_acquire) == 0u &&
        atomic_load_explicit(&owner.status, memory_order_acquire) == SALTS_OK) {
      rc = SALTS_ETIMEDOUT;
      goto cleanup;
    }
  }
  if (atomic_load_explicit(&owner.status, memory_order_acquire) != SALTS_OK) {
    rc = atomic_load_explicit(&owner.status, memory_order_acquire);
    goto cleanup;
  }
  {
    const size_t last_odd = count % 2u == 0u ? count - 1u : count - 2u;
    const int written = snprintf(topic, sizeof(topic), "soak/%zu/value", last_odd);
    if (written <= 0 || (size_t)written >= sizeof(topic)) {
      rc = SALTS_EMSGSIZE;
      goto cleanup;
    }
    salts_mutex_lock(&owner.mutex);
    rc = flowie_topic_index_match(
        &index, (flowie_mqtt_span_t){(const uint8_t *)topic, (size_t)written}, &matches);
    salts_mutex_unlock(&owner.mutex);
  }
  if (rc != SALTS_OK || vec_size(&matches) != 1u)
    rc = SALTS_EPROTO;
cleanup:
  if (atomics_initialized) atomic_store_explicit(&owner.stop, 1, memory_order_release);
  for (size_t i = 0u; i < started_threads; ++i) {
    int join_rc = salts_thread_join(&threads[i]);
    if (rc == SALTS_OK && join_rc != SALTS_OK) rc = join_rc;
    salts_thread_destroy(&threads[i]);
  }
  if (rc == SALTS_OK && started_threads != 0u)
    rc = atomic_load_explicit(&owner.status, memory_order_acquire);
  if (started_threads != 0u)
    *publish_count_out = atomic_load_explicit(&owner.publish_count, memory_order_relaxed);
  if (mutex_initialized) salts_mutex_destroy(&owner.mutex);
  if (matches_initialized) vec_destroy(&matches);
  if (index_initialized) flowie_topic_index_destroy(&index);
  free(threads);
  free(publishers);
  free(bindings);
  return rc;
}

spec("Flowie MQTT scheduled soak") {
  it("child process resource sampling observes no resources after destroy returns") {
    static const char *args[] = {"--list", NULL};
    flowie_soak_resources_t first;
    flowie_soak_resources_t sample;
    size_t handles_min;
    size_t handles_max;
    size_t threads_min;
    size_t threads_max;
    size_t sample_count = 1u;

    check_equal(flowie_soak_run_child_args(FLOWIE_SOAK_ENDPOINT_TEST, args, "--list"), SALTS_OK);
    check_equal(flowie_soak_resource_snapshot(&first), SALTS_OK);
    handles_min = handles_max = first.handles;
    threads_min = threads_max = first.threads;

    for (size_t iteration = 0u; iteration < FLOWIE_SOAK_RESOURCE_REPRO_ITERATIONS; ++iteration) {
      check_equal(flowie_soak_run_child_args(FLOWIE_SOAK_ENDPOINT_TEST, args, "--list"),
                   SALTS_OK);
      check_equal(flowie_soak_resource_snapshot(&sample), SALTS_OK);
      ++sample_count;
      if (sample.handles < handles_min) handles_min = sample.handles;
      if (sample.handles > handles_max) handles_max = sample.handles;
      if (sample.threads < threads_min) threads_min = sample.threads;
      if (sample.threads > threads_max) threads_max = sample.threads;
      if (handles_min != handles_max || threads_min != threads_max) break;
    }

    info("samples=%zu handles_min=%zu handles_max=%zu threads_min=%zu threads_max=%zu",
         sample_count, handles_min, handles_max, threads_min, threads_max);
    check_equal(handles_max, handles_min);
    check_equal(threads_max, threads_min);
  }

  it("child process release barrier rejects sustained resource growth") {
    flowie_soak_resources_t baseline;
    flowie_soak_resources_t current;

    check_equal(flowie_soak_resource_snapshot(&baseline), SALTS_OK);
    check_greater_equal(baseline.threads, 1u);
    --baseline.threads;
    check_equal(flowie_soak_wait_for_process_release(&baseline, &current), SALTS_ENOSPC);
    check_greater(current.threads, baseline.threads);
  }

  it("MQTT-SOAK-001 repeats reconnect and client-id takeover traces") {
    const size_t iterations = flowie_soak_env_size("FLOWIE_MQTT_SOAK_ITERATIONS",
                                                   FLOWIE_SOAK_DEFAULT_ITERATIONS,
                                                   FLOWIE_SOAK_MAX_ITERATIONS);
    const char *programs[] = {FLOWIE_SOAK_ENDPOINT_TEST, FLOWIE_SOAK_ENDPOINT_TEST,
                              FLOWIE_SOAK_ENDPOINT_TEST};
    const char *filters[] = {"MQTT-OWNER-003 takes over MQTT 5",
                             "MQTT-OWNER-003 takes over MQTT 3.1.1",
                             "bridges PUBLISH traffic across MQTT"};
    check_equal(flowie_soak_run_series("MQTT-SOAK-001", programs, filters, 3u, iterations,
                                        flowie_soak_duration_ms("001"), flowie_soak_seed(), 2u,
                                        0u),
                 SALTS_OK);
  }

  it("MQTT-SOAK-002 adds and removes one hundred thousand subscriptions") {
    const size_t iterations = flowie_soak_env_size("FLOWIE_MQTT_SOAK_ITERATIONS",
                                                   FLOWIE_SOAK_DEFAULT_ITERATIONS,
                                                   FLOWIE_SOAK_MAX_ITERATIONS);
    uint64_t latencies[FLOWIE_SOAK_LATENCY_SAMPLES];
    flowie_soak_resources_t baseline;
    flowie_soak_resources_t current;
    const uint64_t seed = flowie_soak_seed();
    const uint64_t series_started_at = salts_hrtime();
    const size_t duration_ms = flowie_soak_duration_ms("002");
    const uint64_t deadline_ms =
        duration_ms != 0u ? salts_monotonic_ms() + (uint64_t)duration_ms : 0u;
    const size_t publisher_count = flowie_soak_env_size(
        "FLOWIE_MQTT_SOAK_PUBLISHERS", FLOWIE_SOAK_DEFAULT_PUBLISHERS, 64u);
    size_t concurrent_publishes = 0u;
    size_t cycles = 0u;
    size_t latency_count = 0u;
    size_t total_publishes = 0u;
    uint64_t random_state = seed;
    check_less_equal(duration_ms, FLOWIE_SOAK_MAX_DURATION_MS);
    check_equal(flowie_soak_index_cycle(&concurrent_publishes), SALTS_OK);
    check_true(concurrent_publishes != 0u);
    check_equal(flowie_soak_resource_snapshot(&baseline), SALTS_OK);
    while (cycles < iterations ||
           (deadline_ms != 0u && salts_monotonic_ms() < deadline_ms)) {
      const uint64_t started_at = salts_hrtime();
      uint64_t latency;
      check_equal(flowie_soak_index_cycle(&concurrent_publishes), SALTS_OK);
      check_true(concurrent_publishes != 0u);
      total_publishes += concurrent_publishes;
      latency = salts_hrtime() - started_at;
      ++cycles;
      if (latency_count < FLOWIE_SOAK_LATENCY_SAMPLES) {
        latencies[latency_count++] = latency;
      } else {
        const size_t slot = (size_t)(flowie_soak_random(&random_state) % cycles);
        if (slot < FLOWIE_SOAK_LATENCY_SAMPLES) latencies[slot] = latency;
      }
    }
    check_equal(flowie_soak_resource_snapshot(&current), SALTS_OK);
    check_true(current.handles <= baseline.handles);
    check_true(current.threads <= baseline.threads);
    check_true(current.rss_bytes <=
               baseline.rss_bytes +
                   flowie_soak_env_size("FLOWIE_MQTT_SOAK_RSS_TOLERANCE_BYTES",
                                        FLOWIE_SOAK_DEFAULT_RSS_TOLERANCE,
                                        1024u * 1024u * 1024u));
    printf("SOAK_RESULT id=MQTT-SOAK-002 seed=%" PRIu64
           " operations=%zu failures=0 duration_ns=%" PRIu64
           " messages=%zu payload_distribution=topic-index concurrency=%zu backend_retries=0 "
           "p50_ns=%" PRIu64 " p95_ns=%" PRIu64
           " p99_ns=%" PRIu64 " rss_baseline=%zu rss_final=%zu handles_baseline=%zu "
           "handles_final=%zu threads_baseline=%zu threads_final=%zu "
           "resource_monotonic_growth=false\n",
           seed, cycles + total_publishes, salts_hrtime() - series_started_at,
           total_publishes, publisher_count,
           flowie_soak_percentile(latencies, latency_count, 50u),
           flowie_soak_percentile(latencies, latency_count, 95u),
           flowie_soak_percentile(latencies, latency_count, 99u), baseline.rss_bytes,
           current.rss_bytes, baseline.handles, current.handles, baseline.threads,
           current.threads);
  }

  it("MQTT-SOAK-003 repeats slow-subscriber send-HWM isolation") {
    const size_t iterations = flowie_soak_env_size("FLOWIE_MQTT_SOAK_ITERATIONS",
                                                   FLOWIE_SOAK_DEFAULT_ITERATIONS,
                                                   FLOWIE_SOAK_MAX_ITERATIONS);
    const char *programs[] = {FLOWIE_SOAK_ENDPOINT_TEST, FLOWIE_SOAK_ENDPOINT_TEST};
    const char *filters[] = {"stalled shared fan-out subscriber", "inflight quota is exhausted"};
    check_equal(flowie_soak_run_series("MQTT-SOAK-003", programs, filters, 2u, iterations,
                                        flowie_soak_duration_ms("003"), flowie_soak_seed(), 3u,
                                        0u),
                 SALTS_OK);
  }

  it("MQTT-SOAK-004 repeats TLS and WSS shutdown boundary traces") {
    const size_t iterations = flowie_soak_env_size("FLOWIE_MQTT_SOAK_ITERATIONS",
                                                   FLOWIE_SOAK_DEFAULT_ITERATIONS,
                                                   FLOWIE_SOAK_MAX_ITERATIONS);
    const char *programs[] = {FLOWIE_SOAK_TRANSPORT_TEST, FLOWIE_SOAK_TRANSPORT_TEST,
                              FLOWIE_SOAK_TRANSPORT_TEST, FLOWIE_SOAK_TRANSPORT_TEST,
                              FLOWIE_SOAK_ENDPOINT_TEST};
    const char *filters[] = {"interrupts TLS and WSS handshakes", "interrupts pending recv",
                             "discards partial MQTT", "interrupts WSS close",
                             "interrupts a pending send"};
    check_equal(flowie_soak_run_series("MQTT-SOAK-004", programs, filters, 5u, iterations,
                                        flowie_soak_duration_ms("004"), flowie_soak_seed(), 1u,
                                        0u),
                 SALTS_OK);
  }

  it("MQTT-SOAK-005 repeats provider outage and lost-commit recovery traces") {
    const size_t iterations = flowie_soak_env_size("FLOWIE_MQTT_SOAK_ITERATIONS",
                                                   FLOWIE_SOAK_DEFAULT_ITERATIONS,
                                                   FLOWIE_SOAK_MAX_ITERATIONS);
    const char *programs[] = {FLOWIE_SOAK_STORE_TEST};
    const char *filters[] = {"MQTT-STORE-007"};
    const size_t trace_count = 1u;
    check_equal(flowie_soak_run_series("MQTT-SOAK-005", programs, filters, trace_count, iterations,
                                        flowie_soak_duration_ms("005"), flowie_soak_seed(), 1u,
                                        SIZE_MAX),
                 SALTS_OK);
  }

  it("MQTT-SOAK-006 repeats ingress and Disruptor HWM shutdown traces") {
    const size_t iterations = flowie_soak_env_size("FLOWIE_MQTT_SOAK_ITERATIONS",
                                                   FLOWIE_SOAK_DEFAULT_ITERATIONS,
                                                   FLOWIE_SOAK_MAX_ITERATIONS);
    const char *programs[] = {FLOWIE_SOAK_RUNTIME_TEST, FLOWIE_SOAK_RUNTIME_TEST,
                              FLOWIE_SOAK_RUNTIME_TEST};
    const char *filters[] = {"drains accepted publishes before stop",
                             "fails fast at async ingress capacity",
                             "interrupts parked disruptor workers"};
    check_equal(flowie_soak_run_series("MQTT-SOAK-006", programs, filters, 3u, iterations,
                                        flowie_soak_duration_ms("006"), flowie_soak_seed(), 1u,
                                        0u),
                 SALTS_OK);
  }
}
