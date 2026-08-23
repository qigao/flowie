#include "flowie_stl_error_internal.h"

#include <turbostl/deque.h>
#include <turbostl/hash_map.h>
#include <turbostl/hash_set.h>
#include <turbostl/vec.h>

#include "flowie_topic_index_internal.h"

#include "tinytest.h"
#include "turbo_error.h"

#include <string.h>

static flowie_mqtt_span_t flowie_topic_test_span(const char *value) {
  return (flowie_mqtt_span_t){(const uint8_t *)value, strlen(value)};
}

static int flowie_topic_test_contains(const vec_t *matches, size_t expected) {
  for (size_t i = 0u; i < vec_size(matches); ++i) {
    const size_t *value = (const size_t *)vec_at_const(matches, i);
    if (value && *value == expected) return 1;
  }
  return 0;
}

static int flowie_topic_test_collect(void *ctx, size_t entry_index) {
  return flowie_stl_error(vec_push((vec_t *)ctx, &entry_index));
}

spec("flowie topic index") {
  it("indexes exact wildcard shared and system topic filters") {
    static const char *const filters[] = {
        "sensors/+/temp", "sensors/#", "sensors/a/temp",
        "$SYS/#",         "#",         "$share/workers/sensors/+/temp"};
    flowie_topic_index_t index;
    vec_t matches = {0};
    memset(&index, 0, sizeof(index));
    check_equal(flowie_topic_index_init(&index), TURBO_OK);
    check_equal(flowie_stl_error(vec_init_bytes(&matches, sizeof(size_t), _Alignof(size_t), SIZE_MAX)), TURBO_OK);
    for (size_t i = 0u; i < sizeof(filters) / sizeof(filters[0]); ++i) {
      check_equal(flowie_topic_index_insert(&index, flowie_topic_test_span(filters[i]), i),
                   TURBO_OK);
    }

    check_equal(
        flowie_topic_index_match(&index, flowie_topic_test_span("sensors/a/temp"), &matches),
        TURBO_OK);
    check_equal(vec_size(&matches), 5u);
    check_true(flowie_topic_test_contains(&matches, 0u));
    check_true(flowie_topic_test_contains(&matches, 1u));
    check_true(flowie_topic_test_contains(&matches, 2u));
    check_true(flowie_topic_test_contains(&matches, 4u));
    check_true(flowie_topic_test_contains(&matches, 5u));
    check_false(flowie_topic_test_contains(&matches, 3u));

    vec_clear(&matches);
    check_equal(flowie_topic_index_match(&index, flowie_topic_test_span("$SYS/status"), &matches),
                 TURBO_OK);
    check_equal(vec_size(&matches), 1u);
    check_true(flowie_topic_test_contains(&matches, 3u));
    check_false(flowie_topic_test_contains(&matches, 4u));

    vec_destroy(&matches);
    flowie_topic_index_destroy(&index);
  }

  it("rejects malformed filters and invalid arguments") {
    flowie_topic_index_t index;
    vec_t matches = {0};
    memset(&index, 0, sizeof(index));
    check_equal(flowie_topic_index_init(&index), TURBO_OK);
    check_equal(flowie_stl_error(vec_init_bytes(&matches, sizeof(size_t), _Alignof(size_t), SIZE_MAX)), TURBO_OK);
    check_equal(flowie_topic_index_insert(&index, flowie_topic_test_span("a/#/b"), 1u),
                 TURBO_EPROTO);
    check_equal(flowie_topic_index_insert(&index, flowie_topic_test_span("$share/group"), 2u),
                 TURBO_EPROTO);
    check_equal(flowie_topic_index_match(&index, (flowie_mqtt_span_t){NULL, 0u}, &matches),
                 TURBO_EINVAL);
    vec_destroy(&matches);
    flowie_topic_index_destroy(&index);
  }

  it("visits concrete topic matches through an immutable query") {
    static const char *const filters[] = {"sensors/+/temp", "sensors/#", "$SYS/#", "#"};
    flowie_topic_index_t index;
    vec_t matches = {0};
    memset(&index, 0, sizeof(index));
    check_equal(flowie_topic_index_init(&index), TURBO_OK);
    check_equal(flowie_stl_error(vec_init_bytes(&matches, sizeof(size_t), _Alignof(size_t), SIZE_MAX)), TURBO_OK);
    for (size_t i = 0u; i < sizeof(filters) / sizeof(filters[0]); ++i)
      check_equal(flowie_topic_index_insert(&index, flowie_topic_test_span(filters[i]), i),
                   TURBO_OK);

    check_equal(flowie_topic_index_visit_topic(&index, flowie_topic_test_span("sensors/a/temp"),
                                                flowie_topic_test_collect, &matches),
                 TURBO_OK);
    check_equal(vec_size(&matches), 3u);
    check_true(flowie_topic_test_contains(&matches, 0u));
    check_true(flowie_topic_test_contains(&matches, 1u));
    check_true(flowie_topic_test_contains(&matches, 3u));

    vec_clear(&matches);
    check_equal(flowie_topic_index_visit_topic(&index, flowie_topic_test_span("$SYS/status"),
                                                flowie_topic_test_collect, &matches),
                 TURBO_OK);
    check_equal(vec_size(&matches), 1u);
    check_true(flowie_topic_test_contains(&matches, 2u));
    vec_destroy(&matches);
    flowie_topic_index_destroy(&index);
  }

  it("visits only policy filters that contain the requested filter language") {
    static const char *const filters[] = {
        "root-a/+/events/#", "root-a/#", "root-a/device-1/events", "#", "$SYS/#", "root-a/+/+"};
    flowie_topic_index_t index;
    vec_t matches = {0};
    memset(&index, 0, sizeof(index));
    check_equal(flowie_topic_index_init(&index), TURBO_OK);
    check_equal(flowie_stl_error(vec_init_bytes(&matches, sizeof(size_t), _Alignof(size_t), SIZE_MAX)), TURBO_OK);
    for (size_t i = 0u; i < sizeof(filters) / sizeof(filters[0]); ++i)
      check_equal(flowie_topic_index_insert(&index, flowie_topic_test_span(filters[i]), i),
                   TURBO_OK);

    check_equal(flowie_topic_index_visit_containing_filters(
                     &index, flowie_topic_test_span("root-a/+/events/temperature"),
                     flowie_topic_test_collect, &matches),
                 TURBO_OK);
    check_equal(vec_size(&matches), 3u);
    check_true(flowie_topic_test_contains(&matches, 0u));
    check_true(flowie_topic_test_contains(&matches, 1u));
    check_true(flowie_topic_test_contains(&matches, 3u));

    vec_clear(&matches);
    check_equal(flowie_topic_index_visit_containing_filters(
                     &index, flowie_topic_test_span("root-a/device-1/events"),
                     flowie_topic_test_collect, &matches),
                 TURBO_OK);
    check_equal(vec_size(&matches), 5u);
    check_true(flowie_topic_test_contains(&matches, 0u));
    check_true(flowie_topic_test_contains(&matches, 1u));
    check_true(flowie_topic_test_contains(&matches, 2u));
    check_true(flowie_topic_test_contains(&matches, 3u));
    check_true(flowie_topic_test_contains(&matches, 5u));

    vec_clear(&matches);
    check_equal(flowie_topic_index_visit_containing_filters(&index,
                                                             flowie_topic_test_span("root-a/#"),
                                                             flowie_topic_test_collect, &matches),
                 TURBO_OK);
    check_equal(vec_size(&matches), 2u);
    check_true(flowie_topic_test_contains(&matches, 1u));
    check_true(flowie_topic_test_contains(&matches, 3u));

    vec_clear(&matches);
    check_equal(flowie_topic_index_visit_containing_filters(
                     &index, flowie_topic_test_span("$SYS/+"), flowie_topic_test_collect, &matches),
                 TURBO_OK);
    check_equal(vec_size(&matches), 1u);
    check_true(flowie_topic_test_contains(&matches, 4u));

    vec_clear(&matches);
    check_equal(flowie_topic_index_visit_containing_filters(
                     &index, flowie_topic_test_span("$share/workers/root-a/+/events/temperature"),
                     flowie_topic_test_collect, &matches),
                 TURBO_OK);
    check_equal(vec_size(&matches), 3u);
    vec_destroy(&matches);
    flowie_topic_index_destroy(&index);
  }

  it("removes bound entries updates swapped positions and prunes empty branches") {
    flowie_topic_index_t index;
    flowie_topic_index_binding_t bindings[4];
    vec_t matches = {0};
    size_t moved = FLOWIE_TOPIC_INDEX_NO_ENTRY;
    size_t removed_position;
    memset(&index, 0, sizeof(index));
    memset(bindings, 0, sizeof(bindings));
    check_equal(flowie_topic_index_init(&index), TURBO_OK);
    check_equal(flowie_stl_error(vec_init_bytes(&matches, sizeof(size_t), _Alignof(size_t), SIZE_MAX)), TURBO_OK);
    check_equal(flowie_topic_index_insert_bound(&index, flowie_topic_test_span("devices/a"), 0u,
                                                 &bindings[0]),
                 TURBO_OK);
    check_equal(flowie_topic_index_insert_bound(&index, flowie_topic_test_span("devices/a"), 1u,
                                                 &bindings[1]),
                 TURBO_OK);
    check_equal(flowie_topic_index_insert_bound(&index, flowie_topic_test_span("devices/+"), 2u,
                                                 &bindings[2]),
                 TURBO_OK);
    check_equal(flowie_topic_index_insert_bound(
                     &index, flowie_topic_test_span("$share/workers/devices/#"), 3u, &bindings[3]),
                 TURBO_OK);

    removed_position = bindings[0].position;
    check_equal(flowie_topic_index_remove(&index, &bindings[0], 0u, &moved), TURBO_OK);
    check_equal(moved, 1u);
    bindings[moved].position = removed_position;
    check_equal(flowie_topic_index_match(&index, flowie_topic_test_span("devices/a"), &matches),
                 TURBO_OK);
    check_equal(vec_size(&matches), 3u);
    check_true(flowie_topic_test_contains(&matches, 1u));
    check_true(flowie_topic_test_contains(&matches, 2u));
    check_true(flowie_topic_test_contains(&matches, 3u));

    vec_clear(&matches);
    check_equal(flowie_topic_index_remove(&index, &bindings[1], 1u, &moved), TURBO_OK);
    check_equal(moved, FLOWIE_TOPIC_INDEX_NO_ENTRY);
    check_equal(flowie_topic_index_remove(&index, &bindings[2], 2u, &moved), TURBO_OK);
    check_equal(flowie_topic_index_remove(&index, &bindings[3], 3u, &moved), TURBO_OK);
    check_equal(flowie_topic_index_match(&index, flowie_topic_test_span("devices/a"), &matches),
                 TURBO_OK);
    check_equal(vec_size(&matches), 0u);
    check_equal(vec_size(&index.nodes), 1u);

    check_equal(flowie_topic_index_insert_bound(&index, flowie_topic_test_span("devices/a"), 0u,
                                                 &bindings[0]),
                 TURBO_OK);
    check_equal(flowie_topic_index_match(&index, flowie_topic_test_span("devices/a"), &matches),
                 TURBO_OK);
    check_equal(vec_size(&matches), 1u);
    check_true(flowie_topic_test_contains(&matches, 0u));
    check_equal(flowie_topic_index_remove(&index, &bindings[0], 0u, &moved), TURBO_OK);
    check_equal(vec_size(&index.nodes), 1u);

    vec_destroy(&matches);
    flowie_topic_index_destroy(&index);
  }
}
