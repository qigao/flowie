#include "flowie_ingress_internal.h"

#include "tinytest.h"
#include <core/error.h>
#include <core/tstr.h>

typedef struct flowie_ingress_capture_s {
  size_t calls;
  uint32_t types[4];
  uint32_t flags[4];
  int buffer_backed[4];
  int owned_payload[4];
  tstr packets[4];
  int result;
} flowie_ingress_capture_t;

typedef struct flowie_ingress_completion_probe_s {
  flowie_ingress_capture_t *capture;
  size_t calls;
  size_t captured_before_callback;
  int status;
} flowie_ingress_completion_probe_t;

static int flowie_ingress_capture_dispatch(void *ctx, flowie_message_t *message,
                                           flowie_publish_result_t *result) {
  flowie_ingress_capture_t *capture = (flowie_ingress_capture_t *)ctx;
  size_t index;
  if (!capture || !message || !result) return SALTS_EINVAL;
  index = capture->calls++;
  if (index < 4u) {
    capture->types[index] = message->type;
    capture->flags[index] = message->flags;
    capture->buffer_backed[index] = message->buffer != NULL;
    capture->owned_payload[index] = message->owned_payload != NULL;
    capture->packets[index] = tstr_new_len(message->payload.data, message->payload.len);
    if (!capture->packets[index]) {
      result->status = SALTS_ENOMEM;
      return SALTS_ENOMEM;
    }
  }
  result->status = capture->result;
  return capture->result;
}

static void flowie_ingress_capture_cleanup(flowie_ingress_capture_t *capture) {
  for (size_t i = 0u; i < 4u; ++i) tstr_freep(&capture->packets[i]);
}

static int flowie_ingress_completion(void *ctx, flowie_ingress_t *ingress,
                                     const flowie_message_t *message,
                                     const flowie_publish_result_t *result) {
  flowie_ingress_completion_probe_t *probe = (flowie_ingress_completion_probe_t *)ctx;
  (void)ingress;
  if (!probe || !message || !result) return SALTS_EINVAL;
  probe->calls += 1u;
  probe->captured_before_callback = probe->capture->calls;
  probe->status = result->status;
  return result->status;
}

static flowie_ingress_t *flowie_ingress_for(flowie_ingress_capture_t *capture,
                                            size_t max_packet_size) {
  flowie_ingress_config_t config = FLOWIE_INGRESS_CONFIG_INIT;
  config.dispatch = flowie_ingress_capture_dispatch;
  config.dispatch_ctx = capture;
  config.max_packet_size = max_packet_size;
  return flowie_ingress_create(&config);
}

