#include "flowie_stl_error_internal.h"

#include <cstl.h>
#include <cstl.h>
#include <cstl.h>
#include <cstl.h>

#include "flowie_session_internal.h"

#include "salts_error.h"
#include <ltv_parser.h>
#include "salts_str.h"
#include <cstl.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct flowie_session_subscription_owned_s {
  tstr filter;
  uint8_t qos;
  uint8_t no_local;
  uint8_t retain_as_published;
  uint8_t retain_handling;
  uint32_t subscription_identifier;
} flowie_session_subscription_owned_t;

typedef enum flowie_session_inflight_state_e {
  FLOWIE_SESSION_INFLIGHT_SETTLEMENT = 1,
  FLOWIE_SESSION_INFLIGHT_QOS2_RELEASE
} flowie_session_inflight_state_t;

typedef struct flowie_session_inflight_s {
  uint16_t packet_id;
  uint8_t qos;
  flowie_session_inflight_state_t state;
} flowie_session_inflight_t;

typedef enum flowie_session_delivery_state_e {
  FLOWIE_SESSION_DELIVERY_RESERVED = 1,
  FLOWIE_SESSION_DELIVERY_WAIT_ACK,
  FLOWIE_SESSION_DELIVERY_WAIT_PUBREC,
  FLOWIE_SESSION_DELIVERY_WAIT_PUBCOMP,
  FLOWIE_SESSION_DELIVERY_QUEUED
} flowie_session_delivery_state_t;

typedef struct flowie_session_delivery_s {
  uint16_t packet_id;
  uint8_t qos;
  flowie_session_delivery_state_t state;
  uint64_t expiry_at_epoch_seconds;
  tstr packet;
} flowie_session_delivery_t;

struct flowie_session_owner_s {
  flowie_session_config_t config;
  tstr client_id;
  vec_t subscriptions;
  vec_t inflight;
  vec_t deliveries;
  hash_map_t delivery_index;
  tstr will_topic;
  tstr will_properties;
  tstr will_payload;
  flowie_mqtt_version_t version;
  uint16_t keep_alive;
  uint32_t session_expiry_interval;
  uint64_t session_generation;
  uint64_t resource_generation;
  uint16_t next_delivery_packet_id;
  uint32_t will_delay_interval;
  uint8_t initialized;
  uint8_t active;
  uint8_t clean_start;
  uint8_t has_will;
  uint8_t will_pending;
  uint8_t will_qos;
  uint8_t will_retain;
};

static int flowie_session_record_delivery_validate(const flowie_session_owner_t *owner,
                                                   const flowie_session_delivery_t *delivery);
static int flowie_session_config_valid(const flowie_session_config_t *config);

/* Average O(1) time, O(D) derived space for D outbound deliveries. */
static int flowie_session_delivery_lookup(flowie_session_owner_t *owner, uint16_t packet_id,
                                          flowie_session_delivery_t **delivery_out,
                                          size_t *index_out) {
  const size_t *mapped_index;
  flowie_session_delivery_t *delivery;
  if (!owner || packet_id == 0u) return SALTS_EINVAL;
  mapped_index =
      (const size_t *)hash_map_get_const(&owner->delivery_index, &packet_id);
  if (!mapped_index) return SALTS_ENOENT;
  delivery = (flowie_session_delivery_t *)vec_at(&owner->deliveries, *mapped_index);
  if (!delivery || delivery->packet_id != packet_id) return SALTS_EPROTO;
  if (delivery_out) *delivery_out = delivery;
  if (index_out) *index_out = *mapped_index;
  return SALTS_OK;
}

/* Amortized O(1) time. The vector remains unchanged if index insertion fails. */
static int flowie_session_delivery_append(flowie_session_owner_t *owner,
                                          const flowie_session_delivery_t *delivery) {
  size_t index;
  int rc;
  if (!owner || !delivery || delivery->packet_id == 0u) return SALTS_EINVAL;
  rc = flowie_session_delivery_lookup(owner, delivery->packet_id, NULL, NULL);
  if (rc == SALTS_OK) return SALTS_EALREADY;
  if (rc != SALTS_ENOENT) return rc;
  index = vec_size(&owner->deliveries);
  rc = flowie_stl_error(vec_push(&owner->deliveries, delivery));
  if (rc != SALTS_OK) return rc;
  rc = flowie_stl_error(
      hash_map_put(&owner->delivery_index, &delivery->packet_id, &index));
  if (rc != SALTS_OK) {
    if (flowie_stl_error(vec_pop(&owner->deliveries, NULL)) != SALTS_OK)
      return SALTS_EPROTO;
    return rc;
  }
  return SALTS_OK;
}

/* Average O(1) time; updates the slot of any vector element moved by swap-remove. */
static int flowie_session_delivery_remove_at(flowie_session_owner_t *owner, size_t index,
                                             flowie_session_delivery_t *removed_out) {
  flowie_session_delivery_t *delivery;
  const flowie_session_delivery_t *last;
  size_t *mapped_index;
  uint16_t packet_id;
  uint16_t moved_packet_id = 0u;
  size_t count;
  int rc;
  if (!owner) return SALTS_EINVAL;
  count = vec_size(&owner->deliveries);
  delivery = (flowie_session_delivery_t *)vec_at(&owner->deliveries, index);
  if (!delivery) return SALTS_ENOENT;
  packet_id = delivery->packet_id;
  mapped_index = (size_t *)hash_map_get(&owner->delivery_index, &packet_id);
  if (!mapped_index || *mapped_index != index) return SALTS_EPROTO;
  if (index + 1u < count) {
    last = (const flowie_session_delivery_t *)vec_at_const(&owner->deliveries, count - 1u);
    if (!last) return SALTS_EPROTO;
    moved_packet_id = last->packet_id;
    mapped_index = (size_t *)hash_map_get(&owner->delivery_index, &moved_packet_id);
    if (!mapped_index || *mapped_index != count - 1u) return SALTS_EPROTO;
  }
  rc = flowie_stl_error(hash_map_remove(&owner->delivery_index, &packet_id, NULL));
  if (rc != SALTS_OK) return SALTS_EPROTO;
  rc = flowie_stl_error(vec_swap_remove(&owner->deliveries, index, removed_out));
  if (rc != SALTS_OK) return rc;
  if (moved_packet_id != 0u) {
    mapped_index = (size_t *)hash_map_get(&owner->delivery_index, &moved_packet_id);
    if (!mapped_index) return SALTS_EPROTO;
    *mapped_index = index;
  }
  return SALTS_OK;
}

void flowie_session_owner_repository_snapshot_cleanup(flowie_protocol_session_row_t *row) {
  if (!row) return;
  free((void *)row->subscriptions);
  free((void *)row->inflight);
  free((void *)row->deliveries);
  row->subscriptions = NULL;
  row->inflight = NULL;
  row->deliveries = NULL;
  row->subscription_count = 0u;
  row->inflight_count = 0u;
  row->delivery_count = 0u;
}

int flowie_session_owner_repository_snapshot(const flowie_session_owner_t *owner,
                                             flowie_protocol_session_row_t *out) {
  flowie_protocol_session_row_t row = FLOWIE_PROTOCOL_SESSION_ROW_INIT;
  size_t persisted_inflight = 0u;
  size_t persisted_deliveries = 0u;
  if (!owner || !out || !owner->initialized || !owner->client_id ||
      owner->resource_generation == 0u)
    return SALTS_EINVAL;
  row.client_id = (flowie_mqtt_span_t){(const uint8_t *)owner->client_id,
                                      tstr_len(owner->client_id)};
  row.revision = owner->resource_generation;
  row.session_id = owner->config.session_id;
  row.session_generation = owner->session_generation;
  row.mqtt_version = owner->version;
  row.keep_alive = owner->keep_alive;
  row.session_expiry_interval = owner->session_expiry_interval;
  row.next_delivery_packet_id = owner->next_delivery_packet_id;
  row.subscription_count = vec_size(&owner->subscriptions);
  for (size_t i = 0u; i < vec_size(&owner->inflight); ++i) {
    const flowie_session_inflight_t *entry =
        (const flowie_session_inflight_t *)vec_at_const(&owner->inflight, i);
    if (entry && entry->state == FLOWIE_SESSION_INFLIGHT_QOS2_RELEASE) ++persisted_inflight;
  }
  for (size_t i = 0u; i < vec_size(&owner->deliveries); ++i) {
    const flowie_session_delivery_t *entry =
        (const flowie_session_delivery_t *)vec_at_const(&owner->deliveries, i);
    if (entry && entry->state != FLOWIE_SESSION_DELIVERY_RESERVED) ++persisted_deliveries;
  }
  row.inflight_count = persisted_inflight;
  row.delivery_count = persisted_deliveries;
  row.subscriptions = (const flowie_protocol_subscription_row_t *)calloc(
      row.subscription_count, sizeof(*row.subscriptions));
  row.inflight = (const flowie_protocol_inflight_row_t *)calloc(
      row.inflight_count, sizeof(*row.inflight));
  row.deliveries = (const flowie_protocol_delivery_row_t *)calloc(
      row.delivery_count, sizeof(*row.deliveries));
  if ((row.subscription_count && !row.subscriptions) ||
      (row.inflight_count && !row.inflight) || (row.delivery_count && !row.deliveries)) {
    flowie_session_owner_repository_snapshot_cleanup(&row);
    return SALTS_ENOMEM;
  }
  for (size_t i = 0u; i < row.subscription_count; ++i) {
    const flowie_session_subscription_owned_t *source =
        (const flowie_session_subscription_owned_t *)vec_at_const(&owner->subscriptions, i);
    flowie_protocol_subscription_row_t *target =
        &((flowie_protocol_subscription_row_t *)row.subscriptions)[i];
    if (!source || !source->filter) goto invalid;
    target->filter = (flowie_mqtt_span_t){(const uint8_t *)source->filter,
                                         tstr_len(source->filter)};
    target->qos = source->qos;
    target->no_local = source->no_local;
    target->retain_as_published = source->retain_as_published;
    target->retain_handling = source->retain_handling;
    target->subscription_identifier = source->subscription_identifier;
  }
  for (size_t i = 0u, target_index = 0u; i < vec_size(&owner->inflight); ++i) {
    const flowie_session_inflight_t *source =
        (const flowie_session_inflight_t *)vec_at_const(&owner->inflight, i);
    if (!source || source->state != FLOWIE_SESSION_INFLIGHT_QOS2_RELEASE) continue;
    ((flowie_protocol_inflight_row_t *)row.inflight)[target_index++] =
        (flowie_protocol_inflight_row_t){source->packet_id, source->qos};
  }
  for (size_t i = 0u, target_index = 0u; i < vec_size(&owner->deliveries); ++i) {
    const flowie_session_delivery_t *source =
        (const flowie_session_delivery_t *)vec_at_const(&owner->deliveries, i);
    flowie_protocol_delivery_row_t *target;
    if (!source || source->state == FLOWIE_SESSION_DELIVERY_RESERVED) continue;
    if (!source->packet) goto invalid;
    target = &((flowie_protocol_delivery_row_t *)row.deliveries)[target_index++];
    target->packet_id = source->packet_id;
    target->qos = source->qos;
    target->state = (uint8_t)(source->state == FLOWIE_SESSION_DELIVERY_QUEUED
                                  ? source->qos == 1u ? FLOWIE_SESSION_DELIVERY_WAIT_ACK
                                                      : FLOWIE_SESSION_DELIVERY_WAIT_PUBREC
                                  : source->state);
    target->expiry_at_epoch_seconds = source->expiry_at_epoch_seconds;
    target->packet = (flowie_mqtt_span_t){(const uint8_t *)source->packet,
                                         tstr_len(source->packet)};
  }
  if (owner->has_will) {
    row.will.present = 1;
    row.will.pending = owner->will_pending || owner->active;
    row.will.qos = owner->will_qos;
    row.will.retain = owner->will_retain;
    row.will.delay_interval = owner->will_delay_interval;
    row.will.topic = (flowie_mqtt_span_t){(const uint8_t *)owner->will_topic,
                                         tstr_len(owner->will_topic)};
    row.will.properties = (flowie_mqtt_span_t){(const uint8_t *)owner->will_properties,
                                              tstr_len(owner->will_properties)};
    row.will.payload = (flowie_mqtt_span_t){(const uint8_t *)owner->will_payload,
                                           tstr_len(owner->will_payload)};
  }
  *out = row;
  return SALTS_OK;
invalid:
  flowie_session_owner_repository_snapshot_cleanup(&row);
  return SALTS_EPROTO;
}

