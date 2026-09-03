#include "flowie_stl_error_internal.h"

#include <cstl.h>
#include <cstl.h>
#include <cstl.h>
#include <cstl.h>

#include "flowie_proxy_protocol_internal.h"

#include "salts_error.h"
#include <cstl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

static const uint8_t FLOWIE_PROXY_PROTOCOL_V2_SIGNATURE[12] = {
    0x0du, 0x0au, 0x0du, 0x0au, 0x00u, 0x0du, 0x0au, 0x51u, 0x55u, 0x49u, 0x54u, 0x0au};

enum {
  FLOWIE_PROXY_PROTOCOL_V2_VERSION = 2,
  FLOWIE_PROXY_PROTOCOL_V2_VERSION_SHIFT = 4,
  FLOWIE_PROXY_PROTOCOL_V2_COMMAND_MASK = 0x0f,
  FLOWIE_PROXY_PROTOCOL_V2_FAMILY_TCP4 = 0x11,
  FLOWIE_PROXY_PROTOCOL_V2_FAMILY_TCP6 = 0x21,
  FLOWIE_PROXY_PROTOCOL_V2_IPV4_ADDRESS_SIZE = 4,
  FLOWIE_PROXY_PROTOCOL_V2_IPV6_ADDRESS_SIZE = 16,
  FLOWIE_PROXY_PROTOCOL_V2_IPV4_BLOCK_SIZE = 12,
  FLOWIE_PROXY_PROTOCOL_V2_IPV6_BLOCK_SIZE = 36,
  FLOWIE_PROXY_PROTOCOL_V2_TLV_HEADER_SIZE = 3
};

enum { FLOWIE_PROXY_PROTOCOL_CIDR_TEXT_CAPACITY = INET6_ADDRSTRLEN + 4 };

static const uint8_t FLOWIE_PROXY_PROTOCOL_V1_PREFIX[] = {'P', 'R', 'O', 'X', 'Y', ' '};

typedef struct flowie_proxy_trusted_network_s {
  int family;
  uint8_t prefix_bits;
  uint8_t address[16];
} flowie_proxy_trusted_network_t;

struct flowie_proxy_protocol_policy_s {
  vec_t trusted_networks;
  size_t max_header_bytes;
  uint64_t header_timeout_ms;
  int trusted_networks_initialized;
};

static int flowie_proxy_protocol_cidr_parse(
    const char *cidr, flowie_proxy_trusted_network_t *out) {
  char address_text[FLOWIE_PROXY_PROTOCOL_CIDR_TEXT_CAPACITY];
  const char *slash;
  char *end = NULL;
  unsigned long prefix;
  size_t address_size;
  size_t text_size;
  unsigned int max_prefix;

  if (!cidr || !cidr[0] || !out) return SALTS_EINVAL;
  slash = strchr(cidr, '/');
  if (!slash || slash == cidr || strchr(slash + 1, '/') != NULL) return SALTS_EINVAL;
  text_size = (size_t)(slash - cidr);
  if (text_size >= sizeof(address_text) || slash[1] == '\0') return SALTS_EINVAL;
  memcpy(address_text, cidr, text_size);
  address_text[text_size] = '\0';

  memset(out, 0, sizeof(*out));
  if (inet_pton(AF_INET, address_text, out->address) == 1) {
    out->family = AF_INET;
    address_size = 4u;
    max_prefix = 32u;
  } else if (inet_pton(AF_INET6, address_text, out->address) == 1) {
    out->family = AF_INET6;
    address_size = 16u;
    max_prefix = 128u;
  } else {
    return SALTS_EINVAL;
  }

  prefix = strtoul(slash + 1, &end, 10);
  if (!end || *end != '\0' || prefix > max_prefix) return SALTS_EINVAL;
  out->prefix_bits = (uint8_t)prefix;
  for (size_t bit = (size_t)prefix; bit < address_size * 8u; ++bit) {
    if ((out->address[bit / 8u] & (uint8_t)(UINT8_C(0x80) >> (bit % 8u))) != 0u) {
      return SALTS_EINVAL;
    }
  }
  return SALTS_OK;
}

static int flowie_proxy_protocol_matches_prefix(const uint8_t *data, size_t data_size,
                                                const uint8_t *prefix, size_t prefix_size) {
  size_t compared = data_size < prefix_size ? data_size : prefix_size;
  return compared != 0u && memcmp(data, prefix, compared) == 0;
}

