#pragma once
#include "recent_connections.h"
#include <filesystem>

namespace flowie::app {
  struct ConnectionProfile {
    ClientRole role = ClientRole::both;
    Connection connection;
  };

  struct MessageOptions {
    std::string topic;
    int qos = 0;
    bool retain = false;
  };

  struct MessageSample {
    std::string payload;
    bool json = false;
    std::optional<MessageOptions> options;
  };

  ConnectionProfile load_connection_file(const std::filesystem::path &file);
  MessageSample load_sample_file(const std::filesystem::path &file);
  std::string load_payload_sample(const std::filesystem::path &file, size_t limit);
  std::wstring utf8_to_wide(std::string_view value);
  std::optional<std::filesystem::path> choose_import_file(HWND owner, bool connection);
  std::optional<std::filesystem::path> choose_collection_file(HWND owner);
} // namespace flowie::app