int flowie_session_owner_repository_restore(const flowie_session_config_t *config,
                                            const flowie_protocol_session_row_t *row,
                                            flowie_session_owner_t **out) {
  flowie_session_owner_t *owner = NULL;
  int rc = SALTS_EPROTO;
  if (out) *out = NULL;
  if (!flowie_session_config_valid(config) || !row || row->size < sizeof(*row) || !out ||
      !row->client_id.data || row->client_id.size == 0u || row->revision == 0u ||
      row->subscription_count > config->max_subscriptions ||
      row->inflight_count + row->delivery_count > config->max_inflight)
    return SALTS_EINVAL;
  owner = flowie_session_owner_create(config);
  if (!owner) return SALTS_ENOMEM;
  owner->client_id = tstr_new_len(row->client_id.data, row->client_id.size);
  if (!owner->client_id) { rc = SALTS_ENOMEM; goto fail; }
  owner->config.session_id = row->session_id;
  owner->version = row->mqtt_version;
  owner->keep_alive = row->keep_alive;
  owner->session_expiry_interval = row->session_expiry_interval;
  owner->session_generation = row->session_generation;
  owner->resource_generation = row->revision;
  owner->next_delivery_packet_id = row->next_delivery_packet_id;
  for (size_t i = 0u; i < row->subscription_count; ++i) {
    flowie_session_subscription_owned_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.filter = tstr_new_len(row->subscriptions[i].filter.data,
                               row->subscriptions[i].filter.size);
    if (!entry.filter) { rc = SALTS_ENOMEM; goto fail; }
    entry.qos = row->subscriptions[i].qos;
    entry.no_local = row->subscriptions[i].no_local;
    entry.retain_as_published = row->subscriptions[i].retain_as_published;
    entry.retain_handling = row->subscriptions[i].retain_handling;
    entry.subscription_identifier = row->subscriptions[i].subscription_identifier;
    rc = flowie_stl_error(vec_push(&owner->subscriptions, &entry));
    if (rc != SALTS_OK) { tstr_freep(&entry.filter); goto fail; }
  }
  for (size_t i = 0u; i < row->inflight_count; ++i) {
    flowie_session_inflight_t entry = {row->inflight[i].packet_id, row->inflight[i].qos,
                                       FLOWIE_SESSION_INFLIGHT_QOS2_RELEASE};
    rc = flowie_stl_error(vec_push(&owner->inflight, &entry));
    if (rc != SALTS_OK) goto fail;
  }
  for (size_t i = 0u; i < row->delivery_count; ++i) {
    flowie_session_delivery_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.packet_id = row->deliveries[i].packet_id;
    entry.qos = row->deliveries[i].qos;
    entry.state = (flowie_session_delivery_state_t)row->deliveries[i].state;
    entry.expiry_at_epoch_seconds = row->deliveries[i].expiry_at_epoch_seconds;
    entry.packet = tstr_new_len(row->deliveries[i].packet.data, row->deliveries[i].packet.size);
    if (!entry.packet) { rc = SALTS_ENOMEM; goto fail; }
    rc = flowie_session_record_delivery_validate(owner, &entry);
    if (rc == SALTS_OK) rc = flowie_session_delivery_append(owner, &entry);
    if (rc == SALTS_EALREADY) rc = SALTS_EPROTO;
    if (rc != SALTS_OK) { tstr_freep(&entry.packet); goto fail; }
  }
  if (row->will.present) {
    owner->will_topic = tstr_new_len(row->will.topic.data, row->will.topic.size);
    owner->will_properties = tstr_new_len(row->will.properties.data, row->will.properties.size);
    owner->will_payload = tstr_new_len(row->will.payload.data, row->will.payload.size);
    if (!owner->will_topic || !owner->will_properties || !owner->will_payload) {
      rc = SALTS_ENOMEM; goto fail;
    }
    owner->has_will = 1u;
    owner->will_pending = (uint8_t)row->will.pending;
    owner->will_qos = row->will.qos;
    owner->will_retain = row->will.retain;
    owner->will_delay_interval = row->will.delay_interval;
  }
  owner->active = 0u;
  owner->clean_start = 0u;
  *out = owner;
  return SALTS_OK;
fail:
  flowie_session_owner_destroy(owner);
  return rc;
}

static int flowie_session_config_valid(const flowie_session_config_t *config) {
  return config && config->size >= sizeof(*config) &&
         config->abi_version == FLOWIE_SESSION_INTERNAL_ABI_V1 && config->owner_instance_id != 0u &&
         config->session_id != 0u && config->max_subscriptions != 0u &&
         config->max_subscriptions <= FLOWIE_SESSION_INTERNAL_MAX_SUBSCRIPTIONS &&
         config->max_inflight != 0u && config->max_inflight <= UINT16_MAX &&
         flowie_protocol_settlement_policy_validate(&config->settlement) == SALTS_OK;
}

static void flowie_session_subscriptions_clear(vec_t *subscriptions) {
  if (!subscriptions) return;
  for (size_t i = 0u; i < vec_size(subscriptions); ++i) {
    flowie_session_subscription_owned_t *entry =
        (flowie_session_subscription_owned_t *)vec_at(subscriptions, i);
    if (entry) tstr_freep(&entry->filter);
  }
  vec_clear(subscriptions);
}

static void flowie_session_subscriptions_destroy(vec_t *subscriptions) {
  if (!subscriptions) return;
  flowie_session_subscriptions_clear(subscriptions);
  vec_destroy(subscriptions);
}

static void flowie_session_inflight_clear(flowie_session_owner_t *owner, int keep_qos2_release) {
  size_t index = 0u;
  while (index < vec_size(&owner->inflight)) {
    const flowie_session_inflight_t *entry =
        (const flowie_session_inflight_t *)vec_at_const(&owner->inflight, index);
    if (keep_qos2_release && entry && entry->state == FLOWIE_SESSION_INFLIGHT_QOS2_RELEASE) {
      ++index;
    } else {
      (void)flowie_stl_error(vec_swap_remove(&owner->inflight, index, NULL));
    }
  }
}

static void flowie_session_deliveries_clear(flowie_session_owner_t *owner) {
  if (!owner) return;
  for (size_t i = 0u; i < vec_size(&owner->deliveries); ++i) {
    flowie_session_delivery_t *delivery =
        (flowie_session_delivery_t *)vec_at(&owner->deliveries, i);
    if (delivery) tstr_freep(&delivery->packet);
  }
  vec_clear(&owner->deliveries);
  hash_map_clear(&owner->delivery_index);
}

static void flowie_session_will_clear(flowie_session_owner_t *owner) {
  if (!owner) return;
  tstr_freep(&owner->will_topic);
  tstr_freep(&owner->will_properties);
  tstr_freep(&owner->will_payload);
  owner->will_delay_interval = 0u;
  owner->has_will = 0u;
  owner->will_pending = 0u;
  owner->will_qos = 0u;
  owner->will_retain = 0u;
}

static int flowie_session_will_delay(const flowie_mqtt_connect_view_t *connect, uint32_t *out) {
  flowie_mqtt_property_iterator_t iterator = FLOWIE_MQTT_PROPERTY_ITERATOR_INIT;
  flowie_mqtt_property_view_t property = FLOWIE_MQTT_PROPERTY_VIEW_INIT;
  int found = 0;
  int rc;
  if (!connect || !out) return SALTS_EINVAL;
  *out = 0u;
  if (connect->version != FLOWIE_MQTT_VERSION_5 || connect->will_properties.values.size == 0u)
    return SALTS_OK;
  rc = flowie_mqtt_property_iterator_init(&connect->will_properties, &iterator);
  if (rc != FLOWIE_MQTT_PARSE_OK) return SALTS_EPROTO;
  while ((rc = flowie_mqtt_property_iterator_next(&iterator, &property)) == FLOWIE_MQTT_PARSE_OK) {
    if (property.identifier != FLOWIE_MQTT_PROPERTY_WILL_DELAY_INTERVAL) continue;
    if (found) return SALTS_EPROTO;
    *out = property.integer;
    found = 1;
  }
  return rc == FLOWIE_MQTT_PARSE_NEED_MORE ? SALTS_OK : SALTS_EPROTO;
}

static int flowie_session_subscriptions_clone(const vec_t *source, vec_t *out) {
  int rc = flowie_stl_error(vec_init_bytes(out, sizeof(flowie_session_subscription_owned_t), _Alignof(flowie_session_subscription_owned_t), SIZE_MAX));
  if (rc != SALTS_OK) return rc;
  for (size_t i = 0u; i < vec_size(source); ++i) {
    const flowie_session_subscription_owned_t *entry =
        (const flowie_session_subscription_owned_t *)vec_at_const(source, i);
    flowie_session_subscription_owned_t copy;
    if (!entry || !entry->filter) {
      rc = SALTS_EPROTO;
      goto fail;
    }
    copy = *entry;
    copy.filter = tstr_dup(entry->filter);
    if (!copy.filter) {
      rc = SALTS_ENOMEM;
      goto fail;
    }
    rc = flowie_stl_error(vec_push(out, &copy));
    if (rc != SALTS_OK) {
      tstr_free(copy.filter);
      goto fail;
    }
  }
  return SALTS_OK;

fail:
  flowie_session_subscriptions_destroy(out);
  return rc;
}

static int flowie_session_generation_advance(flowie_session_owner_t *owner) {
  if (owner->resource_generation == UINT64_MAX) return SALTS_ERANGE;
  owner->resource_generation += 1u;
  return SALTS_OK;
}

static int flowie_session_expiry(const flowie_mqtt_connect_view_t *connect, uint32_t *out) {
  flowie_mqtt_property_iterator_t iterator = FLOWIE_MQTT_PROPERTY_ITERATOR_INIT;
  flowie_mqtt_property_view_t property = FLOWIE_MQTT_PROPERTY_VIEW_INIT;
  int found = 0;
  int rc;
  if (flowie_mqtt_version_is_3x(connect->version)) {
    *out = connect->clean_start ? 0u : UINT32_MAX;
    return SALTS_OK;
  }
  *out = 0u;
  if (connect->properties.values.size == 0u) return SALTS_OK;
  rc = flowie_mqtt_property_iterator_init(&connect->properties, &iterator);
  if (rc != FLOWIE_MQTT_PARSE_OK) return SALTS_EPROTO;
  while ((rc = flowie_mqtt_property_iterator_next(&iterator, &property)) == FLOWIE_MQTT_PARSE_OK) {
    if (property.identifier == FLOWIE_MQTT_PROPERTY_SESSION_EXPIRY_INTERVAL) {
      if (found) return SALTS_EPROTO;
      *out = property.integer;
      found = 1;
    }
  }
  return rc == FLOWIE_MQTT_PARSE_NEED_MORE ? SALTS_OK : SALTS_EPROTO;
}

flowie_session_owner_t *flowie_session_owner_create(const flowie_session_config_t *config) {
  flowie_session_owner_t *owner;
  if (!flowie_session_config_valid(config)) return NULL;
  owner = (flowie_session_owner_t *)calloc(1u, sizeof(*owner));
  if (!owner) return NULL;
  owner->config = *config;
  owner->config.size = sizeof(owner->config);
  owner->resource_generation = 1u;
  if (flowie_stl_error(vec_init_bytes(&owner->subscriptions, sizeof(flowie_session_subscription_owned_t), _Alignof(flowie_session_subscription_owned_t), SIZE_MAX)) !=
      SALTS_OK) {
    free(owner);
    return NULL;
  }
  if (flowie_stl_error(vec_init_bytes(&owner->inflight, sizeof(flowie_session_inflight_t), _Alignof(flowie_session_inflight_t), SIZE_MAX)) != SALTS_OK) {
    vec_destroy(&owner->subscriptions);
    free(owner);
    return NULL;
  }
  if (flowie_stl_error(vec_init_bytes(&owner->deliveries, sizeof(flowie_session_delivery_t), _Alignof(flowie_session_delivery_t), SIZE_MAX)) != SALTS_OK) {
    vec_destroy(&owner->inflight);
    vec_destroy(&owner->subscriptions);
    free(owner);
    return NULL;
  }
  if (flowie_stl_error(hash_map_init_bytes(
          &owner->delivery_index, sizeof(uint16_t), _Alignof(uint16_t),
          sizeof(size_t), _Alignof(size_t), config->max_inflight, hash_bytes,
          hash_key_equal, NULL)) != SALTS_OK) {
    vec_destroy(&owner->deliveries);
    vec_destroy(&owner->inflight);
    vec_destroy(&owner->subscriptions);
    free(owner);
    return NULL;
  }
  return owner;
}

flowie_session_owner_t *flowie_session_owner_clone(const flowie_session_owner_t *owner) {
  flowie_session_owner_t *copy;
  int rc;
  if (!owner || !flowie_session_config_valid(&owner->config)) return NULL;
  copy = flowie_session_owner_create(&owner->config);
  if (!copy) return NULL;
  if (owner->client_id) {
    copy->client_id = tstr_dup(owner->client_id);
    if (!copy->client_id) goto fail;
  }
  if (owner->has_will) {
    copy->will_topic = tstr_clone(owner->will_topic);
    copy->will_properties = tstr_clone(owner->will_properties);
    copy->will_payload = tstr_clone(owner->will_payload);
    if (!copy->will_topic || !copy->will_properties || !copy->will_payload) goto fail;
  }
  flowie_session_subscriptions_destroy(&copy->subscriptions);
  rc = flowie_session_subscriptions_clone(&owner->subscriptions, &copy->subscriptions);
  if (rc != SALTS_OK) goto fail;
  for (size_t i = 0u; i < vec_size(&owner->inflight); ++i) {
    const flowie_session_inflight_t *entry =
        (const flowie_session_inflight_t *)vec_at_const(&owner->inflight, i);
    if (!entry || flowie_stl_error(vec_push(&copy->inflight, entry)) != SALTS_OK) goto fail;
  }
  for (size_t i = 0u; i < vec_size(&owner->deliveries); ++i) {
    const flowie_session_delivery_t *entry =
        (const flowie_session_delivery_t *)vec_at_const(&owner->deliveries, i);
    flowie_session_delivery_t cloned;
    if (!entry) goto fail;
    cloned = *entry;
    cloned.packet = entry->packet ? tstr_clone(entry->packet) : NULL;
    if (entry->packet && !cloned.packet) goto fail;
    rc = flowie_session_delivery_append(copy, &cloned);
    if (rc != SALTS_OK) {
      tstr_freep(&cloned.packet);
      goto fail;
    }
  }
  copy->version = owner->version;
  copy->keep_alive = owner->keep_alive;
  copy->session_expiry_interval = owner->session_expiry_interval;
  copy->session_generation = owner->session_generation;
  copy->resource_generation = owner->resource_generation;
  copy->next_delivery_packet_id = owner->next_delivery_packet_id;
  copy->will_delay_interval = owner->will_delay_interval;
  copy->initialized = owner->initialized;
  copy->active = owner->active;
  copy->clean_start = owner->clean_start;
  copy->has_will = owner->has_will;
  copy->will_pending = owner->will_pending;
  copy->will_qos = owner->will_qos;
  copy->will_retain = owner->will_retain;
  return copy;

fail:
  flowie_session_owner_destroy(copy);
  return NULL;
}