static uint16_t flowie_proxy_protocol_read_u16(const uint8_t *data) {
  return (uint16_t)(((uint16_t)data[0] << 8u) | data[1]);
}

typedef struct flowie_proxy_protocol_v1_token_s {
  const uint8_t *data;
  size_t size;
} flowie_proxy_protocol_v1_token_t;

static int flowie_proxy_protocol_v1_tokenize(
    const uint8_t *line, size_t line_size, flowie_proxy_protocol_v1_token_t tokens[6],
    size_t *token_count) {
  size_t count = 0u;
  size_t start = 0u;
  if (!line || !tokens || !token_count || line_size == 0u) return SALTS_EINVAL;
  for (size_t i = 0u; i <= line_size; ++i) {
    if (i != line_size && line[i] != ' ') continue;
    if (i == start || count >= 6u) return SALTS_EPROTO;
    tokens[count].data = line + start;
    tokens[count].size = i - start;
    ++count;
    start = i + 1u;
  }
  *token_count = count;
  return SALTS_OK;
}

static int flowie_proxy_protocol_v1_token_equal(
    const flowie_proxy_protocol_v1_token_t *token, const char *text) {
  size_t text_size = text ? strlen(text) : 0u;
  return token && text && token->size == text_size &&
         memcmp(token->data, text, text_size) == 0;
}

static int flowie_proxy_protocol_v1_address(
    const flowie_proxy_protocol_v1_token_t *token, int family,
    char output[FLOWIE_PROXY_PROTOCOL_ADDRESS_TEXT_SIZE]) {
  uint8_t binary[16];
  char text[FLOWIE_PROXY_PROTOCOL_ADDRESS_TEXT_SIZE];
  if (!token || !output || token->size == 0u || token->size >= sizeof(text)) return SALTS_EPROTO;
  memcpy(text, token->data, token->size);
  text[token->size] = '\0';
  if (inet_pton(family, text, binary) != 1 ||
      !inet_ntop(family, binary, output, FLOWIE_PROXY_PROTOCOL_ADDRESS_TEXT_SIZE))
    return SALTS_EPROTO;
  return SALTS_OK;
}

static int flowie_proxy_protocol_v1_port(const flowie_proxy_protocol_v1_token_t *token,
                                         uint16_t *out) {
  uint32_t value = 0u;
  if (!token || !out || token->size == 0u || token->size > 5u ||
      (token->size > 1u && token->data[0] == '0'))
    return SALTS_EPROTO;
  for (size_t i = 0u; i < token->size; ++i) {
    if (token->data[i] < '0' || token->data[i] > '9') return SALTS_EPROTO;
    value = value * 10u + (uint32_t)(token->data[i] - '0');
    if (value > UINT16_MAX) return SALTS_EPROTO;
  }
  *out = (uint16_t)value;
  return SALTS_OK;
}

