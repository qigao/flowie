#pragma once
#include "import_files.h"
#include <initializer_list>
#include <json_parser.h>
#include <memory>

namespace flowie::app::json_fields {
  using Json = std::unique_ptr<json_value_t, decltype(&json_free)>;
  Json parse(std::string_view bytes);
  void require_fields(const json_value_t *root, std::initializer_list<std::string_view> fields);
  std::string read_string(const json_value_t *root, const char *key,
                          const char *default_value = nullptr);
  int read_integer(const json_value_t *root, const char *key, int fallback, int min, int max);
  ConnectionProfile read_profile(const json_value_t *root, const std::filesystem::path &file);
} // namespace flowie::app::json_fields