int flowie_session_owner_touch(flowie_session_owner_t *owner) {
  if (!owner || !owner->initialized) return SALTS_EINVAL;
  return flowie_session_generation_advance(owner);
}

void flowie_session_owner_destroy(flowie_session_owner_t *owner) {
  if (!owner) return;
  flowie_session_subscriptions_destroy(&owner->subscriptions);
  flowie_session_deliveries_clear(owner);
  hash_map_destroy(&owner->delivery_index);
  vec_destroy(&owner->deliveries);
  vec_destroy(&owner->inflight);
  flowie_session_will_clear(owner);
  tstr_freep(&owner->client_id);
  free(owner);
}

static int flowie_session_owner_open_impl(flowie_session_owner_t *owner,
                                           const flowie_mqtt_connect_view_t *connect,
                                           int allow_active) {
  tstr client_id = NULL;
  tstr will_topic = NULL;
  tstr will_properties = NULL;
  tstr will_payload = NULL;
  uint32_t expiry;
  uint32_t will_delay = 0u;
  int rc;
  if (!owner || !connect || connect->size < sizeof(*connect) ||
      connect->abi_version != FLOWIE_MQTT_PROTOCOL_ABI_V1 ||
      connect->properties.size < sizeof(connect->properties) ||
      connect->properties.abi_version != FLOWIE_MQTT_PROTOCOL_ABI_V1 ||
      !flowie_mqtt_version_is_supported(connect->version) ||
      (!connect->client_id.data && connect->client_id.size != 0u))
    return SALTS_EINVAL;
  if (owner->active && !allow_active) return SALTS_EALREADY;
  if (connect->client_id.size == 0u) return SALTS_ENOTSUP;
  if (connect->client_id.size > UINT16_MAX) return SALTS_EMSGSIZE;
  if (!flowie_mqtt_utf8_validate(connect->client_id)) return SALTS_EPROTO;
  if (owner->initialized &&
      (tstr_len(owner->client_id) != connect->client_id.size ||
       memcmp(owner->client_id, connect->client_id.data, connect->client_id.size) != 0))
    return SALTS_EPROTO;
  if (owner->initialized && owner->version != connect->version && !connect->clean_start)
    return SALTS_EPROTO;
  if (owner->session_generation == UINT64_MAX || owner->resource_generation == UINT64_MAX)
    return SALTS_ERANGE;
  rc = flowie_session_expiry(connect, &expiry);
  if (rc != SALTS_OK) return rc;
  if (connect->will_topic.size != 0u) {
    rc = flowie_session_will_delay(connect, &will_delay);
    if (rc != SALTS_OK) return rc;
    will_topic = tstr_new_len(connect->will_topic.data, connect->will_topic.size);
    will_properties = tstr_new_len(connect->will_properties.values.data,
                                   connect->will_properties.values.size);
    will_payload = tstr_new_len(connect->will_payload.data, connect->will_payload.size);
    if (!will_topic || !will_properties || !will_payload) {
      tstr_freep(&will_topic);
      tstr_freep(&will_properties);
      tstr_freep(&will_payload);
      return SALTS_ENOMEM;
    }
  }
  if (!owner->initialized) {
    client_id = tstr_new_len(connect->client_id.data, connect->client_id.size);
    if (!client_id) {
      tstr_freep(&will_topic);
      tstr_freep(&will_properties);
      tstr_freep(&will_payload);
      return SALTS_ENOMEM;
    }
  }
  if (connect->clean_start) {
    flowie_session_subscriptions_clear(&owner->subscriptions);
    flowie_session_inflight_clear(owner, 0);
    flowie_session_deliveries_clear(owner);
  }
  if (client_id) owner->client_id = client_id;
  flowie_session_will_clear(owner);
  if (will_topic) {
    owner->will_topic = will_topic;
    owner->will_properties = will_properties;
    owner->will_payload = will_payload;
    owner->will_delay_interval = will_delay;
    owner->has_will = 1u;
    owner->will_qos = connect->will_qos;
    owner->will_retain = connect->will_retain;
  }
  owner->version = connect->version;
  owner->keep_alive = connect->keep_alive;
  owner->session_expiry_interval = expiry;
  owner->clean_start = connect->clean_start;
  owner->session_generation += 1u;
  owner->resource_generation += 1u;
  owner->initialized = 1u;
  owner->active = 1u;
  return SALTS_OK;
}

int flowie_session_owner_open(flowie_session_owner_t *owner,
                              const flowie_mqtt_connect_view_t *connect) {
  return flowie_session_owner_open_impl(owner, connect, 0);
}

static int flowie_session_owner_connect_impl(flowie_session_owner_t *owner,
                                              const flowie_mqtt_connect_view_t *connect,
                                              flowie_session_connect_result_t *out,
                                              int allow_active) {
  flowie_session_connect_result_t result = FLOWIE_SESSION_CONNECT_RESULT_INIT;
  int initialized;
  int rc;
  if (!owner || !connect || connect->size < sizeof(*connect) ||
      connect->abi_version != FLOWIE_MQTT_PROTOCOL_ABI_V1 || !out || out->size < sizeof(*out) ||
      out->abi_version != FLOWIE_SESSION_INTERNAL_ABI_V1)
    return SALTS_EINVAL;

  initialized = owner->initialized != 0u;
  result.reply.type = FLOWIE_MQTT_PACKET_CONNACK;
  result.reply.version = connect->version;
  rc = flowie_session_owner_open_impl(owner, connect, allow_active);
  if (rc == SALTS_OK) {
    result.accepted = 1u;
    result.session_present = (uint8_t)(initialized && !connect->clean_start);
    result.reply.session_present =
        connect->version == FLOWIE_MQTT_VERSION_3_1 ? 0u : result.session_present;
    rc = flowie_session_owner_route(owner, &result.route);
    if (rc != SALTS_OK) return rc;
    *out = result;
    return SALTS_OK;
  }

  result.close_after_reply = 1u;
  if (rc == SALTS_ENOTSUP) {
    result.reply.reason_code =
        connect->version == FLOWIE_MQTT_VERSION_5 ? UINT8_C(0x85) : UINT8_C(0x02);
  } else if (rc == SALTS_EALREADY) {
    result.reply.reason_code =
        connect->version == FLOWIE_MQTT_VERSION_5 ? UINT8_C(0x89) : UINT8_C(0x03);
  } else {
    return rc;
  }
  *out = result;
  return SALTS_OK;
}

int flowie_session_owner_connect(flowie_session_owner_t *owner,
                                 const flowie_mqtt_connect_view_t *connect,
                                 flowie_session_connect_result_t *out) {
  return flowie_session_owner_connect_impl(owner, connect, out, 0);
}

int flowie_session_owner_connect_takeover(flowie_session_owner_t *owner,
                                          const flowie_mqtt_connect_view_t *connect,
                                          flowie_session_connect_result_t *out) {
  return flowie_session_owner_connect_impl(owner, connect, out, 1);
}

int flowie_session_owner_close(flowie_session_owner_t *owner) {
  int rc;
  if (!owner) return SALTS_EINVAL;
  if (!owner->active) return SALTS_EALREADY;
  rc = flowie_session_generation_advance(owner);
  if (rc != SALTS_OK) return rc;
  owner->active = 0u;
  if (owner->has_will) owner->will_pending = 1u;
  if (owner->session_expiry_interval == 0u) {
    flowie_session_subscriptions_clear(&owner->subscriptions);
    flowie_session_inflight_clear(owner, 0);
    flowie_session_deliveries_clear(owner);
  } else {
    flowie_session_inflight_clear(owner, 1);
  }
  return SALTS_OK;
}

int flowie_session_owner_snapshot(const flowie_session_owner_t *owner,
                                  flowie_session_snapshot_t *out) {
  flowie_session_snapshot_t snapshot = FLOWIE_SESSION_SNAPSHOT_INIT;
  if (!owner || !out || out->size < sizeof(*out) ||
      out->abi_version != FLOWIE_SESSION_INTERNAL_ABI_V1)
    return SALTS_EINVAL;
  snapshot.active = owner->active;
  snapshot.clean_start = owner->clean_start;
  snapshot.version = owner->version;
  snapshot.keep_alive = owner->keep_alive;
  snapshot.session_expiry_interval = owner->session_expiry_interval;
  snapshot.owner_instance_id = owner->config.owner_instance_id;
  snapshot.session_id = owner->config.session_id;
  snapshot.session_generation = owner->session_generation;
  snapshot.resource_generation = owner->resource_generation;
  snapshot.subscription_count = vec_size(&owner->subscriptions);
  snapshot.subscription_capacity = owner->config.max_subscriptions;
  snapshot.inflight_count = vec_size(&owner->inflight) + vec_size(&owner->deliveries);
  snapshot.inflight_capacity = owner->config.max_inflight;
  snapshot.client_id.data = (const uint8_t *)owner->client_id;
  snapshot.client_id.size = owner->client_id ? tstr_len(owner->client_id) : 0u;
  snapshot.has_will = owner->has_will;
  snapshot.will_pending = owner->will_pending;
  snapshot.will_qos = owner->will_qos;
  snapshot.will_retain = owner->will_retain;
  snapshot.will_delay_interval = owner->will_delay_interval;
  snapshot.will_topic = (flowie_mqtt_span_t){(const uint8_t *)owner->will_topic,
                                              owner->will_topic ? tstr_len(owner->will_topic) : 0u};
  snapshot.will_properties =
      (flowie_mqtt_span_t){(const uint8_t *)owner->will_properties,
                           owner->will_properties ? tstr_len(owner->will_properties) : 0u};
  snapshot.will_payload =
      (flowie_mqtt_span_t){(const uint8_t *)owner->will_payload,
                           owner->will_payload ? tstr_len(owner->will_payload) : 0u};
  *out = snapshot;
  return SALTS_OK;
}

int flowie_session_owner_route(const flowie_session_owner_t *owner,
                               flowie_protocol_route_t *out) {
  flowie_protocol_route_t route = FLOWIE_PROTOCOL_ROUTE_INIT;
  if (!owner || !out || out->size < sizeof(*out) ||
      out->contract_version != FLOWIE_PROTOCOL_CONTRACT_VERSION)
    return SALTS_EINVAL;
  if (!owner->active || owner->session_generation == 0u) return SALTS_EBUSY;
  route.protocol = FLOWIE_PROTOCOL_MQTT;
  route.owner_instance_id = owner->config.owner_instance_id;
  route.session_id = owner->config.session_id;
  route.session_generation = owner->session_generation;
  *out = route;
  return SALTS_OK;
}

int flowie_session_owner_disconnect(flowie_session_owner_t *owner,
                                    const flowie_mqtt_control_packet_view_t *disconnect) {
  flowie_mqtt_property_iterator_t iterator = FLOWIE_MQTT_PROPERTY_ITERATOR_INIT;
  flowie_mqtt_property_view_t property = FLOWIE_MQTT_PROPERTY_VIEW_INIT;
  uint32_t expiry = 0u;
  int found = 0;
  int changed = 0;
  int rc;
  if (!owner || !disconnect || disconnect->size < sizeof(*disconnect) ||
      disconnect->abi_version != FLOWIE_MQTT_PROTOCOL_ABI_V1 ||
      disconnect->type != FLOWIE_MQTT_PACKET_DISCONNECT || disconnect->version != owner->version)
    return SALTS_EINVAL;
  if (!owner->active) return SALTS_EBUSY;
  if (disconnect->version == FLOWIE_MQTT_VERSION_5) {
    rc = flowie_mqtt_property_iterator_init(&disconnect->properties, &iterator);
    if (rc != FLOWIE_MQTT_PARSE_OK) return SALTS_EPROTO;
    while ((rc = flowie_mqtt_property_iterator_next(&iterator, &property)) ==
           FLOWIE_MQTT_PARSE_OK) {
      if (property.identifier != FLOWIE_MQTT_PROPERTY_SESSION_EXPIRY_INTERVAL) continue;
      if (found) return SALTS_EPROTO;
      expiry = property.integer;
      found = 1;
    }
    if (rc != FLOWIE_MQTT_PARSE_NEED_MORE) return SALTS_EPROTO;
    if (found && expiry != owner->session_expiry_interval) {
      if (owner->session_expiry_interval == 0u && expiry != 0u) return SALTS_EPROTO;
      changed = 1;
    }
  }
  if (disconnect->reason_code != 0x04u && owner->has_will) changed = 1;
  if (!changed) return SALTS_OK;
  if (owner->resource_generation == UINT64_MAX) return SALTS_ERANGE;
  if (found) owner->session_expiry_interval = expiry;
  if (disconnect->reason_code != 0x04u && owner->has_will) flowie_session_will_clear(owner);
  owner->resource_generation += 1u;
  return SALTS_OK;
}