int flowie_proxy_protocol_v1_parse(const void *data, size_t data_size, size_t max_header_size,
                                   flowie_proxy_protocol_v1_view_t *out, size_t *consumed) {
  flowie_proxy_protocol_v1_view_t decoded = FLOWIE_PROXY_PROTOCOL_V1_VIEW_INIT;
  flowie_proxy_protocol_v1_token_t tokens[6] = {{0}};
  const uint8_t *bytes = (const uint8_t *)data;
  size_t search_size;
  size_t header_size = 0u;
  size_t token_count = 0u;
  int family;
  int rc;
  if (consumed) *consumed = 0u;
  if (!bytes || !out || !consumed || out->size != sizeof(*out) ||
      out->abi_version != FLOWIE_PROXY_PROTOCOL_V2_ABI_V1 ||
      max_header_size < sizeof(FLOWIE_PROXY_PROTOCOL_V1_PREFIX))
    return SALTS_EINVAL;
  if (!flowie_proxy_protocol_matches_prefix(bytes, data_size,
                                            FLOWIE_PROXY_PROTOCOL_V1_PREFIX,
                                            sizeof(FLOWIE_PROXY_PROTOCOL_V1_PREFIX)))
    return SALTS_EPROTO;
  search_size = data_size < max_header_size ? data_size : max_header_size;
  if (search_size > FLOWIE_PROXY_PROTOCOL_V1_MAX_WIRE_SIZE)
    search_size = FLOWIE_PROXY_PROTOCOL_V1_MAX_WIRE_SIZE;
  for (size_t i = 0u; i < search_size; ++i) {
    if (bytes[i] == '\n') {
      if (i == 0u || bytes[i - 1u] != '\r') return SALTS_EPROTO;
      header_size = i + 1u;
      break;
    }
    if (bytes[i] == '\r' && i + 1u < search_size && bytes[i + 1u] != '\n')
      return SALTS_EPROTO;
  }
  if (header_size == 0u) {
    if (data_size >= max_header_size) return SALTS_EMSGSIZE;
    if (data_size >= FLOWIE_PROXY_PROTOCOL_V1_MAX_WIRE_SIZE) return SALTS_EPROTO;
    return FLOWIE_PROXY_PROTOCOL_INCOMPLETE;
  }
  if (header_size > max_header_size) return SALTS_EMSGSIZE;
  if (header_size >= sizeof("PROXY UNKNOWN\r\n") - 1u &&
      memcmp(bytes, "PROXY UNKNOWN", sizeof("PROXY UNKNOWN") - 1u) == 0 &&
      (bytes[sizeof("PROXY UNKNOWN") - 1u] == '\r' ||
       bytes[sizeof("PROXY UNKNOWN") - 1u] == ' ')) {
    decoded.header_size = header_size;
    *out = decoded;
    *consumed = header_size;
    return SALTS_OK;
  }
  rc = flowie_proxy_protocol_v1_tokenize(bytes, header_size - 2u, tokens, &token_count);
  if (rc != SALTS_OK) return rc;
  if (token_count != 6u || !flowie_proxy_protocol_v1_token_equal(&tokens[0], "PROXY"))
    return SALTS_EPROTO;
  if (flowie_proxy_protocol_v1_token_equal(&tokens[1], "TCP4")) {
    family = AF_INET;
    decoded.address_family = FLOWIE_PROXY_PROTOCOL_ADDRESS_IPV4;
  } else if (flowie_proxy_protocol_v1_token_equal(&tokens[1], "TCP6")) {
    family = AF_INET6;
    decoded.address_family = FLOWIE_PROXY_PROTOCOL_ADDRESS_IPV6;
  } else {
    return SALTS_EPROTO;
  }
  rc = flowie_proxy_protocol_v1_address(&tokens[2], family, decoded.source_address);
  if (rc == SALTS_OK)
    rc = flowie_proxy_protocol_v1_address(&tokens[3], family, decoded.destination_address);
  if (rc == SALTS_OK) rc = flowie_proxy_protocol_v1_port(&tokens[4], &decoded.source_port);
  if (rc == SALTS_OK) rc = flowie_proxy_protocol_v1_port(&tokens[5], &decoded.destination_port);
  if (rc != SALTS_OK) return rc;
  decoded.command = FLOWIE_PROXY_PROTOCOL_COMMAND_PROXY;
  decoded.header_size = header_size;
  *out = decoded;
  *consumed = header_size;
  return SALTS_OK;
}

static int flowie_proxy_protocol_tlv_decode(const uint8_t *data, size_t size, size_t offset,
                                            flowie_proxy_protocol_v2_tlv_t *out,
                                            size_t *next_offset) {
  flowie_proxy_protocol_v2_tlv_t decoded = FLOWIE_PROXY_PROTOCOL_V2_TLV_INIT;
  size_t value_size;
  if (!data || !out || !next_offset || offset > size) return SALTS_EINVAL;
  if (offset == size) return SALTS_ENOENT;
  if (size - offset < FLOWIE_PROXY_PROTOCOL_V2_TLV_HEADER_SIZE) return SALTS_EPROTO;
  value_size = flowie_proxy_protocol_read_u16(data + offset + 1u);
  if (value_size > size - offset - FLOWIE_PROXY_PROTOCOL_V2_TLV_HEADER_SIZE)
    return SALTS_EPROTO;
  decoded.type = data[offset];
  decoded.value = data + offset + FLOWIE_PROXY_PROTOCOL_V2_TLV_HEADER_SIZE;
  decoded.value_size = value_size;
  *out = decoded;
  *next_offset = offset + FLOWIE_PROXY_PROTOCOL_V2_TLV_HEADER_SIZE + value_size;
  return SALTS_OK;
}

static int flowie_proxy_protocol_tlvs_validate(const uint8_t *data, size_t size) {
  size_t offset = 0u;
  while (offset != size) {
    flowie_proxy_protocol_v2_tlv_t ignored = FLOWIE_PROXY_PROTOCOL_V2_TLV_INIT;
    int rc = flowie_proxy_protocol_tlv_decode(data, size, offset, &ignored, &offset);
    if (rc != SALTS_OK) return rc;
  }
  return SALTS_OK;
}

