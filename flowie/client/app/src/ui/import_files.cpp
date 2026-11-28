#include "import_files.h"
#include "json_fields.h"
#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace flowie::app {
  using namespace json_fields;
  namespace {
    constexpr size_t kProfileFileLimit = 64 * 1024;
    constexpr size_t kSampleFileLimit = 2 * 1024 * 1024;

    std::string read_file(const std::filesystem::path &file, size_t limit) {
      std::ifstream input(file, std::ios::binary);
      if (!input) throw std::runtime_error("Could not open the selected file.");
      std::string bytes(limit + 1, '\0');
      input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      const auto size = static_cast<size_t>(input.gcount());
      if (size > limit)
        throw std::runtime_error("The selected file exceeds the import size limit.");
      if (input.bad() || !input.eof())
        throw std::runtime_error("Could not read the selected file.");
      bytes.resize(size);
      if (bytes.compare(0, 3, "\xef\xbb\xbf") == 0) bytes.erase(0, 3);
      if (bytes.find('\0') != bytes.npos)
        throw std::runtime_error("The selected file contains NUL bytes.");
      return bytes;
    }

    std::string tls_path(const json_value_t *root, const char *key,
                         const std::filesystem::path &file) {
      const auto value = read_string(root, key, "");
      if (value.empty()) return {};
      auto path = std::filesystem::u8path(value);
      if (path.is_absolute()) return value;
      if (path.has_root_path())
        throw std::runtime_error(
            "Use a fully qualified TLS path or a path relative to the connection file.");
      return (file.parent_path() / path).lexically_normal().u8string();
    }
  } // namespace

  std::wstring utf8_to_wide(std::string_view value) {
    if (value.empty()) return {};
    if (value.size() > INT_MAX) throw std::runtime_error("Text is too large to display.");
    const int size = static_cast<int>(value.size());
    const int count =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), size, nullptr, 0);
    if (!count) throw std::runtime_error("Invalid UTF-8.");
    std::wstring text(static_cast<size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), size, text.data(),
                            count) != count)
      throw std::runtime_error("Could not decode UTF-8.");
    return text;
  }

  ConnectionProfile load_connection_file(const std::filesystem::path &file) {
    const auto bytes = read_file(file, kProfileFileLimit);
    (void)utf8_to_wide(bytes);
    const auto root = parse(bytes);
    return read_profile(root.get(), file);
  }

  ConnectionProfile json_fields::read_profile(const json_value_t *root,
                                              const std::filesystem::path &file,
                                              const CollectionVariables *variables) {
    require_fields(root, {"role", "host", "port", "transport", "protocol", "path", "username",
                          "ca_file", "cert_file", "key_file"});
    ConnectionProfile profile;
    const auto role = read_string(root, "role", "client");
    if (role == "publisher") profile.role = ClientRole::publisher;
    else if (role == "subscriber") profile.role = ClientRole::subscriber;
    else if (role != "client")
      throw std::runtime_error("role must be client, publisher or subscriber.");
    auto &connection = profile.connection;
    connection.host = read_string(root, "host");
    if (!valid_broker_host(utf8_to_wide(expand_collection_text(connection.host, variables, 4096))))
      throw std::runtime_error("Invalid broker host.");
    connection.port = read_integer(root, "port", 1883, 1, 65535);
    const auto transport = read_string(root, "transport", "tcp");
    if (transport == "tls") connection.transport = FLOWIE_MQTT_CLIENT_TRANSPORT_TLS;
    else if (transport == "ws") connection.transport = FLOWIE_MQTT_CLIENT_TRANSPORT_WS;
    else if (transport == "wss") connection.transport = FLOWIE_MQTT_CLIENT_TRANSPORT_WSS;
    else if (transport != "tcp") throw std::runtime_error("transport must be tcp, tls, ws or wss.");
    const auto protocol = read_string(root, "protocol", "5");
    if (protocol == "3.1.1") connection.version = FLOWIE_MQTT_VERSION_3_1_1;
    else if (protocol != "5") throw std::runtime_error("protocol must be 5 or 3.1.1.");
    connection.path = read_string(root, "path", "/mqtt");
    connection.username = read_string(root, "username", "");
    connection.ca_file = tls_path(root, "ca_file", file);
    connection.cert_file = tls_path(root, "cert_file", file);
    connection.key_file = tls_path(root, "key_file", file);
    for (auto value : {&connection.path, &connection.username, &connection.ca_file,
                       &connection.cert_file, &connection.key_file}) {
      if (utf8_to_wide(*value).size() > 4096)
        throw std::runtime_error("A connection field exceeds 4096 characters.");
    }
    const bool websocket = transport == "ws" || transport == "wss";
    const bool tls = transport == "tls" || transport == "wss";
    const auto resolved_path = expand_collection_text(connection.path, variables, 4096);
    if (websocket && (resolved_path.empty() || resolved_path.front() != '/'))
      throw std::runtime_error("WebSocket path must start with /.");
    if (!tls && (!connection.ca_file.empty() || !connection.cert_file.empty() ||
                 !connection.key_file.empty()))
      throw std::runtime_error("TLS files require tls or wss transport.");
    if (connection.cert_file.empty() != connection.key_file.empty())
      throw std::runtime_error("Supply both cert_file and key_file, or omit both.");
    return profile;
  }

  std::string load_payload_sample(const std::filesystem::path &file, size_t limit) {
    auto payload = read_file(file, limit + 3);
    if (payload.size() > limit) throw std::runtime_error("Sample exceeds the payload limit.");
    (void)utf8_to_wide(payload);
    return payload;
  }

  MessageSample load_sample_file(const std::filesystem::path &file) {
    const auto name = file.filename().wstring();
    constexpr std::wstring_view sample_suffix = L".mqtt-sample.json";
    const bool full_sample =
        name.size() >= sample_suffix.size() &&
        CompareStringOrdinal(name.data() + name.size() - sample_suffix.size(),
                             static_cast<int>(sample_suffix.size()), sample_suffix.data(),
                             static_cast<int>(sample_suffix.size()), TRUE) == CSTR_EQUAL;
    const auto extension = file.extension().wstring();
    const bool json = CompareStringOrdinal(extension.c_str(), -1, L".json", -1, TRUE) == CSTR_EQUAL;
    if (!json && CompareStringOrdinal(extension.c_str(), -1, L".txt", -1, TRUE) != CSTR_EQUAL)
      throw std::runtime_error("Choose a .txt, .json or .mqtt-sample.json file.");
    auto bytes = read_file(file, full_sample ? kSampleFileLimit : kPayloadBytes + 3);
    (void)utf8_to_wide(bytes);
    MessageSample sample;
    if (full_sample) {
      const auto document = parse(bytes);
      const auto *root = document.get();
      require_fields(root, {"topic", "qos", "retain", "format", "payload"});
      MessageOptions options;
      options.topic = read_string(root, "topic");
      if (!valid_topic(options.topic, false) || utf8_to_wide(options.topic).size() > 4096)
        throw std::runtime_error("Invalid publish topic.");
      options.qos = read_integer(root, "qos", 0, 0, 2);
      if (const auto *retain = json_object_get(root, "retain")) {
        if (json_type(retain) != JSON_BOOL) throw std::runtime_error("retain must be a boolean.");
        options.retain = json_bool(retain);
      }
      const auto format = read_string(root, "format", "text");
      if (format != "text" && format != "json")
        throw std::runtime_error("format must be text or json.");
      sample.json = format == "json";
      sample.payload = read_string(root, "payload");
      (void)utf8_to_wide(sample.payload);
      sample.options = std::move(options);
    } else {
      sample.json = json;
      sample.payload = std::move(bytes);
    }
    if (sample.payload.size() > kPayloadBytes)
      throw std::runtime_error("Payload exceeds 960 KiB limit.");
    if (sample.json) {
      std::string error;
      if (!format_json(sample.payload, nullptr, error)) throw std::runtime_error(error);
    }
    return sample;
  }
} // namespace flowie::app