int flowie_session_owner_will_complete(flowie_session_owner_t *owner) {
  if (!owner) return SALTS_EINVAL;
  if (!owner->has_will || !owner->will_pending) return SALTS_ENOENT;
  if (owner->resource_generation == UINT64_MAX) return SALTS_ERANGE;
  flowie_session_will_clear(owner);
  owner->resource_generation += 1u;
  return SALTS_OK;
}

static flowie_session_subscription_owned_t *
flowie_session_subscription_find(vec_t *subscriptions, flowie_mqtt_span_t filter) {
  for (size_t i = 0u; i < vec_size(subscriptions); ++i) {
    flowie_session_subscription_owned_t *entry =
        (flowie_session_subscription_owned_t *)vec_at(subscriptions, i);
    if (entry && entry->filter && tstr_len(entry->filter) == filter.size &&
        memcmp(entry->filter, filter.data, filter.size) == 0)
      return entry;
  }
  return NULL;
}

static int flowie_session_subscription_apply(vec_t *subscriptions, size_t capacity,
                                             const flowie_mqtt_subscription_view_t *entry,
                                             uint32_t subscription_identifier,
                                             int *changed) {
  flowie_session_subscription_owned_t *existing =
      flowie_session_subscription_find(subscriptions, entry->filter);
  if (existing) {
    if (existing->qos != entry->qos || existing->no_local != entry->no_local ||
        existing->retain_as_published != entry->retain_as_published ||
        existing->retain_handling != entry->retain_handling ||
        existing->subscription_identifier != subscription_identifier) {
      existing->qos = entry->qos;
      existing->no_local = entry->no_local;
      existing->retain_as_published = entry->retain_as_published;
      existing->retain_handling = entry->retain_handling;
      existing->subscription_identifier = subscription_identifier;
      *changed = 1;
    }
    return SALTS_OK;
  }
  if (vec_size(subscriptions) >= capacity) return SALTS_ENOSPC;
  {
    flowie_session_subscription_owned_t added;
    int rc;
    memset(&added, 0, sizeof(added));
    added.filter = tstr_new_len(entry->filter.data, entry->filter.size);
    if (!added.filter) return SALTS_ENOMEM;
    added.qos = entry->qos;
    added.no_local = entry->no_local;
    added.retain_as_published = entry->retain_as_published;
    added.retain_handling = entry->retain_handling;
    added.subscription_identifier = subscription_identifier;
    rc = flowie_stl_error(vec_push(subscriptions, &added));
    if (rc != SALTS_OK) {
      tstr_free(added.filter);
      return rc;
    }
  }
  *changed = 1;
  return SALTS_OK;
}

int flowie_session_owner_subscribe(flowie_session_owner_t *owner,
                                   const flowie_mqtt_packet_view_t *packet,
                                   const flowie_mqtt_subscribe_view_t *subscribe,
                                   flowie_session_subscribe_result_t *out) {
  flowie_mqtt_subscription_iterator_t iterator = FLOWIE_MQTT_SUBSCRIPTION_ITERATOR_INIT;
  flowie_mqtt_subscription_view_t entry;
  flowie_session_subscribe_result_t result = FLOWIE_SESSION_SUBSCRIBE_RESULT_INIT;
  vec_t staged = {0};
  size_t count = 0u;
  uint32_t subscription_identifier = 0u;
  int changed = 0;
  int rc;
  if (!owner || !packet || packet->size < sizeof(*packet) || !subscribe ||
      subscribe->size < sizeof(*subscribe) || !out || out->size < sizeof(*out) ||
      out->abi_version != FLOWIE_SESSION_INTERNAL_ABI_V1)
    return SALTS_EINVAL;
  if (!owner->active) return SALTS_EBUSY;
  if (packet->abi_version != FLOWIE_MQTT_PROTOCOL_ABI_V1 ||
      subscribe->abi_version != FLOWIE_MQTT_PROTOCOL_ABI_V1 ||
      packet->type != FLOWIE_MQTT_PACKET_SUBSCRIBE || packet->version != owner->version ||
      subscribe->packet_id == 0u || subscribe->entry_count == 0u)
    return SALTS_EPROTO;
  rc = flowie_mqtt_subscription_iterator_init(packet, subscribe, &iterator);
  if (rc != FLOWIE_MQTT_PARSE_OK) return SALTS_EPROTO;
  if (packet->version == FLOWIE_MQTT_VERSION_5 && subscribe->properties.values.size != 0u) {
    flowie_mqtt_property_iterator_t property_iterator = FLOWIE_MQTT_PROPERTY_ITERATOR_INIT;
    flowie_mqtt_property_view_t property = FLOWIE_MQTT_PROPERTY_VIEW_INIT;
    rc = flowie_mqtt_property_iterator_init(&subscribe->properties, &property_iterator);
    if (rc != FLOWIE_MQTT_PARSE_OK) return SALTS_EPROTO;
    while ((rc = flowie_mqtt_property_iterator_next(&property_iterator, &property)) ==
           FLOWIE_MQTT_PARSE_OK) {
      if (property.identifier == FLOWIE_MQTT_PROPERTY_SUBSCRIPTION_IDENTIFIER)
        subscription_identifier = property.integer;
    }
    if (rc != FLOWIE_MQTT_PARSE_NEED_MORE) return SALTS_EPROTO;
  }
  rc = flowie_session_subscriptions_clone(&owner->subscriptions, &staged);
  if (rc != SALTS_OK) return rc;
  while ((rc = flowie_mqtt_subscription_iterator_next(&iterator, &entry)) == FLOWIE_MQTT_PARSE_OK) {
    rc = flowie_session_subscription_apply(&staged, owner->config.max_subscriptions, &entry,
                                           subscription_identifier, &changed);
    if (rc != SALTS_OK) goto fail;
    ++count;
  }
  if (rc != FLOWIE_MQTT_PARSE_NEED_MORE || count != subscribe->entry_count) {
    rc = SALTS_EPROTO;
    goto fail;
  }
  if (changed) {
    vec_t previous;
    rc = flowie_session_generation_advance(owner);
    if (rc != SALTS_OK) goto fail;
    previous = owner->subscriptions;
    owner->subscriptions = staged;
    staged = previous;
  }
  flowie_session_subscriptions_destroy(&staged);
  result.packet_id = subscribe->packet_id;
  result.accepted_count = count;
  result.changed = (uint8_t)changed;
  *out = result;
  return SALTS_OK;

fail:
  flowie_session_subscriptions_destroy(&staged);
  return rc;
}

int flowie_session_owner_subscription_at(const flowie_session_owner_t *owner, size_t index,
                                         flowie_session_subscription_t *out) {
  flowie_session_subscription_t value = FLOWIE_SESSION_SUBSCRIPTION_INIT;
  const flowie_session_subscription_owned_t *entry;
  if (!owner || !out || out->size < sizeof(*out) ||
      out->abi_version != FLOWIE_SESSION_INTERNAL_ABI_V1)
    return SALTS_EINVAL;
  entry =
      (const flowie_session_subscription_owned_t *)vec_at_const(&owner->subscriptions, index);
  if (!entry || !entry->filter) return SALTS_ENOENT;
  value.filter.data = (const uint8_t *)entry->filter;
  value.filter.size = tstr_len(entry->filter);
  value.qos = entry->qos;
  value.no_local = entry->no_local;
  value.retain_as_published = entry->retain_as_published;
  value.retain_handling = entry->retain_handling;
  value.subscription_identifier = entry->subscription_identifier;
  *out = value;
  return SALTS_OK;
}

int flowie_session_owner_delivery_reserve(flowie_session_owner_t *owner, uint8_t qos,
                                          uint16_t *packet_id) {
  flowie_session_delivery_t delivery;
  uint16_t candidate;
  int rc;
  if (!owner || !packet_id || qos == 0u || qos > 2u) return SALTS_EINVAL;
  if (!owner->active && (!owner->initialized || owner->session_expiry_interval == 0u))
    return SALTS_EBUSY;
  if (vec_size(&owner->inflight) + vec_size(&owner->deliveries) >=
      owner->config.max_inflight)
    return SALTS_ENOSPC;
  candidate = owner->next_delivery_packet_id;
  for (size_t attempt = 0u; attempt < UINT16_MAX; ++attempt) {
    candidate = candidate == UINT16_MAX ? 1u : (uint16_t)(candidate + 1u);
    rc = flowie_session_delivery_lookup(owner, candidate, NULL, NULL);
    if (rc == SALTS_ENOENT) break;
    if (rc != SALTS_OK) return rc;
  }
  if (rc != SALTS_ENOENT) return SALTS_ENOSPC;
  memset(&delivery, 0, sizeof(delivery));
  delivery.packet_id = candidate;
  delivery.qos = qos;
  delivery.state = FLOWIE_SESSION_DELIVERY_RESERVED;
  rc = flowie_session_delivery_append(owner, &delivery);
  if (rc != SALTS_OK) return rc;
  owner->next_delivery_packet_id = candidate;
  *packet_id = candidate;
  return SALTS_OK;
}

static int flowie_session_owner_delivery_commit_state(flowie_session_owner_t *owner,
                                                      uint16_t packet_id, flowie_mqtt_span_t packet,
                                                      uint64_t expiry_at_epoch_seconds,
                                                      int queued) {
  flowie_session_delivery_t *delivery;
  tstr owned;
  if (!owner || !packet.data || packet.size == 0u) return SALTS_EINVAL;
  {
    int rc = flowie_session_delivery_lookup(owner, packet_id, &delivery, NULL);
    if (rc != SALTS_OK) return rc;
  }
  if (delivery->state != FLOWIE_SESSION_DELIVERY_RESERVED || delivery->packet) return SALTS_EALREADY;
  owned = tstr_new_len(packet.data, packet.size);
  if (!owned) return SALTS_ENOMEM;
  delivery->packet = owned;
  delivery->expiry_at_epoch_seconds = expiry_at_epoch_seconds;
  delivery->state = queued ? FLOWIE_SESSION_DELIVERY_QUEUED
                           : delivery->qos == 1u ? FLOWIE_SESSION_DELIVERY_WAIT_ACK
                                                 : FLOWIE_SESSION_DELIVERY_WAIT_PUBREC;
  if (expiry_at_epoch_seconds != 0u &&
      flowie_session_record_delivery_validate(owner, delivery) != SALTS_OK) {
    tstr_freep(&delivery->packet);
    delivery->expiry_at_epoch_seconds = 0u;
    delivery->state = FLOWIE_SESSION_DELIVERY_RESERVED;
    return SALTS_EPROTO;
  }
  if (owner->resource_generation == UINT64_MAX) {
    tstr_freep(&delivery->packet);
    delivery->expiry_at_epoch_seconds = 0u;
    delivery->state = FLOWIE_SESSION_DELIVERY_RESERVED;
    return SALTS_ERANGE;
  }
  owner->resource_generation += 1u;
  return SALTS_OK;
}

int flowie_session_owner_delivery_commit(flowie_session_owner_t *owner, uint16_t packet_id,
                                         flowie_mqtt_span_t packet,
                                         uint64_t expiry_at_epoch_seconds) {
  return flowie_session_owner_delivery_commit_state(owner, packet_id, packet,
                                                    expiry_at_epoch_seconds, 0);
}

int flowie_session_owner_delivery_commit_queued(flowie_session_owner_t *owner, uint16_t packet_id,
                                                flowie_mqtt_span_t packet,
                                                uint64_t expiry_at_epoch_seconds) {
  return flowie_session_owner_delivery_commit_state(owner, packet_id, packet,
                                                    expiry_at_epoch_seconds, 1);
}

int flowie_session_owner_delivery_cancel(flowie_session_owner_t *owner, uint16_t packet_id) {
  flowie_session_delivery_t removed;
  size_t index;
  int rc;
  if (!owner || packet_id == 0u) return SALTS_EINVAL;
  rc = flowie_session_delivery_lookup(owner, packet_id, NULL, &index);
  if (rc != SALTS_OK) return rc;
  memset(&removed, 0, sizeof(removed));
  rc = flowie_session_delivery_remove_at(owner, index, &removed);
  if (rc != SALTS_OK) return rc;
  tstr_freep(&removed.packet);
  return SALTS_OK;
}