int flowie_proxy_protocol_v2_tlv_cursor_init(
    const flowie_proxy_protocol_v2_view_t *view,
    flowie_proxy_protocol_v2_tlv_cursor_t *cursor) {
  if (!view || view->size != sizeof(*view) ||
      view->abi_version != FLOWIE_PROXY_PROTOCOL_V2_ABI_V1 || !cursor ||
      view->command != FLOWIE_PROXY_PROTOCOL_COMMAND_PROXY ||
      (!view->tlvs && view->tlvs_size != 0u))
    return SALTS_EINVAL;
  cursor->data = view->tlvs;
  cursor->data_size = view->tlvs_size;
  cursor->offset = 0u;
  return SALTS_OK;
}

int flowie_proxy_protocol_v2_tlv_next(flowie_proxy_protocol_v2_tlv_cursor_t *cursor,
                                      flowie_proxy_protocol_v2_tlv_t *out) {
  size_t next_offset;
  int rc;
  if (!cursor || !out || cursor->offset > cursor->data_size ||
      (!cursor->data && cursor->data_size != 0u))
    return SALTS_EINVAL;
  rc = flowie_proxy_protocol_tlv_decode(cursor->data, cursor->data_size, cursor->offset, out,
                                        &next_offset);
  if (rc == SALTS_OK) cursor->offset = next_offset;
  return rc;
}

int flowie_proxy_protocol_v2_parse(const void *data, size_t data_size, size_t max_header_size,
                                   flowie_proxy_protocol_v2_view_t *out, size_t *consumed) {
  flowie_proxy_protocol_v2_view_t decoded = FLOWIE_PROXY_PROTOCOL_V2_VIEW_INIT;
  const uint8_t *bytes = (const uint8_t *)data;
  size_t prefix_size;
  size_t payload_size;
  size_t header_size;
  size_t address_block_size;
  uint8_t command;
  int rc;
  if (consumed) *consumed = 0u;
  if (!bytes || !out || !consumed || out->size != sizeof(*out) ||
      out->abi_version != FLOWIE_PROXY_PROTOCOL_V2_ABI_V1 ||
      max_header_size < FLOWIE_PROXY_PROTOCOL_V2_FIXED_SIZE ||
      max_header_size > FLOWIE_PROXY_PROTOCOL_V2_MAX_WIRE_SIZE)
    return SALTS_EINVAL;
  prefix_size = data_size < sizeof(FLOWIE_PROXY_PROTOCOL_V2_SIGNATURE)
                    ? data_size
                    : sizeof(FLOWIE_PROXY_PROTOCOL_V2_SIGNATURE);
  if (memcmp(bytes, FLOWIE_PROXY_PROTOCOL_V2_SIGNATURE, prefix_size) != 0) return SALTS_EPROTO;
  if (data_size >= 13u &&
      bytes[12] >> FLOWIE_PROXY_PROTOCOL_V2_VERSION_SHIFT !=
          FLOWIE_PROXY_PROTOCOL_V2_VERSION)
    return SALTS_EPROTO;
  if (data_size >= 13u) {
    command = bytes[12] & FLOWIE_PROXY_PROTOCOL_V2_COMMAND_MASK;
    if (command != FLOWIE_PROXY_PROTOCOL_COMMAND_LOCAL &&
        command != FLOWIE_PROXY_PROTOCOL_COMMAND_PROXY)
      return SALTS_EPROTO;
  }
  if (data_size < FLOWIE_PROXY_PROTOCOL_V2_FIXED_SIZE)
    return FLOWIE_PROXY_PROTOCOL_INCOMPLETE;
  payload_size = flowie_proxy_protocol_read_u16(bytes + 14u);
  header_size = FLOWIE_PROXY_PROTOCOL_V2_FIXED_SIZE + payload_size;
  if (header_size > max_header_size) return SALTS_EMSGSIZE;
  if (data_size < header_size) return FLOWIE_PROXY_PROTOCOL_INCOMPLETE;
  command = bytes[12] & FLOWIE_PROXY_PROTOCOL_V2_COMMAND_MASK;
  decoded.command = (flowie_proxy_protocol_command_t)command;
  decoded.header_size = header_size;
  if (command == FLOWIE_PROXY_PROTOCOL_COMMAND_LOCAL) {
    *out = decoded;
    *consumed = header_size;
    return SALTS_OK;
  }
  if (bytes[13] == FLOWIE_PROXY_PROTOCOL_V2_FAMILY_TCP4) {
    decoded.address_family = FLOWIE_PROXY_PROTOCOL_ADDRESS_IPV4;
    decoded.address_size = FLOWIE_PROXY_PROTOCOL_V2_IPV4_ADDRESS_SIZE;
    address_block_size = FLOWIE_PROXY_PROTOCOL_V2_IPV4_BLOCK_SIZE;
  } else if (bytes[13] == FLOWIE_PROXY_PROTOCOL_V2_FAMILY_TCP6) {
    decoded.address_family = FLOWIE_PROXY_PROTOCOL_ADDRESS_IPV6;
    decoded.address_size = FLOWIE_PROXY_PROTOCOL_V2_IPV6_ADDRESS_SIZE;
    address_block_size = FLOWIE_PROXY_PROTOCOL_V2_IPV6_BLOCK_SIZE;
  } else {
    return SALTS_EPROTO;
  }
  if (payload_size < address_block_size) return SALTS_EPROTO;
  decoded.source_address = bytes + FLOWIE_PROXY_PROTOCOL_V2_FIXED_SIZE;
  decoded.destination_address = decoded.source_address + decoded.address_size;
  decoded.source_port = flowie_proxy_protocol_read_u16(
      decoded.destination_address + decoded.address_size);
  decoded.destination_port = flowie_proxy_protocol_read_u16(
      decoded.destination_address + decoded.address_size + sizeof(uint16_t));
  decoded.tlvs = bytes + FLOWIE_PROXY_PROTOCOL_V2_FIXED_SIZE + address_block_size;
  decoded.tlvs_size = payload_size - address_block_size;
  rc = flowie_proxy_protocol_tlvs_validate(decoded.tlvs, decoded.tlvs_size);
  if (rc != SALTS_OK) return rc;
  *out = decoded;
  *consumed = header_size;
  return SALTS_OK;
}