spec("flowie MQTT connection ingress") {
  it("keeps a half packet buffered and dispatches one buffer-backed packet when complete") {
    static const uint8_t connect[] = {0x10, 0x17, 0x00, 0x04, 'M',  'Q',  'T',  'T', 0x05,
                                      0x02, 0x00, 0x3c, 0x07, 0x15, 0x00, 0x04, 'n', 'o',
                                      'n',  'e',  0x00, 0x03, 'c',  'l',  'i'};
    static const uint8_t packet[] = {0x30, 0x07, 0x00, 0x01, 'a', 0x00, 'o', 'k', '!'};
    flowie_ingress_capture_t capture = {0};
    flowie_ingress_t *ingress = flowie_ingress_for(&capture, 32u);
    size_t published = 99u;
    uint32_t expected_flags = 0u;

    check_not_null(ingress);
    check_equal(flowie_ingress_feed(ingress, connect, sizeof(connect), &published), SALTS_OK);
    check_equal(published, 1u);
    check_equal(flowie_mqtt_message_flags_encode(FLOWIE_MQTT_VERSION_5, 0u, &expected_flags),
                SALTS_OK);
    check_equal(capture.flags[0], expected_flags);
    check_equal(flowie_ingress_feed(ingress, packet, 3u, &published), SALTS_OK);
    check_equal(published, 0u);
    check_equal(capture.calls, 1u);
    check_equal(flowie_ingress_buffered_bytes(ingress), 3u);
    check_equal(flowie_ingress_feed(ingress, packet + 3u, sizeof(packet) - 3u, &published),
                SALTS_OK);
    check_equal(published, 1u);
    check_equal(capture.calls, 2u);
    check_equal(capture.types[1], FLOWIE_MQTT_PACKET_PUBLISH);
    check_equal(capture.flags[1], expected_flags);
    check_equal(capture.buffer_backed[1], 1);
    check_equal(capture.owned_payload[1], 0);
    check_equal(flowie_ingress_buffered_bytes(ingress), 0u);
    check_equal(tstr_len(capture.packets[1]), sizeof(packet));
    check_equal(capture.packets[1], packet, sizeof(packet));

    flowie_ingress_destroy(ingress);
    check_equal(capture.packets[1], packet, sizeof(packet));
    flowie_ingress_capture_cleanup(&capture);
  }

  it("dispatches every complete packet from one sticky receive chunk") {
    static const uint8_t packets[] = {0x10, 0x17, 0x00, 0x04, 'M',  'Q',  'T',  'T',  0x05, 0x02,
                                      0x00, 0x3c, 0x07, 0x15, 0x00, 0x04, 'n',  'o',  'n',  'e',
                                      0x00, 0x03, 'c',  'l',  'i',  0xc0, 0x00, 0xc0, 0x00};
    flowie_ingress_capture_t capture = {0};
    flowie_ingress_t *ingress = flowie_ingress_for(&capture, sizeof(packets));
    size_t published = 0u;

    check_not_null(ingress);
    check_equal(flowie_ingress_feed(ingress, packets, sizeof(packets), &published), SALTS_OK);
    check_equal(published, 3u);
    check_equal(capture.calls, 3u);
    check_equal(capture.types[0], FLOWIE_MQTT_PACKET_CONNECT);
    check_equal(capture.types[1], FLOWIE_MQTT_PACKET_PINGREQ);
    check_equal(capture.types[2], FLOWIE_MQTT_PACKET_PINGREQ);
    check_equal(capture.packets[1], packets + sizeof(packets) - 4u, 2u);
    check_equal(capture.packets[2], packets + sizeof(packets) - 2u, 2u);

    flowie_ingress_destroy(ingress);
    flowie_ingress_capture_cleanup(&capture);
  }

  it("transfers packet ownership before invoking the same-lane dispatch callback") {
    static const uint8_t packet[] = {0x10, 0x17, 0x00, 0x04, 'M',  'Q',  'T',  'T', 0x05,
                                     0x02, 0x00, 0x3c, 0x07, 0x15, 0x00, 0x04, 'n', 'o',
                                     'n',  'e',  0x00, 0x03, 'c',  'l',  'i'};
    flowie_ingress_capture_t capture = {0};
    flowie_ingress_t *ingress = flowie_ingress_for(&capture, sizeof(packet));
    size_t published = 0u;

    check_not_null(ingress);
    check_equal(flowie_ingress_feed(ingress, packet, sizeof(packet), &published), SALTS_OK);
    check_equal(published, 1u);
    check_equal(capture.calls, 1u);
    check_equal(capture.buffer_backed[0], 1);
    check_equal(capture.owned_payload[0], 0);
    check_equal(capture.packets[0], packet, sizeof(packet));

    flowie_ingress_destroy(ingress);
    flowie_ingress_capture_cleanup(&capture);
  }

  it("reports completion only after the dispatch callback returns") {
    static const uint8_t packet[] = {0x10, 0x17, 0x00, 0x04, 'M',  'Q',  'T',  'T', 0x05,
                                     0x02, 0x00, 0x3c, 0x07, 0x15, 0x00, 0x04, 'n', 'o',
                                     'n',  'e',  0x00, 0x03, 'c',  'l',  'i'};
    flowie_ingress_capture_t capture = {0};
    flowie_ingress_completion_probe_t probe = {&capture, 0u, 0u, SALTS_EINVAL};
    flowie_ingress_config_t config = FLOWIE_INGRESS_CONFIG_INIT;
    flowie_ingress_t *ingress;
    size_t published = 0u;

    config.dispatch = flowie_ingress_capture_dispatch;
    config.dispatch_ctx = &capture;
    config.max_packet_size = sizeof(packet);
    config.publish_complete = flowie_ingress_completion;
    config.prepare_ctx = &probe;
    ingress = flowie_ingress_create(&config);
    check_not_null(ingress);
    check_equal(flowie_ingress_feed(ingress, packet, sizeof(packet), &published), SALTS_OK);
    check_equal(published, 1u);
    check_equal(probe.calls, 1u);
    check_equal(probe.captured_before_callback, 1u);
    check_equal(probe.status, SALTS_OK);

    flowie_ingress_destroy(ingress);
    flowie_ingress_capture_cleanup(&capture);
  }

  it("preserves dispatch failure through the completion callback") {
    static const uint8_t packet[] = {0x10, 0x17, 0x00, 0x04, 'M',  'Q',  'T',  'T', 0x05,
                                     0x02, 0x00, 0x3c, 0x07, 0x15, 0x00, 0x04, 'n', 'o',
                                     'n',  'e',  0x00, 0x03, 'c',  'l',  'i'};
    flowie_ingress_capture_t capture = {0};
    flowie_ingress_completion_probe_t probe = {&capture, 0u, 0u, SALTS_OK};
    flowie_ingress_config_t config = FLOWIE_INGRESS_CONFIG_INIT;
    flowie_ingress_t *ingress;
    size_t published = 0u;

    capture.result = SALTS_EIO;
    config.dispatch = flowie_ingress_capture_dispatch;
    config.dispatch_ctx = &capture;
    config.max_packet_size = sizeof(packet);
    config.publish_complete = flowie_ingress_completion;
    config.prepare_ctx = &probe;
    ingress = flowie_ingress_create(&config);
    check_not_null(ingress);
    check_equal(flowie_ingress_feed(ingress, packet, sizeof(packet), &published), SALTS_EIO);
    check_equal(published, 0u);
    check_equal(probe.calls, 1u);
    check_equal(probe.captured_before_callback, 1u);
    check_equal(probe.status, SALTS_EIO);

    flowie_ingress_destroy(ingress);
    flowie_ingress_capture_cleanup(&capture);
  }

  it("fails fast on packet HWM and protocol errors without hidden replay") {
    static const uint8_t oversized[] = {0x30, 0x09};
    static const uint8_t pings[] = {0xc0, 0x00, 0xc0, 0x00};
    flowie_ingress_capture_t capture = {0};
    flowie_ingress_t *ingress = flowie_ingress_for(&capture, 8u);
    size_t published = 0u;

    check_not_null(ingress);
    check_equal(flowie_ingress_feed(ingress, oversized, sizeof(oversized), &published),
                SALTS_EMSGSIZE);
    check_equal(published, 0u);
    check_equal(capture.calls, 0u);
    flowie_ingress_destroy(ingress);

    ingress = flowie_ingress_for(&capture, 8u);
    check_not_null(ingress);
    check_equal(flowie_ingress_feed(ingress, pings, sizeof(pings), &published), SALTS_EPROTO);
    check_equal(published, 0u);
    check_equal(capture.calls, 0u);
    check_equal(flowie_ingress_feed(ingress, pings, 2u, &published), SALTS_EPROTO);
    check_equal(capture.calls, 0u);

    flowie_ingress_destroy(ingress);
    flowie_ingress_capture_cleanup(&capture);
  }
}