int flowie_session_owner_delivery_expire(flowie_session_owner_t *owner, uint64_t now_epoch_seconds,
                                         size_t *removed_count) {
  size_t count = 0u;
  size_t index = 0u;
  if (!owner || !removed_count || now_epoch_seconds == 0u) return SALTS_EINVAL;
  *removed_count = 0u;
  for (size_t i = 0u; i < vec_size(&owner->deliveries); ++i) {
    const flowie_session_delivery_t *delivery =
        (const flowie_session_delivery_t *)vec_at_const(&owner->deliveries, i);
    if (delivery && delivery->expiry_at_epoch_seconds != 0u &&
        delivery->expiry_at_epoch_seconds <= now_epoch_seconds)
      count += 1u;
  }
  if (count == 0u) return SALTS_OK;
  if (owner->resource_generation == UINT64_MAX) return SALTS_ERANGE;
  while (index < vec_size(&owner->deliveries)) {
    flowie_session_delivery_t *delivery =
        (flowie_session_delivery_t *)vec_at(&owner->deliveries, index);
    if (!delivery || delivery->expiry_at_epoch_seconds == 0u ||
        delivery->expiry_at_epoch_seconds > now_epoch_seconds) {
      index += 1u;
      continue;
    }
    {
      flowie_session_delivery_t removed;
      int rc;
      memset(&removed, 0, sizeof(removed));
      rc = flowie_session_delivery_remove_at(owner, index, &removed);
      if (rc != SALTS_OK) return rc;
      tstr_freep(&removed.packet);
    }
  }
  owner->resource_generation += 1u;
  *removed_count = count;
  return SALTS_OK;
}

int flowie_session_owner_delivery_expire_packet(flowie_session_owner_t *owner, uint16_t packet_id,
                                                uint64_t now_epoch_seconds, int *removed) {
  flowie_session_delivery_t *delivery;
  flowie_session_delivery_t removed_delivery;
  size_t index;
  int rc;
  if (!owner || packet_id == 0u || now_epoch_seconds == 0u || !removed) return SALTS_EINVAL;
  *removed = 0;
  rc = flowie_session_delivery_lookup(owner, packet_id, &delivery, &index);
  if (rc != SALTS_OK) return rc;
  if (delivery->expiry_at_epoch_seconds == 0u ||
      delivery->expiry_at_epoch_seconds > now_epoch_seconds)
    return SALTS_OK;
  if (owner->resource_generation == UINT64_MAX) return SALTS_ERANGE;
  memset(&removed_delivery, 0, sizeof(removed_delivery));
  rc = flowie_session_delivery_remove_at(owner, index, &removed_delivery);
  if (rc != SALTS_OK) return rc;
  tstr_freep(&removed_delivery.packet);
  owner->resource_generation += 1u;
  *removed = 1;
  return SALTS_OK;
}

int flowie_session_delivery_packet_expiry_refresh(flowie_mqtt_version_t version, uint8_t *packet,
                                                  size_t packet_size,
                                                  uint64_t expiry_at_epoch_seconds,
                                                  uint64_t now_epoch_seconds) {
  flowie_mqtt_parse_options_t options = FLOWIE_MQTT_PARSE_OPTIONS_INIT;
  flowie_mqtt_packet_view_t packet_view = FLOWIE_MQTT_PACKET_VIEW_INIT;
  flowie_mqtt_publish_view_t publish = FLOWIE_MQTT_PUBLISH_VIEW_INIT;
  flowie_mqtt_property_iterator_t iterator = FLOWIE_MQTT_PROPERTY_ITERATOR_INIT;
  flowie_mqtt_property_view_t property = FLOWIE_MQTT_PROPERTY_VIEW_INIT;
  size_t consumed = 0u;
  int found = 0;
  int rc;
  if (!packet || packet_size == 0u || expiry_at_epoch_seconds == 0u || now_epoch_seconds == 0u ||
      expiry_at_epoch_seconds <= now_epoch_seconds || version != FLOWIE_MQTT_VERSION_5)
    return SALTS_EINVAL;
  options.version = version;
  options.max_packet_size = packet_size;
  rc = flowie_mqtt_packet_parse(packet, packet_size, &options, &packet_view, &consumed, NULL);
  if (rc != FLOWIE_MQTT_PARSE_OK || consumed != packet_size ||
      flowie_mqtt_publish_parse(&packet_view, &publish) != FLOWIE_MQTT_PARSE_OK)
    return SALTS_EPROTO;
  rc = flowie_mqtt_property_iterator_init(&publish.properties, &iterator);
  if (rc != FLOWIE_MQTT_PARSE_OK) return SALTS_EPROTO;
  for (;;) {
    const uint8_t *begin = iterator.cursor;
    rc = flowie_mqtt_property_iterator_next(&iterator, &property);
    if (rc == FLOWIE_MQTT_PARSE_NEED_MORE) break;
    if (rc != FLOWIE_MQTT_PARSE_OK || !begin || iterator.cursor < begin) return SALTS_EPROTO;
    if (property.identifier == FLOWIE_MQTT_PROPERTY_MESSAGE_EXPIRY_INTERVAL) {
      uint64_t remaining = expiry_at_epoch_seconds - now_epoch_seconds;
      uint32_t interval = remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;
      size_t property_size = (size_t)(iterator.cursor - begin);
      uint8_t *value;
      if (found || property_size < sizeof(uint32_t)) return SALTS_EPROTO;
      value = packet + (size_t)(iterator.cursor - packet) - sizeof(uint32_t);
      value[0] = (uint8_t)(interval >> 24u);
      value[1] = (uint8_t)(interval >> 16u);
      value[2] = (uint8_t)(interval >> 8u);
      value[3] = (uint8_t)interval;
      found = 1;
    }
  }
  return found ? SALTS_OK : SALTS_EPROTO;
}

int flowie_session_owner_delivery_pending_at_ex(flowie_session_owner_t *owner, size_t index,
                                                uint64_t now_epoch_seconds,
                                                flowie_mqtt_span_t *packet, uint16_t *packet_id,
                                                uint64_t *expiry_at_epoch_seconds) {
  flowie_session_delivery_t *delivery;
  int rc;
  if (!owner || !packet || !packet_id || !expiry_at_epoch_seconds) return SALTS_EINVAL;
  delivery = (flowie_session_delivery_t *)vec_at(&owner->deliveries, index);
  if (!delivery || !delivery->packet || delivery->state == FLOWIE_SESSION_DELIVERY_RESERVED)
    return SALTS_ENOENT;
  if (delivery->expiry_at_epoch_seconds != 0u) {
    if (now_epoch_seconds == 0u || delivery->expiry_at_epoch_seconds <= now_epoch_seconds)
      return SALTS_EBUSY;
    if (owner->version == FLOWIE_MQTT_VERSION_5) {
      rc = flowie_session_delivery_packet_expiry_refresh(
          owner->version, (uint8_t *)delivery->packet, tstr_len(delivery->packet),
          delivery->expiry_at_epoch_seconds, now_epoch_seconds);
      if (rc != SALTS_OK) return rc;
    } else if (!flowie_mqtt_version_is_3x(owner->version)) {
      return SALTS_EPROTO;
    }
  }
  if (delivery->state == FLOWIE_SESSION_DELIVERY_QUEUED) {
    delivery->state = delivery->qos == 1u ? FLOWIE_SESSION_DELIVERY_WAIT_ACK
                                          : FLOWIE_SESSION_DELIVERY_WAIT_PUBREC;
  } else if ((delivery->state == FLOWIE_SESSION_DELIVERY_WAIT_ACK ||
              delivery->state == FLOWIE_SESSION_DELIVERY_WAIT_PUBREC) &&
             tstr_len(delivery->packet) != 0u &&
             ((uint8_t)delivery->packet[0] >> 4u) == FLOWIE_MQTT_PACKET_PUBLISH) {
    delivery->packet[0] = (char)((uint8_t)delivery->packet[0] | UINT8_C(0x08));
  }
  packet->data = (const uint8_t *)delivery->packet;
  packet->size = tstr_len(delivery->packet);
  *packet_id = delivery->packet_id;
  *expiry_at_epoch_seconds = delivery->expiry_at_epoch_seconds;
  return SALTS_OK;
}

int flowie_session_owner_delivery_pending_at(flowie_session_owner_t *owner, size_t index,
                                             flowie_mqtt_span_t *packet) {
  uint16_t packet_id = 0u;
  uint64_t expiry_at_epoch_seconds = 0u;
  flowie_session_delivery_t *delivery;
  if (!owner || !packet) return SALTS_EINVAL;
  delivery = (flowie_session_delivery_t *)vec_at(&owner->deliveries, index);
  if (!delivery) return SALTS_ENOENT;
  if (delivery->expiry_at_epoch_seconds != 0u) return SALTS_EBUSY;
  return flowie_session_owner_delivery_pending_at_ex(owner, index, 0u, packet, &packet_id,
                                                     &expiry_at_epoch_seconds);
}

int flowie_session_owner_delivery_ack(flowie_session_owner_t *owner,
                                      const flowie_mqtt_packet_view_t *packet,
                                      flowie_session_ack_intent_t *reply) {
  flowie_mqtt_control_packet_view_t control = FLOWIE_MQTT_CONTROL_PACKET_VIEW_INIT;
  flowie_session_delivery_t *delivery;
  flowie_session_delivery_t removed;
  flowie_mqtt_control_packet_t pubrel = FLOWIE_MQTT_CONTROL_PACKET_INIT;
  uint8_t encoded[32];
  size_t written = 0u;
  size_t index;
  int rc;
  if (!owner || !packet || packet->size < sizeof(*packet) || !reply || reply->size < sizeof(*reply) ||
      reply->abi_version != FLOWIE_SESSION_INTERNAL_ABI_V1)
    return SALTS_EINVAL;
  if (!owner->active || packet->version != owner->version) return SALTS_EBUSY;
  if (packet->type != FLOWIE_MQTT_PACKET_PUBACK && packet->type != FLOWIE_MQTT_PACKET_PUBREC &&
      packet->type != FLOWIE_MQTT_PACKET_PUBCOMP)
    return SALTS_EPROTO;
  rc = flowie_mqtt_control_packet_parse(packet, &control);
  if (rc != FLOWIE_MQTT_PARSE_OK) return SALTS_EPROTO;
  rc = flowie_session_delivery_lookup(owner, control.packet_id, &delivery, &index);
  if (rc != SALTS_OK || delivery->state == FLOWIE_SESSION_DELIVERY_RESERVED)
    return rc == SALTS_OK ? SALTS_ENOENT : rc;
  *reply = (flowie_session_ack_intent_t)FLOWIE_SESSION_ACK_INTENT_INIT;
  if (packet->type == FLOWIE_MQTT_PACKET_PUBACK) {
    if (delivery->qos != 1u || delivery->state != FLOWIE_SESSION_DELIVERY_WAIT_ACK)
      return SALTS_EPROTO;
  } else if (packet->type == FLOWIE_MQTT_PACKET_PUBREC) {
    if (delivery->qos != 2u ||
        (delivery->state != FLOWIE_SESSION_DELIVERY_WAIT_PUBREC &&
         delivery->state != FLOWIE_SESSION_DELIVERY_WAIT_PUBCOMP))
      return SALTS_EPROTO;
    if (control.reason_code < UINT8_C(0x80)) {
      if (delivery->state == FLOWIE_SESSION_DELIVERY_WAIT_PUBREC) {
        tstr owned;
        if (owner->resource_generation == UINT64_MAX) return SALTS_ERANGE;
        pubrel.version = owner->version;
        pubrel.type = FLOWIE_MQTT_PACKET_PUBREL;
        pubrel.packet_id = control.packet_id;
        rc = flowie_mqtt_control_packet_encode(&pubrel, encoded, sizeof(encoded), &written);
        if (rc != FLOWIE_MQTT_PARSE_OK) return SALTS_EPROTO;
        owned = tstr_new_len(encoded, written);
        if (!owned) return SALTS_ENOMEM;
        tstr_freep(&delivery->packet);
        delivery->packet = owned;
        delivery->state = FLOWIE_SESSION_DELIVERY_WAIT_PUBCOMP;
        delivery->expiry_at_epoch_seconds = 0u;
      }
      reply->kind = FLOWIE_SESSION_ACK_PUBREL;
      reply->packet_id = control.packet_id;
      if (owner->resource_generation == UINT64_MAX) return SALTS_ERANGE;
      owner->resource_generation += 1u;
      return SALTS_OK;
    }
  } else if (delivery->qos != 2u ||
             delivery->state != FLOWIE_SESSION_DELIVERY_WAIT_PUBCOMP) {
    return SALTS_EPROTO;
  }
  if (owner->resource_generation == UINT64_MAX) return SALTS_ERANGE;
  memset(&removed, 0, sizeof(removed));
  rc = flowie_session_delivery_remove_at(owner, index, &removed);
  if (rc != SALTS_OK) return rc;
  tstr_freep(&removed.packet);
  owner->resource_generation += 1u;
  return SALTS_OK;
}

static int flowie_session_subscription_remove(vec_t *subscriptions,
                                              flowie_mqtt_span_t filter, int *removed) {
  if (!subscriptions || !removed) return SALTS_EINVAL;
  *removed = 0;
  for (size_t i = 0u; i < vec_size(subscriptions); ++i) {
    flowie_session_subscription_owned_t *entry =
        (flowie_session_subscription_owned_t *)vec_at(subscriptions, i);
    flowie_session_subscription_owned_t owned;
    int rc;
    if (!entry || !entry->filter || tstr_len(entry->filter) != filter.size ||
        memcmp(entry->filter, filter.data, filter.size) != 0)
      continue;
    memset(&owned, 0, sizeof(owned));
    rc = flowie_stl_error(vec_swap_remove(subscriptions, i, &owned));
    if (rc != SALTS_OK) return rc;
    tstr_freep(&owned.filter);
    *removed = 1;
    return SALTS_OK;
  }
  return SALTS_OK;
}