int flowie_proxy_protocol_policy_create(
    const flowie_endpoint_proxy_binding_t *binding,
    flowie_proxy_protocol_policy_t **out) {
  flowie_proxy_protocol_policy_t *policy;
  int rc;

  if (out) *out = NULL;
  if (!binding || binding->size != sizeof(*binding) || !out ||
      !binding->trusted_peer_cidrs || binding->trusted_peer_count == 0u ||
      binding->trusted_peer_count > FLOWIE_ENDPOINT_PROXY_MAX_TRUSTED_PEERS ||
      binding->max_header_bytes < FLOWIE_PROXY_PROTOCOL_V2_FIXED_SIZE ||
      binding->max_header_bytes > FLOWIE_PROXY_PROTOCOL_V2_MAX_WIRE_SIZE ||
      binding->header_timeout_ms == 0u) {
    return SALTS_EINVAL;
  }

  policy = (flowie_proxy_protocol_policy_t *)calloc(1, sizeof(*policy));
  if (!policy) return SALTS_ENOMEM;
  rc = flowie_stl_error(vec_init_bytes(&policy->trusted_networks, sizeof(flowie_proxy_trusted_network_t), _Alignof(flowie_proxy_trusted_network_t), SIZE_MAX));
  if (rc != SALTS_OK) {
    free(policy);
    return rc;
  }
  policy->trusted_networks_initialized = 1;
  policy->max_header_bytes = binding->max_header_bytes;
  policy->header_timeout_ms = binding->header_timeout_ms;

  for (size_t i = 0u; i < binding->trusted_peer_count; ++i) {
    flowie_proxy_trusted_network_t network;
    rc = flowie_proxy_protocol_cidr_parse(binding->trusted_peer_cidrs[i], &network);
    if (rc == SALTS_OK) rc = flowie_stl_error(vec_push(&policy->trusted_networks, &network));
    if (rc != SALTS_OK) {
      flowie_proxy_protocol_policy_destroy(policy);
      return rc;
    }
  }
  *out = policy;
  return SALTS_OK;
}

void flowie_proxy_protocol_policy_destroy(flowie_proxy_protocol_policy_t *policy) {
  if (!policy) return;
  if (policy->trusted_networks_initialized)
    vec_destroy(&policy->trusted_networks);
  free(policy);
}
