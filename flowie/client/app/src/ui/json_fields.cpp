#include "json_fields.h"
#include <algorithm>
#include <charconv>
#include <stdexcept>

namespace flowie::app::json_fields {
  void require_fields(const json_value_t *root, std::initializer_list<std::string_view> fields) {
    if (!root || json_type(root) != JSON_OBJECT)
      throw std::runtime_error("The selected file must contain a JSON object.");
    for (size_t i = 0; i < json_object_size(root); ++i) {
      const std::string_view key(json_object_key(root, i), json_object_key_len(root, i));
      if (std::find(fields.begin(), fields.end(), key) == fields.end())
        throw std::runtime_error("The selected file contains an unsupported field.");
    }
  }

  std::string read_string(const json_value_t *root, const char *key, const char *default_value) {
    const auto *item = json_object_get(root, key);
    if (!item && default_value) return default_value;
    if (!item || json_type(item) != JSON_STRING)
      throw std::runtime_error(std::string(key) + " must be a string.");
    std::string value(json_string(item), json_string_len(item));
    if (value.find('\0') != value.npos)
      throw std::runtime_error(std::string(key) + " must not contain NUL.");
    return value;
  }

  int read_integer(const json_value_t *root, const char *key, int fallback, int min, int max) {
    const auto *item = json_object_get(root, key);
    if (!item) return fallback;
    size_t length = 0;
    const char *token = json_type(item) == JSON_NUMBER ? json_number_text(item, &length) : nullptr;
    int value = 0;
    if (token) {
      const auto parsed = std::from_chars(token, token + length, value);
      if (parsed.ec == std::errc{} && parsed.ptr == token + length && value >= min && value <= max)
        return value;
    }
    throw std::runtime_error(std::string(key) + " must be an integer from " + std::to_string(min) +
                             " to " + std::to_string(max) + ".");
  }

  Json parse(std::string_view bytes) {
    Json root(json_parse(bytes.data(), bytes.size()), json_free);
    if (!root) throw std::runtime_error("Could not parse JSON.");
    return root;
  }
} // namespace flowie::app::json_fields