int flowie_session_owner_unsubscribe(flowie_session_owner_t *owner,
                                     const flowie_mqtt_packet_view_t *packet,
                                     const flowie_mqtt_unsubscribe_view_t *unsubscribe,
                                     uint8_t *reason_codes, size_t reason_code_capacity,
                                     flowie_session_unsubscribe_result_t *out) {
  flowie_mqtt_topic_filter_iterator_t iterator = FLOWIE_MQTT_TOPIC_FILTER_ITERATOR_INIT;
  flowie_session_unsubscribe_result_t result = FLOWIE_SESSION_UNSUBSCRIBE_RESULT_INIT;
  flowie_mqtt_span_t filter;
  vec_t staged = {0};
  tstr staged_reasons = NULL;
  size_t count = 0u;
  size_t removed_count = 0u;
  int rc;
  if (!owner || !packet || packet->size < sizeof(*packet) || !unsubscribe ||
      unsubscribe->size < sizeof(*unsubscribe) || !out || out->size < sizeof(*out) ||
      out->abi_version != FLOWIE_SESSION_INTERNAL_ABI_V1 ||
      (!reason_codes && reason_code_capacity != 0u))
    return SALTS_EINVAL;
  if (!owner->active) return SALTS_EBUSY;
  if (packet->abi_version != FLOWIE_MQTT_PROTOCOL_ABI_V1 ||
      unsubscribe->abi_version != FLOWIE_MQTT_PROTOCOL_ABI_V1 ||
      packet->type != FLOWIE_MQTT_PACKET_UNSUBSCRIBE || packet->version != owner->version ||
      unsubscribe->packet_id == 0u || unsubscribe->filter_count == 0u)
    return SALTS_EPROTO;
  if (packet->version == FLOWIE_MQTT_VERSION_5) {
    if (!reason_codes || reason_code_capacity < unsubscribe->filter_count) return SALTS_ENOSPC;
    staged_reasons = tstr_new_len(NULL, unsubscribe->filter_count);
    if (!staged_reasons) return SALTS_ENOMEM;
  }
  rc = flowie_mqtt_topic_filter_iterator_init(unsubscribe, &iterator);
  if (rc != FLOWIE_MQTT_PARSE_OK) {
    rc = SALTS_EPROTO;
    goto fail_reasons;
  }
  rc = flowie_session_subscriptions_clone(&owner->subscriptions, &staged);
  if (rc != SALTS_OK) goto fail_reasons;
  while ((rc = flowie_mqtt_topic_filter_iterator_next(&iterator, &filter)) ==
         FLOWIE_MQTT_PARSE_OK) {
    int removed = 0;
    rc = flowie_session_subscription_remove(&staged, filter, &removed);
    if (rc != SALTS_OK) goto fail;
    if (staged_reasons) staged_reasons[count] = removed ? UINT8_C(0x00) : UINT8_C(0x11);
    removed_count += (size_t)removed;
    ++count;
  }
  if (rc != FLOWIE_MQTT_PARSE_NEED_MORE || count != unsubscribe->filter_count) {
    rc = SALTS_EPROTO;
    goto fail;
  }
  if (removed_count != 0u) {
    vec_t previous;
    rc = flowie_session_generation_advance(owner);
    if (rc != SALTS_OK) goto fail;
    previous = owner->subscriptions;
    owner->subscriptions = staged;
    staged = previous;
  }
  flowie_session_subscriptions_destroy(&staged);
  result.packet_id = unsubscribe->packet_id;
  result.filter_count = count;
  result.removed_count = removed_count;
  result.changed = (uint8_t)(removed_count != 0u);
  if (staged_reasons) {
    memcpy(reason_codes, staged_reasons, count);
    result.reason_codes = (flowie_mqtt_span_t){reason_codes, count};
  }
  tstr_freep(&staged_reasons);
  *out = result;
  return SALTS_OK;

fail:
  flowie_session_subscriptions_destroy(&staged);
fail_reasons:
  tstr_freep(&staged_reasons);
  return rc;
}

static flowie_session_inflight_t *
flowie_session_inflight_find(flowie_session_owner_t *owner, uint16_t packet_id, size_t *index_out) {
  for (size_t i = 0u; i < vec_size(&owner->inflight); ++i) {
    flowie_session_inflight_t *entry =
        (flowie_session_inflight_t *)vec_at(&owner->inflight, i);
    if (entry && entry->packet_id == packet_id) {
      if (index_out) *index_out = i;
      return entry;
    }
  }
  return NULL;
}

static int flowie_session_route_check(const flowie_session_owner_t *owner,
                                      const flowie_protocol_route_t *route) {
  if (!route || route->size < sizeof(*route) ||
      route->contract_version != FLOWIE_PROTOCOL_CONTRACT_VERSION)
    return SALTS_EINVAL;
  if (route->protocol != FLOWIE_PROTOCOL_MQTT) return SALTS_EPROTO;
  if (route->owner_instance_id != owner->config.owner_instance_id ||
      route->session_id != owner->config.session_id ||
      route->session_generation != owner->session_generation)
    return SALTS_EBUSY;
  return SALTS_OK;
}

static flowie_session_ack_intent_t flowie_session_ack(flowie_session_ack_kind_t kind,
                                                      uint16_t packet_id) {
  flowie_session_ack_intent_t ack = FLOWIE_SESSION_ACK_INTENT_INIT;
  ack.kind = kind;
  ack.packet_id = packet_id;
  return ack;
}

int flowie_session_ack_control_packet(const flowie_session_ack_intent_t *ack,
                                      flowie_mqtt_version_t version,
                                      flowie_mqtt_control_packet_t *out) {
  flowie_mqtt_control_packet_t packet = FLOWIE_MQTT_CONTROL_PACKET_INIT;
  if (!ack || ack->size < sizeof(*ack) || ack->abi_version != FLOWIE_SESSION_INTERNAL_ABI_V1 ||
      !out || out->size < sizeof(*out) || out->abi_version != FLOWIE_MQTT_PROTOCOL_ABI_V1 ||
      ack->packet_id == 0u ||
      !flowie_mqtt_version_is_supported(version))
    return SALTS_EINVAL;
  switch (ack->kind) {
  case FLOWIE_SESSION_ACK_PUBACK:
    packet.type = FLOWIE_MQTT_PACKET_PUBACK;
    break;
  case FLOWIE_SESSION_ACK_PUBREC:
    packet.type = FLOWIE_MQTT_PACKET_PUBREC;
    break;
  case FLOWIE_SESSION_ACK_PUBREL:
    packet.type = FLOWIE_MQTT_PACKET_PUBREL;
    break;
  case FLOWIE_SESSION_ACK_PUBCOMP:
    packet.type = FLOWIE_MQTT_PACKET_PUBCOMP;
    break;
  default:
    return SALTS_EPROTO;
  }
  if (flowie_mqtt_version_is_3x(version) && ack->reason_code != 0u) return SALTS_EPROTO;
  packet.version = version;
  packet.packet_id = ack->packet_id;
  packet.reason_code = ack->reason_code;
  *out = packet;
  return SALTS_OK;
}

int flowie_session_owner_publish_begin(flowie_session_owner_t *owner,
                                       const flowie_mqtt_publish_view_t *publish,
                                       flowie_session_publish_begin_result_t *out) {
  flowie_session_publish_begin_result_t result = FLOWIE_SESSION_PUBLISH_BEGIN_RESULT_INIT;
  flowie_session_inflight_t *existing;
  int rc;
  if (!owner || !publish || publish->size < sizeof(*publish) || !out || out->size < sizeof(*out) ||
      out->abi_version != FLOWIE_SESSION_INTERNAL_ABI_V1)
    return SALTS_EINVAL;
  if (!owner->active) return SALTS_EBUSY;
  rc = flowie_publish_message_map(publish, owner->version, owner->config.owner_instance_id,
                                  owner->config.session_id, owner->session_generation,
                                  &result.message);
  if (rc != SALTS_OK) return rc;
  if (publish->qos == 0u) {
    result.admit_application = 1u;
    *out = result;
    return SALTS_OK;
  }
  existing = flowie_session_inflight_find(owner, publish->packet_id, NULL);
  if (existing) {
    if (!publish->duplicate || existing->qos != publish->qos) return SALTS_EPROTO;
    if (existing->state == FLOWIE_SESSION_INFLIGHT_QOS2_RELEASE) {
      result.has_ack = 1u;
      result.ack = flowie_session_ack(FLOWIE_SESSION_ACK_PUBREC, publish->packet_id);
    }
    *out = result;
    return SALTS_OK;
  }
  if (vec_size(&owner->inflight) >= owner->config.max_inflight) return SALTS_ENOSPC;
  if (owner->resource_generation == UINT64_MAX) return SALTS_ERANGE;
  {
    flowie_session_inflight_t added;
    added.packet_id = publish->packet_id;
    added.qos = publish->qos;
    added.state = FLOWIE_SESSION_INFLIGHT_SETTLEMENT;
    rc = flowie_stl_error(vec_push(&owner->inflight, &added));
    if (rc != SALTS_OK) return rc;
  }
  owner->resource_generation += 1u;
  result.admit_application = 1u;
  *out = result;
  return SALTS_OK;
}

static flowie_protocol_settlement_point_t
flowie_session_required_settlement(const flowie_session_owner_t *owner, uint8_t qos) {
  return qos == 1u ? owner->config.settlement.qos1 : owner->config.settlement.qos2;
}

int flowie_session_owner_publish_settle(flowie_session_owner_t *owner,
                                        const flowie_protocol_route_t *route,
                                        const flowie_protocol_settlement_request_t *request,
                                        flowie_session_ack_intent_t *out) {
  flowie_session_inflight_t *entry;
  flowie_session_ack_intent_t ack;
  size_t index;
  int rc;
  if (!owner || !request || request->size < sizeof(*request) || !out || out->size < sizeof(*out) ||
      out->abi_version != FLOWIE_SESSION_INTERNAL_ABI_V1)
    return SALTS_EINVAL;
  if (!owner->active) return SALTS_EBUSY;
  rc = flowie_session_route_check(owner, route);
  if (rc != SALTS_OK) return rc;
  rc = flowie_protocol_message_validate(&request->message);
  if (rc != SALTS_OK || request->message.protocol != FLOWIE_PROTOCOL_MQTT ||
      request->message.protocol_version != (uint32_t)owner->version ||
      request->message.kind != FLOWIE_PROTOCOL_MESSAGE_DATA || request->message.qos == 0u ||
      request->message.qos > 2u || request->message.packet_id > UINT16_MAX ||
      request->message.session_generation != owner->session_generation ||
      request->message.packet_id == 0u || request->point < FLOWIE_PROTOCOL_SETTLE_RECEIVED ||
      request->point > FLOWIE_PROTOCOL_SETTLE_DURABLE)
    return SALTS_EPROTO;
  entry = flowie_session_inflight_find(owner, (uint16_t)request->message.packet_id, &index);
  if (!entry) return SALTS_ENOENT;
  if (entry->qos != request->message.qos) return SALTS_EPROTO;
  if (entry->state != FLOWIE_SESSION_INFLIGHT_SETTLEMENT) return SALTS_EALREADY;
  if (request->status != SALTS_OK) return request->status;
  if (request->point < flowie_session_required_settlement(owner, entry->qos)) return SALTS_EBUSY;
  if (owner->resource_generation == UINT64_MAX) return SALTS_ERANGE;
  if (entry->qos == 1u) {
    ack = flowie_session_ack(FLOWIE_SESSION_ACK_PUBACK, entry->packet_id);
    rc = flowie_stl_error(vec_swap_remove(&owner->inflight, index, NULL));
    if (rc != SALTS_OK) return rc;
  } else {
    ack = flowie_session_ack(FLOWIE_SESSION_ACK_PUBREC, entry->packet_id);
    entry->state = FLOWIE_SESSION_INFLIGHT_QOS2_RELEASE;
  }
  owner->resource_generation += 1u;
  *out = ack;
  return SALTS_OK;
}

int flowie_session_owner_qos2_release(flowie_session_owner_t *owner,
                                      const flowie_protocol_route_t *route, uint16_t packet_id,
                                      flowie_session_ack_intent_t *out) {
  flowie_session_inflight_t *entry;
  flowie_session_ack_intent_t ack;
  size_t index;
  int rc;
  if (!owner || packet_id == 0u || !out || out->size < sizeof(*out) ||
      out->abi_version != FLOWIE_SESSION_INTERNAL_ABI_V1)
    return SALTS_EINVAL;
  if (!owner->active) return SALTS_EBUSY;
  rc = flowie_session_route_check(owner, route);
  if (rc != SALTS_OK) return rc;
  entry = flowie_session_inflight_find(owner, packet_id, &index);
  if (!entry) return SALTS_ENOENT;
  if (entry->qos != 2u || entry->state != FLOWIE_SESSION_INFLIGHT_QOS2_RELEASE) return SALTS_EPROTO;
  if (owner->resource_generation == UINT64_MAX) return SALTS_ERANGE;
  ack = flowie_session_ack(FLOWIE_SESSION_ACK_PUBCOMP, packet_id);
  rc = flowie_stl_error(vec_swap_remove(&owner->inflight, index, NULL));
  if (rc != SALTS_OK) return rc;
  owner->resource_generation += 1u;
  *out = ack;
  return SALTS_OK;
}

#define FLOWIE_SESSION_RECORD_HEADER_SIZE 8u
#define FLOWIE_SESSION_RECORD_METADATA_SIZE 25u
#define FLOWIE_SESSION_RECORD_VERSION_MAJOR 1u
#define FLOWIE_SESSION_RECORD_VERSION_MINOR 3u
#define FLOWIE_SESSION_RECORD_WILL_METADATA_SIZE 7u

static void flowie_session_record_write_u16(uint8_t *out, uint16_t value) {
  out[0] = (uint8_t)(value >> 8u);
  out[1] = (uint8_t)value;
}

static void flowie_session_record_write_u32(uint8_t *out, uint32_t value) {
  out[0] = (uint8_t)(value >> 24u);
  out[1] = (uint8_t)(value >> 16u);
  out[2] = (uint8_t)(value >> 8u);
  out[3] = (uint8_t)value;
}

static void flowie_session_record_write_u64(uint8_t *out, uint64_t value) {
  for (size_t i = 0u; i < 8u; ++i) out[i] = (uint8_t)(value >> (56u - i * 8u));
}

static uint16_t flowie_session_record_read_u16(const uint8_t *data) {
  return (uint16_t)(((uint16_t)data[0] << 8u) | data[1]);
}

static uint32_t flowie_session_record_read_u32(const uint8_t *data) {
  return ((uint32_t)data[0] << 24u) | ((uint32_t)data[1] << 16u) | ((uint32_t)data[2] << 8u) |
         data[3];
}

static uint64_t flowie_session_record_read_u64(const uint8_t *data) {
  uint64_t value = 0u;
  for (size_t i = 0u; i < 8u; ++i) value = (value << 8u) | data[i];
  return value;
}

static int flowie_session_record_size_add(size_t *total, size_t value_size) {
  size_t wire_size = ltv_wire_size(value_size);
  if (!total || wire_size == 0u || *total > SIZE_MAX - wire_size) return SALTS_ERANGE;
  *total += wire_size;
  return SALTS_OK;
}

static int flowie_session_record_append(uint8_t *out, size_t capacity, size_t *offset,
                                        uint8_t type, const uint8_t *value, size_t value_size) {
  size_t wire_size;
  size_t written;
  if (!out || !offset || type == 0u || (!value && value_size != 0u) || *offset > capacity)
    return SALTS_EINVAL;
  wire_size = ltv_wire_size(value_size);
  if (wire_size == 0u || wire_size > capacity - *offset) return SALTS_ENOSPC;
  written = ltv_build(type, value, value_size, out + *offset, capacity - *offset);
  if (written != wire_size) return SALTS_EPROTO;
  *offset += written;
  return SALTS_OK;
}

int flowie_session_owner_record_encode(const flowie_session_owner_t *owner, uint8_t *out,
                                       size_t capacity, size_t *out_size) {
  uint8_t header[FLOWIE_SESSION_RECORD_HEADER_SIZE] = {'F', 'S', 'E', 'S', 0u,
                                                       FLOWIE_SESSION_RECORD_VERSION_MAJOR, 0u,
                                                       FLOWIE_SESSION_RECORD_VERSION_MINOR};
  uint8_t metadata[FLOWIE_SESSION_RECORD_METADATA_SIZE];
  uint8_t will_metadata[FLOWIE_SESSION_RECORD_WILL_METADATA_SIZE];
  size_t required = 0u;
  size_t offset = 0u;
  int rc;
  if (!owner || !out_size || (!out && capacity != 0u) || !owner->initialized ||
      !owner->client_id || owner->session_generation == 0u || owner->resource_generation == 0u)
    return SALTS_EINVAL;
  rc = flowie_session_record_size_add(&required, sizeof(header));
  if (rc == SALTS_OK) rc = flowie_session_record_size_add(&required, sizeof(metadata));
  for (size_t i = 0u; rc == SALTS_OK && i < vec_size(&owner->subscriptions); ++i) {
    const flowie_session_subscription_owned_t *entry =
        (const flowie_session_subscription_owned_t *)vec_at_const(&owner->subscriptions, i);
    if (!entry || !entry->filter) return SALTS_EPROTO;
    rc = flowie_session_record_size_add(&required, tstr_len(entry->filter));
  }
  for (size_t i = 0u; rc == SALTS_OK && i < vec_size(&owner->subscriptions); ++i)
    rc = flowie_session_record_size_add(&required, 8u);
  for (size_t i = 0u; rc == SALTS_OK && i < vec_size(&owner->inflight); ++i) {
    const flowie_session_inflight_t *entry =
        (const flowie_session_inflight_t *)vec_at_const(&owner->inflight, i);
    if (entry && entry->state == FLOWIE_SESSION_INFLIGHT_QOS2_RELEASE)
      rc = flowie_session_record_size_add(&required, 3u);
  }
  for (size_t i = 0u; rc == SALTS_OK && i < vec_size(&owner->deliveries); ++i) {
    const flowie_session_delivery_t *entry =
        (const flowie_session_delivery_t *)vec_at_const(&owner->deliveries, i);
    if (entry && entry->state != FLOWIE_SESSION_DELIVERY_RESERVED) {
      if (!entry->packet) return SALTS_EPROTO;
      rc = flowie_session_record_size_add(&required, 12u);
    }
  }
  for (size_t i = 0u; rc == SALTS_OK && i < vec_size(&owner->deliveries); ++i) {
    const flowie_session_delivery_t *entry =
        (const flowie_session_delivery_t *)vec_at_const(&owner->deliveries, i);
    if (entry && entry->state != FLOWIE_SESSION_DELIVERY_RESERVED)
      rc = flowie_session_record_size_add(&required, tstr_len(entry->packet));
  }
  if (rc == SALTS_OK && owner->has_will) {
    if (!owner->will_topic || !owner->will_properties || !owner->will_payload ||
        owner->will_qos > 2u || owner->will_retain > 1u ||
        !flowie_mqtt_topic_name_validate((flowie_mqtt_span_t){
            (const uint8_t *)owner->will_topic, tstr_len(owner->will_topic)}))
      return SALTS_EPROTO;
    rc = flowie_session_record_size_add(&required, sizeof(will_metadata));
    if (rc == SALTS_OK)
      rc = flowie_session_record_size_add(&required, tstr_len(owner->will_topic));
    if (rc == SALTS_OK)
      rc = flowie_session_record_size_add(&required, tstr_len(owner->will_properties));
    if (rc == SALTS_OK)
      rc = flowie_session_record_size_add(&required, tstr_len(owner->will_payload));
  }
  if (rc != SALTS_OK) return rc;
  *out_size = required;
  if (!out || capacity < required) return SALTS_ENOSPC;
  memset(metadata, 0, sizeof(metadata));
  metadata[0] = (uint8_t)owner->version;
  flowie_session_record_write_u64(metadata + 1u, owner->config.session_id);
  flowie_session_record_write_u64(metadata + 9u, owner->session_generation);
  flowie_session_record_write_u16(metadata + 17u, owner->keep_alive);
  flowie_session_record_write_u32(metadata + 19u, owner->session_expiry_interval);
  flowie_session_record_write_u16(metadata + 23u, owner->next_delivery_packet_id);
  rc = flowie_session_record_append(out, capacity, &offset, 1u, header, sizeof(header));
  if (rc == SALTS_OK)
    rc = flowie_session_record_append(out, capacity, &offset, 2u, metadata, sizeof(metadata));
  for (size_t i = 0u; rc == SALTS_OK && i < vec_size(&owner->subscriptions); ++i) {
    const flowie_session_subscription_owned_t *entry =
        (const flowie_session_subscription_owned_t *)vec_at_const(&owner->subscriptions, i);
    rc = flowie_session_record_append(out, capacity, &offset, 3u,
                                      (const uint8_t *)entry->filter, tstr_len(entry->filter));
  }
  for (size_t i = 0u; rc == SALTS_OK && i < vec_size(&owner->subscriptions); ++i) {
    const flowie_session_subscription_owned_t *entry =
        (const flowie_session_subscription_owned_t *)vec_at_const(&owner->subscriptions, i);
    uint8_t options[8] = {entry->qos, entry->no_local, entry->retain_as_published,
                          entry->retain_handling, 0u, 0u, 0u, 0u};
    flowie_session_record_write_u32(options + 4u, entry->subscription_identifier);
    rc = flowie_session_record_append(out, capacity, &offset, 4u, options, sizeof(options));
  }
  for (size_t i = 0u; rc == SALTS_OK && i < vec_size(&owner->inflight); ++i) {
    const flowie_session_inflight_t *entry =
        (const flowie_session_inflight_t *)vec_at_const(&owner->inflight, i);
    uint8_t state[3];
    if (!entry || entry->state != FLOWIE_SESSION_INFLIGHT_QOS2_RELEASE) continue;
    flowie_session_record_write_u16(state, entry->packet_id);
    state[2] = entry->qos;
    rc = flowie_session_record_append(out, capacity, &offset, 5u, state, sizeof(state));
  }
  for (size_t i = 0u; rc == SALTS_OK && i < vec_size(&owner->deliveries); ++i) {
    const flowie_session_delivery_t *entry =
        (const flowie_session_delivery_t *)vec_at_const(&owner->deliveries, i);
    uint8_t state[12];
    flowie_session_delivery_state_t persisted_state;
    if (!entry || entry->state == FLOWIE_SESSION_DELIVERY_RESERVED) continue;
    flowie_session_record_write_u16(state, entry->packet_id);
    state[2] = entry->qos;
    persisted_state = entry->state == FLOWIE_SESSION_DELIVERY_QUEUED
                          ? entry->qos == 1u ? FLOWIE_SESSION_DELIVERY_WAIT_ACK
                                             : FLOWIE_SESSION_DELIVERY_WAIT_PUBREC
                          : entry->state;
    state[3] = (uint8_t)persisted_state;
    flowie_session_record_write_u64(state + 4u, entry->expiry_at_epoch_seconds);
    rc = flowie_session_record_append(out, capacity, &offset, 6u, state, sizeof(state));
  }
  for (size_t i = 0u; rc == SALTS_OK && i < vec_size(&owner->deliveries); ++i) {
    const flowie_session_delivery_t *entry =
        (const flowie_session_delivery_t *)vec_at_const(&owner->deliveries, i);
    if (!entry || entry->state == FLOWIE_SESSION_DELIVERY_RESERVED) continue;
    rc = flowie_session_record_append(out, capacity, &offset, 7u,
                                      (const uint8_t *)entry->packet, tstr_len(entry->packet));
  }
  if (rc == SALTS_OK && owner->has_will) {
    will_metadata[0] = (uint8_t)(owner->will_pending || owner->active);
    will_metadata[1] = owner->will_qos;
    will_metadata[2] = owner->will_retain;
    flowie_session_record_write_u32(will_metadata + 3u, owner->will_delay_interval);
    rc = flowie_session_record_append(out, capacity, &offset, 8u, will_metadata,
                                      sizeof(will_metadata));
    if (rc == SALTS_OK)
      rc = flowie_session_record_append(out, capacity, &offset, 9u,
                                        (const uint8_t *)owner->will_topic,
                                        tstr_len(owner->will_topic));
    if (rc == SALTS_OK)
      rc = flowie_session_record_append(out, capacity, &offset, 10u,
                                        (const uint8_t *)owner->will_properties,
                                        tstr_len(owner->will_properties));
    if (rc == SALTS_OK)
      rc = flowie_session_record_append(out, capacity, &offset, 11u,
                                        (const uint8_t *)owner->will_payload,
                                        tstr_len(owner->will_payload));
  }
  return rc == SALTS_OK && offset == required ? SALTS_OK : rc == SALTS_OK ? SALTS_EPROTO : rc;
}

static int flowie_session_record_delivery_validate(const flowie_session_owner_t *owner,
                                                   const flowie_session_delivery_t *delivery) {
  flowie_mqtt_parse_options_t options = FLOWIE_MQTT_PARSE_OPTIONS_INIT;
  flowie_mqtt_packet_view_t packet = FLOWIE_MQTT_PACKET_VIEW_INIT;
  size_t consumed = 0u;
  int rc;
  if (!owner || !delivery || !delivery->packet) return SALTS_EPROTO;
  options.version = owner->version;
  options.max_packet_size = tstr_len(delivery->packet);
  rc = flowie_mqtt_packet_parse((const uint8_t *)delivery->packet, tstr_len(delivery->packet),
                                &options, &packet, &consumed, NULL);
  if (rc != FLOWIE_MQTT_PARSE_OK || consumed != tstr_len(delivery->packet)) return SALTS_EPROTO;
  if (delivery->state == FLOWIE_SESSION_DELIVERY_WAIT_PUBCOMP) {
    flowie_mqtt_control_packet_view_t control = FLOWIE_MQTT_CONTROL_PACKET_VIEW_INIT;
    if (delivery->qos != 2u || packet.type != FLOWIE_MQTT_PACKET_PUBREL ||
        flowie_mqtt_control_packet_parse(&packet, &control) != FLOWIE_MQTT_PARSE_OK ||
        control.packet_id != delivery->packet_id || delivery->expiry_at_epoch_seconds != 0u)
      return SALTS_EPROTO;
  } else {
    flowie_mqtt_publish_view_t publish = FLOWIE_MQTT_PUBLISH_VIEW_INIT;
    if (packet.type != FLOWIE_MQTT_PACKET_PUBLISH ||
        flowie_mqtt_publish_parse(&packet, &publish) != FLOWIE_MQTT_PARSE_OK ||
        publish.packet_id != delivery->packet_id || publish.qos != delivery->qos ||
        (delivery->state == FLOWIE_SESSION_DELIVERY_WAIT_ACK && delivery->qos != 1u) ||
        (delivery->state == FLOWIE_SESSION_DELIVERY_WAIT_PUBREC && delivery->qos != 2u) ||
        (delivery->state == FLOWIE_SESSION_DELIVERY_QUEUED &&
         (delivery->qos == 0u || delivery->qos > 2u)))
      return SALTS_EPROTO;
    if (delivery->expiry_at_epoch_seconds != 0u && owner->version == FLOWIE_MQTT_VERSION_5) {
      flowie_mqtt_property_iterator_t iterator = FLOWIE_MQTT_PROPERTY_ITERATOR_INIT;
      flowie_mqtt_property_view_t property = FLOWIE_MQTT_PROPERTY_VIEW_INIT;
      int found = 0;
      if (flowie_mqtt_property_iterator_init(&publish.properties, &iterator) !=
          FLOWIE_MQTT_PARSE_OK)
        return SALTS_EPROTO;
      while ((rc = flowie_mqtt_property_iterator_next(&iterator, &property)) ==
             FLOWIE_MQTT_PARSE_OK) {
        if (property.identifier == FLOWIE_MQTT_PROPERTY_MESSAGE_EXPIRY_INTERVAL) {
          if (found) return SALTS_EPROTO;
          found = 1;
        }
      }
      if (!found || rc != FLOWIE_MQTT_PARSE_NEED_MORE) return SALTS_EPROTO;
    } else if (delivery->expiry_at_epoch_seconds != 0u &&
               !flowie_mqtt_version_is_3x(owner->version)) {
      return SALTS_EPROTO;
    }
  }
  return SALTS_OK;
}

int flowie_session_owner_record_restore(const flowie_session_config_t *config,
                                        flowie_mqtt_span_t client_id, uint64_t revision,
                                        const uint8_t *data, size_t data_size,
                                        flowie_session_owner_t **out) {
  flowie_session_owner_t *owner = NULL;
  size_t offset = 0u;
  size_t option_index = 0u;
  size_t packet_index = 0u;
  uint8_t previous_type = 0u;
  uint8_t record_minor = 0u;
  int header_seen = 0;
  int metadata_seen = 0;
  int will_metadata_seen = 0;
  int will_topic_seen = 0;
  int will_properties_seen = 0;
  int will_payload_seen = 0;
  int rc = SALTS_EPROTO;
  if (out) *out = NULL;
  if (!flowie_session_config_valid(config) || !out || !data || data_size == 0u ||
      !client_id.data || client_id.size == 0u || client_id.size > UINT16_MAX ||
      !flowie_mqtt_utf8_validate(client_id) || revision == 0u ||
      revision > (uint64_t)INT64_MAX)
    return SALTS_EINVAL;
  owner = flowie_session_owner_create(config);
  if (!owner) return SALTS_ENOMEM;
  owner->client_id = tstr_new_len(client_id.data, client_id.size);
  if (!owner->client_id) {
    rc = SALTS_ENOMEM;
    goto fail;
  }
  while (offset < data_size) {
    ltv_message_t message = {0};
    uint32_t payload_size = 0u;
    size_t header_size = 0u;
    size_t wire_size;
    uint8_t type;
    const uint8_t *value;
    size_t value_size;
    rc = ltv_peek_size(data + offset, data_size - offset, &payload_size, &header_size);
    if (rc != LTV_PARSE_OK || payload_size == 0u || header_size > data_size - offset ||
        payload_size > data_size - offset - header_size) {
      rc = SALTS_EPROTO;
      goto fail;
    }
    wire_size = header_size + payload_size;
    if (ltv_parse(data + offset, wire_size, &message) != LTV_PARSE_OK) {
      rc = SALTS_EPROTO;
      goto fail;
    }
    type = message.type;
    value = message.value;
    value_size = message.value_size;
    if (type == 0u || type > 11u || type < previous_type ||
        (type == previous_type && (type < 3u || type > 7u)) ||
        (record_minor == 0u && type > 7u)) {
      rc = SALTS_EPROTO;
      goto fail;
    }
    if (type == 1u) {
      if (header_seen || offset != 0u || value_size != FLOWIE_SESSION_RECORD_HEADER_SIZE ||
          memcmp(value, "FSES", 4u) != 0 || value[4] != 0u ||
          value[5] != FLOWIE_SESSION_RECORD_VERSION_MAJOR || value[6] != 0u ||
          value[7] > FLOWIE_SESSION_RECORD_VERSION_MINOR) {
        rc = SALTS_EPROTO;
        goto fail;
      }
      record_minor = value[7];
      header_seen = 1;
    } else if (type == 2u) {
      uint64_t session_id;
      uint64_t session_generation;
      if (!header_seen || metadata_seen || value_size != FLOWIE_SESSION_RECORD_METADATA_SIZE) {
        rc = SALTS_EPROTO;
        goto fail;
      }
      session_id = flowie_session_record_read_u64(value + 1u);
      session_generation = flowie_session_record_read_u64(value + 9u);
      if (!flowie_mqtt_version_is_supported((flowie_mqtt_version_t)value[0]) ||
          session_id == 0u || session_generation == 0u) {
        rc = SALTS_EPROTO;
        goto fail;
      }
      owner->config.session_id = session_id;
      owner->version = (flowie_mqtt_version_t)value[0];
      owner->session_generation = session_generation;
      owner->keep_alive = flowie_session_record_read_u16(value + 17u);
      owner->session_expiry_interval = flowie_session_record_read_u32(value + 19u);
      owner->next_delivery_packet_id = flowie_session_record_read_u16(value + 23u);
      metadata_seen = 1;
    } else if (type == 3u) {
      flowie_mqtt_span_t filter = {value, value_size};
      flowie_session_subscription_owned_t subscription;
      if (!metadata_seen || vec_size(&owner->subscriptions) >= config->max_subscriptions ||
          !flowie_mqtt_topic_filter_validate(filter) ||
          flowie_session_subscription_find(&owner->subscriptions, filter)) {
        rc = SALTS_EPROTO;
        goto fail;
      }
      memset(&subscription, 0, sizeof(subscription));
      subscription.filter = tstr_new_len(value, value_size);
      if (!subscription.filter) {
        rc = SALTS_ENOMEM;
        goto fail;
      }
      rc = flowie_stl_error(vec_push(&owner->subscriptions, &subscription));
      if (rc != SALTS_OK) {
        tstr_freep(&subscription.filter);
        goto fail;
      }
    } else if (type == 4u) {
      flowie_session_subscription_owned_t *subscription =
          (flowie_session_subscription_owned_t *)vec_at(&owner->subscriptions, option_index);
      if (!subscription || value_size != (record_minor >= 2u ? 8u : 4u) || value[0] > 2u ||
          value[1] > 1u ||
          value[2] > 1u || value[3] > 2u) {
        rc = SALTS_EPROTO;
        goto fail;
      }
      subscription->qos = value[0];
      subscription->no_local = value[1];
      subscription->retain_as_published = value[2];
      subscription->retain_handling = value[3];
      subscription->subscription_identifier =
          record_minor >= 2u ? flowie_session_record_read_u32(value + 4u) : 0u;
      if (subscription->subscription_identifier > FLOWIE_MQTT_MAX_REMAINING_LENGTH) {
        rc = SALTS_EPROTO;
        goto fail;
      }
      ++option_index;
    } else if (type == 5u) {
      flowie_session_inflight_t inflight;
      if (value_size != 3u || value[2] != 2u ||
          vec_size(&owner->inflight) + vec_size(&owner->deliveries) >=
              config->max_inflight) {
        rc = SALTS_EPROTO;
        goto fail;
      }
      inflight.packet_id = flowie_session_record_read_u16(value);
      inflight.qos = value[2];
      inflight.state = FLOWIE_SESSION_INFLIGHT_QOS2_RELEASE;
      if (inflight.packet_id == 0u || flowie_session_inflight_find(owner, inflight.packet_id, NULL)) {
        rc = SALTS_EPROTO;
        goto fail;
      }
      rc = flowie_stl_error(vec_push(&owner->inflight, &inflight));
      if (rc != SALTS_OK) {
        goto fail;
      }
    } else if (type == 6u) {
      flowie_session_delivery_t delivery;
      int delivery_lookup_rc;
      if (value_size != (record_minor >= 3u ? 12u : 4u) ||
          vec_size(&owner->inflight) + vec_size(&owner->deliveries) >=
              config->max_inflight) {
        rc = SALTS_EPROTO;
        goto fail;
      }
      memset(&delivery, 0, sizeof(delivery));
      delivery.packet_id = flowie_session_record_read_u16(value);
      delivery.qos = value[2];
      delivery.state = (flowie_session_delivery_state_t)value[3];
      delivery.expiry_at_epoch_seconds =
          record_minor >= 3u ? flowie_session_record_read_u64(value + 4u) : 0u;
      delivery_lookup_rc =
          flowie_session_delivery_lookup(owner, delivery.packet_id, NULL, NULL);
      if (delivery.packet_id == 0u || delivery.qos == 0u || delivery.qos > 2u ||
          delivery.state < FLOWIE_SESSION_DELIVERY_WAIT_ACK ||
          delivery.state > FLOWIE_SESSION_DELIVERY_WAIT_PUBCOMP ||
          delivery_lookup_rc != SALTS_ENOENT ||
          flowie_session_inflight_find(owner, delivery.packet_id, NULL)) {
        rc = SALTS_EPROTO;
        goto fail;
      }
      rc = flowie_session_delivery_append(owner, &delivery);
      if (rc != SALTS_OK) {
        goto fail;
      }
    } else if (type == 7u) {
      flowie_session_delivery_t *delivery =
          (flowie_session_delivery_t *)vec_at(&owner->deliveries, packet_index);
      if (!delivery || delivery->packet || value_size == 0u) {
        rc = SALTS_EPROTO;
        goto fail;
      }
      delivery->packet = tstr_new_len(value, value_size);
      if (!delivery->packet) {
        rc = SALTS_ENOMEM;
        goto fail;
      }
      ++packet_index;
    } else if (type == 8u) {
      if (!metadata_seen || will_metadata_seen ||
          value_size != FLOWIE_SESSION_RECORD_WILL_METADATA_SIZE || value[0] > 1u ||
          value[1] > 2u || value[2] > 1u) {
        rc = SALTS_EPROTO;
        goto fail;
      }
      owner->will_pending = value[0];
      owner->will_qos = value[1];
      owner->will_retain = value[2];
      owner->will_delay_interval = flowie_session_record_read_u32(value + 3u);
      owner->has_will = 1u;
      will_metadata_seen = 1;
    } else if (type == 9u) {
      flowie_mqtt_span_t topic = {value, value_size};
      if (!will_metadata_seen || will_topic_seen || !flowie_mqtt_topic_name_validate(topic)) {
        rc = SALTS_EPROTO;
        goto fail;
      }
      owner->will_topic = tstr_new_len(value, value_size);
      if (!owner->will_topic) {
        rc = SALTS_ENOMEM;
        goto fail;
      }
      will_topic_seen = 1;
    } else if (type == 10u) {
      if (!will_topic_seen || will_properties_seen) {
        rc = SALTS_EPROTO;
        goto fail;
      }
      owner->will_properties = tstr_new_len(value, value_size);
      if (!owner->will_properties) {
        rc = SALTS_ENOMEM;
        goto fail;
      }
      will_properties_seen = 1;
    } else {
      if (!will_properties_seen || will_payload_seen) {
        rc = SALTS_EPROTO;
        goto fail;
      }
      owner->will_payload = tstr_new_len(value, value_size);
      if (!owner->will_payload) {
        rc = SALTS_ENOMEM;
        goto fail;
      }
      will_payload_seen = 1;
    }
    previous_type = type;
    offset += wire_size;
  }
  if (!header_seen || !metadata_seen || option_index != vec_size(&owner->subscriptions) ||
      packet_index != vec_size(&owner->deliveries) ||
      (record_minor == 0u && will_metadata_seen) ||
      (will_metadata_seen && (!will_topic_seen || !will_properties_seen || !will_payload_seen))) {
    rc = SALTS_EPROTO;
    goto fail;
  }
  for (size_t i = 0u; i < vec_size(&owner->deliveries); ++i) {
    const flowie_session_delivery_t *delivery =
        (const flowie_session_delivery_t *)vec_at_const(&owner->deliveries, i);
    rc = flowie_session_record_delivery_validate(owner, delivery);
    if (rc != SALTS_OK) goto fail;
  }
  owner->resource_generation = revision;
  owner->initialized = 1u;
  owner->active = 0u;
  owner->clean_start = 0u;
  *out = owner;
  return SALTS_OK;

fail:
  flowie_session_owner_destroy(owner);
  return rc;
}
