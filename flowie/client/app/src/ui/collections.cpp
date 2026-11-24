#include "collections.h"
#include "json_fields.h"
#include <core/base64.h>
#include <ini_parser.h>
#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <shlobj.h>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace flowie::app {
  namespace {
    using namespace json_fields;
    constexpr size_t kLibraryBytes = 8 * 1024 * 1024;
    // Allow one encoded Windows long path per recent file plus the last directory.
    constexpr size_t kSettingsBytes = (kRecentCollectionLimit + 1) * 160 * 1024;
    constexpr char kDirectorySection[] = "collections";
    constexpr char kDirectoryKey[] = "last_directory_base64";
    constexpr char kRecentSection[] = "recent_collections";
    using Ini = std::unique_ptr<ini_t, decltype(&ini_free)>;

    [[noreturn]] void windows_error(DWORD code, const char *operation) {
      throw std::system_error(static_cast<int>(code), std::system_category(), operation);
    }

    struct File {
      HANDLE handle;
      ~File() {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
      }
      File(const File &) = delete;
      File &operator=(const File &) = delete;
      explicit File(HANDLE file) : handle(file) {}
      void close() {
        const HANDLE file = std::exchange(handle, INVALID_HANDLE_VALUE);
        if (!CloseHandle(file)) windows_error(GetLastError(), "Close Collections file");
      }
    };

    File lock_collection_update(const std::filesystem::path &file) {
      std::filesystem::create_directories(file.parent_path());
      auto name = file;
      name += L".lock";
      const auto handle = CreateFileW(name.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, OPEN_ALWAYS,
                                     FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                                     nullptr);
      if (handle == INVALID_HANDLE_VALUE) windows_error(GetLastError(), "Lock Collections");
      return File(handle);
    }

    bool same_name(const SavedRequest &left, const SavedRequest &right) {
      return left.collection == right.collection && left.name == right.name;
    }

    bool valid_saved_filter(const std::string &filter) {
      return valid_topic(filter, true) && utf8_to_wide(filter).size() <= 4096 &&
             (filter.front() != '$' || filter.rfind("$SYS/", 0) == 0);
    }

    bool optional_boolean(const json_value_t *object, const char *key) {
      const auto *value = json_object_get(object, key);
      if (!value) return false;
      if (json_type(value) != JSON_BOOL)
        throw std::runtime_error(std::string(key) + " must be a boolean.");
      return json_bool(value);
    }

    SavedRequest decode(const json_value_t *value, const std::filesystem::path &file) {
      require_fields(value, {"collection", "name", "notes", "connection", "topics", "message",
                             "tool"});
      SavedRequest request;
      request.collection = read_string(value, "collection");
      request.name = read_string(value, "name");
      request.notes = read_string(value, "notes", "");
      if (utf8_to_wide(request.notes).size() > kSavedNotesLimit)
        throw std::runtime_error("Request notes exceed 2048 characters.");
      validate_saved_names(request);
      if (const auto *tool = json_object_get(value, "tool")) {
        require_fields(value, {"collection", "name", "notes", "tool"});
        require_fields(tool, {"kind", "endpoint", "method", "body", "hex", "baud", "bits", "parity",
                              "stop_bits", "flow", "json_body", "use_cookies", "headers"});
        ToolDraft draft;
        const int kind = read_integer(tool, "kind", -1, 0, 3);
        if (kind < 0) throw std::runtime_error("A saved tool requires its protocol kind.");
        draft.kind = static_cast<ToolKind>(kind);
        draft.endpoint = read_string(tool, "endpoint");
        draft.method = read_string(tool, "method");
        draft.body = read_string(tool, "body");
        draft.headers = read_string(tool, "headers", "");
        if (draft.endpoint.size() > 4096 || draft.method.size() > 128 ||
            draft.body.size() > kToolBytes)
          throw std::runtime_error("Saved request fields exceed their size limits.");
        (void)utf8_to_wide(draft.endpoint);
        (void)utf8_to_wide(draft.method);
        (void)utf8_to_wide(draft.body);
        (void)utf8_to_wide(draft.headers);
        if (!draft.headers.empty()) {
          if (!request_tool(draft.kind))
            throw std::runtime_error("Saved headers require an HTTP or JSON-RPC request.");
          draft.headers = collection_request_headers(draft.headers, false);
        }
        const auto *hex = json_object_get(tool, "hex");
        if (!hex || json_type(hex) != JSON_BOOL) throw std::runtime_error("hex must be a boolean.");
        draft.hex = json_bool(hex);
        draft.json_body = optional_boolean(tool, "json_body");
        draft.use_cookies = optional_boolean(tool, "use_cookies");
        if (draft.kind != ToolKind::http && (draft.json_body || draft.use_cookies))
          throw std::runtime_error("JSON body and cookie options apply to HTTP requests.");
        draft.baud = read_integer(tool, "baud", 115200, 1, 4000000);
        draft.bits = read_integer(tool, "bits", 8, 5, 8);
        draft.parity = read_integer(tool, "parity", 0, 0, 4);
        draft.stop_bits = read_integer(tool, "stop_bits", 1, 1, 2);
        draft.flow = read_integer(tool, "flow", 0, 0, 3);
        request.tool = std::move(draft);
        return request;
      }
      const auto *profile = json_object_get(value, "connection");
      require_fields(profile, {"role", "host", "port", "transport", "protocol", "path", "ca_file",
                               "cert_file", "key_file"});
      request.profile = read_profile(profile, file);
      if (request.profile.connection.path.find_first_of("?#") != std::string::npos)
        throw std::runtime_error("Saved WebSocket paths cannot contain queries or fragments.");
      const auto *topics = json_object_get(value, "topics");
      if (!topics || json_type(topics) != JSON_ARRAY || json_array_size(topics) > kSavedTopicLimit)
        throw std::runtime_error("Invalid saved subscription list.");
      for (size_t i = 0; i < json_array_size(topics); ++i) {
        const auto *topic = json_array_get(topics, i);
        require_fields(topic, {"filter", "qos"});
        auto filter = read_string(topic, "filter");
        if (!valid_saved_filter(filter))
          throw std::runtime_error("Invalid saved subscription filter.");
        if (std::any_of(request.topics.begin(), request.topics.end(),
                        [&](const auto &entry) { return entry.filter == filter; }))
          throw std::runtime_error("Duplicate saved subscription filter.");
        request.topics.push_back({std::move(filter), read_integer(topic, "qos", 0, 0, 2)});
      }
      if (request.profile.role == ClientRole::publisher && !request.topics.empty())
        throw std::runtime_error("Publisher requests cannot contain subscriptions.");
      if (const auto *message = json_object_get(value, "message")) {
        if (request.profile.role == ClientRole::subscriber)
          throw std::runtime_error("Subscriber requests cannot contain a publish draft.");
        require_fields(message, {"topic", "qos", "retain", "format", "payload"});
        auto topic = read_string(message, "topic");
        auto payload = read_string(message, "payload");
        const auto format = read_string(message, "format");
        if (utf8_to_wide(topic).size() > 4096 || payload.size() > kPayloadBytes ||
            (format != "text" && format != "json"))
          throw std::runtime_error("Invalid saved publish draft.");
        (void)utf8_to_wide(payload);
        const auto *retain = json_object_get(message, "retain");
        if (!retain || json_type(retain) != JSON_BOOL)
          throw std::runtime_error("retain must be boolean.");
        request.message =
            MessageSample{std::move(payload), format == "json",
                          MessageOptions{std::move(topic), read_integer(message, "qos", 0, 0, 2),
                                         json_bool(retain)}};
      }
      return request;
    }

    void add(json_value_t *parent, const char *key, json_value_t *value) {
      Json owned(value, json_free);
      if (!owned || !(key ? json_object_add_checked(parent, key, value)
                          : json_array_add_checked(parent, value)))
        throw std::runtime_error("Could not encode a saved request.");
      owned.release();
    }

    void string(json_value_t *parent, const char *key, std::string_view value) {
      add(parent, key, json_create_string_n(value.data(), value.size()));
    }

    Json encode(const SavedRequest &request) {
      Json value(json_create_object(), json_free);
      if (!value) throw std::bad_alloc();
      string(value.get(), "collection", request.collection);
      string(value.get(), "name", request.name);
      if (!request.notes.empty()) string(value.get(), "notes", request.notes);
      if (request.tool) {
        const auto &draft = *request.tool;
        auto *tool = json_create_object();
        add(value.get(), "tool", tool);
        add(tool, "kind", json_create_int64(static_cast<int>(draft.kind)));
        string(tool, "endpoint", draft.endpoint);
        string(tool, "method", draft.method);
        string(tool, "body", draft.body);
        if (!draft.headers.empty()) string(tool, "headers", draft.headers);
        add(tool, "hex", json_create_bool(draft.hex));
        if (draft.json_body) add(tool, "json_body", json_create_bool(true));
        if (draft.use_cookies) add(tool, "use_cookies", json_create_bool(true));
        add(tool, "baud", json_create_int64(draft.baud));
        add(tool, "bits", json_create_int64(draft.bits));
        add(tool, "parity", json_create_int64(draft.parity));
        add(tool, "stop_bits", json_create_int64(draft.stop_bits));
        add(tool, "flow", json_create_int64(draft.flow));
        return value;
      }
      auto *profile = json_create_object();
      add(value.get(), "connection", profile);
      constexpr const char *roles[] = {"client", "publisher", "subscriber"};
      constexpr const char *transports[] = {"tcp", "tls", "ws", "wss"};
      const auto role = static_cast<unsigned>(request.profile.role);
      const auto &connection = request.profile.connection;
      const auto transport = static_cast<unsigned>(connection.transport) - 1;
      if (role >= std::size(roles) || transport >= std::size(transports))
        throw std::runtime_error("Invalid connection role or transport.");
      string(profile, "role", roles[role]);
      string(profile, "transport", transports[transport]);
      string(profile, "protocol", connection.version == FLOWIE_MQTT_VERSION_5 ? "5" : "3.1.1");
      string(profile, "host", connection.host);
      add(profile, "port", json_create_int64(connection.port));
      string(profile, "path", connection.path);
      string(profile, "ca_file", connection.ca_file);
      string(profile, "cert_file", connection.cert_file);
      string(profile, "key_file", connection.key_file);
      auto *topics = json_create_array();
      add(value.get(), "topics", topics);
      for (const auto &topic : request.topics) {
        auto *stored = json_create_object();
        add(topics, nullptr, stored);
        string(stored, "filter", topic.filter);
        add(stored, "qos", json_create_int64(topic.qos));
      }
      if (request.message) {
        auto *message = json_create_object();
        add(value.get(), "message", message);
        const auto &options = request.message->options.value();
        string(message, "topic", options.topic);
        add(message, "qos", json_create_int64(options.qos));
        add(message, "retain", json_create_bool(options.retain));
        string(message, "format", request.message->json ? "json" : "text");
        string(message, "payload", request.message->payload);
      }
      return value;
    }

    std::string read_collections_file(const std::filesystem::path &file, size_t limit,
                                      bool require_existing, const char *size_error) {
      File input(CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
      if (input.handle == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        if (!require_existing && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND))
          return {};
        windows_error(error, "Open Collections file");
      }
      LARGE_INTEGER size{};
      if (!GetFileSizeEx(input.handle, &size)) windows_error(GetLastError(), "Size Collections file");
      if (size.QuadPart <= 0 || static_cast<unsigned long long>(size.QuadPart) > limit)
        throw std::runtime_error(size_error);
      std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
      DWORD received = 0;
      if (!ReadFile(input.handle, bytes.data(), static_cast<DWORD>(bytes.size()), &received, nullptr))
        windows_error(GetLastError(), "Read Collections file");
      if (received != bytes.size()) throw std::runtime_error("Incomplete Collections read.");
      return bytes;
    }

    void write_collections_file(const std::filesystem::path &file, const char *bytes, DWORD size) {
      GUID id{};
      wchar_t identifier[40]{};
      if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, identifier, 40))
        throw std::runtime_error("Could not name the Collections temporary file.");
      const auto path = file.parent_path() / (std::wstring(identifier) + L".tmp");
      struct Temporary {
        const std::filesystem::path &path;
        bool owned = false;
        ~Temporary() {
          if (owned) DeleteFileW(path.c_str());
        }
      } temporary{path};
      File output(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr));
      if (output.handle == INVALID_HANDLE_VALUE)
        windows_error(GetLastError(), "Create Collections temporary file");
      temporary.owned = true;
      DWORD written = 0;
      if (!WriteFile(output.handle, bytes, size, &written, nullptr))
        windows_error(GetLastError(), "Write Collections");
      if (written != size) throw std::runtime_error("Incomplete Collections write.");
      if (!FlushFileBuffers(output.handle)) windows_error(GetLastError(), "Flush Collections");
      output.close();
      if (!MoveFileExW(path.c_str(), file.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        windows_error(GetLastError(), "Commit Collections");
      temporary.owned = false;
    }

    Ini read_settings(const std::filesystem::path &settings) {
      const auto bytes = read_collections_file(settings, kSettingsBytes, false,
                                                "Invalid Collections history file size.");
      if (bytes.empty()) return Ini(nullptr, ini_free);
      Ini ini(ini_parse(bytes.data(), bytes.size()), ini_free);
      if (!ini) throw std::runtime_error("Could not parse Collections history.");
      return ini;
    }

    std::filesystem::path read_settings_path(ini_t *ini, const char *section, const char *key) {
      const auto *encoded = ini_get(ini, section, key);
      if (!encoded) return {};
      const auto decoded = tn_base64_decode_ex(encoded);
      if (!decoded.ok) throw std::runtime_error("Could not decode a Collections history path.");
      const std::unique_ptr<uint8_t, decltype(&std::free)> data(decoded.value.data, std::free);
      if (!decoded.value.len) throw std::runtime_error("A Collections history path is empty.");
      const std::string_view utf8(reinterpret_cast<const char *>(data.get()), decoded.value.len);
      if (utf8.find('\0') != utf8.npos)
        throw std::runtime_error("Invalid Collections history path.");
      std::filesystem::path path(utf8_to_wide(utf8));
      if (!path.is_absolute()) throw std::runtime_error("Invalid Collections history path.");
      return path;
    }

    std::string encode_settings_path(const std::filesystem::path &path) {
      // The INI parser treats semicolons and hashes as comments, even inside values.
      const auto utf8 = path.u8string();
      const auto encoded =
          tn_base64_encode_ex(reinterpret_cast<const uint8_t *>(utf8.data()), utf8.size());
      if (!encoded.ok) throw std::runtime_error("Could not encode a Collections history path.");
      const std::unique_ptr<char, decltype(&std::free)> data(encoded.value, std::free);
      return data.get();
    }
  } // namespace

  std::filesystem::path default_collections_file() {
    PWSTR folder = nullptr;
    const HRESULT status = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder);
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owner(folder, CoTaskMemFree);
    if (FAILED(status)) throw std::runtime_error("Could not locate Local AppData for Collections.");
    return std::filesystem::path(folder) / L"Flowie" / L"MQTT Workbench" / L"collections.json";
  }

  bool same_collection_file(const std::filesystem::path &first,
                            const std::filesystem::path &second) {
    return CompareStringOrdinal(first.c_str(), -1, second.c_str(), -1, TRUE) == CSTR_EQUAL;
  }

  std::filesystem::path read_collections_directory(const std::filesystem::path &settings) {
    const auto ini = read_settings(settings);
    return read_settings_path(ini.get(), kDirectorySection, kDirectoryKey);
  }

  std::vector<std::filesystem::path> read_recent_collections(const std::filesystem::path &settings) {
    const auto ini = read_settings(settings);
    std::vector<std::filesystem::path> recent;
    for (size_t i = 0; i < kRecentCollectionLimit; ++i) {
      auto path = read_settings_path(ini.get(), kRecentSection, ("file_" + std::to_string(i)).c_str());
      if (path.empty()) break;
      recent.push_back(std::move(path));
    }
    return recent;
  }

  void remember_collection_file(const std::filesystem::path &settings,
                                const std::filesystem::path &file) {
    const auto lock = lock_collection_update(settings);
    auto recent = read_recent_collections(settings);
    recent.erase(std::remove_if(recent.begin(), recent.end(),
                                [&](const auto &entry) { return same_collection_file(entry, file); }),
                 recent.end());
    if (recent.size() == kRecentCollectionLimit) recent.pop_back();
    recent.insert(recent.begin(), file);
    std::string bytes = std::string("[") + kDirectorySection + "]\n" + kDirectoryKey + "=" +
                        encode_settings_path(file.parent_path()) + "\n\n[" + kRecentSection + "]\n";
    for (size_t i = 0; i < recent.size(); ++i)
      bytes += "file_" + std::to_string(i) + "=" + encode_settings_path(recent[i]) + "\n";
    if (bytes.size() > kSettingsBytes)
      throw std::runtime_error("Collections history exceeds its size limit.");
    write_collections_file(settings, bytes.data(), static_cast<DWORD>(bytes.size()));
  }

  void validate_saved_names(const SavedRequest &request) {
    for (const auto &name :
         {std::string_view(request.collection), std::string_view(request.name)}) {
      const auto text = utf8_to_wide(name);
      if (text.empty() || text.size() > kSavedNameLimit ||
          text.find_first_not_of(L' ') == text.npos ||
          std::any_of(text.begin(), text.end(), [](wchar_t ch) { return ch < L' '; }))
        throw std::runtime_error("Enter a collection and request name of at most 64 characters.");
    }
  }

  std::vector<SavedRequest> CollectionStore::load(bool require_existing) const {
    using namespace json_fields;
    const auto bytes = read_collections_file(file_, kLibraryBytes, require_existing,
                                              "Invalid Collections file size (8 MiB maximum).");
    if (bytes.empty()) return {};
    const auto root = parse(bytes);
    if (json_type(root.get()) != JSON_ARRAY || json_array_size(root.get()) > kSavedRequestLimit)
      throw std::runtime_error("Collections must contain at most 64 requests.");
    std::vector<SavedRequest> requests;
    for (size_t i = 0; i < json_array_size(root.get()); ++i) {
      auto request = decode(json_array_get(root.get(), i), file_);
      if (std::any_of(requests.begin(), requests.end(),
                      [&](const auto &entry) { return same_name(entry, request); }))
        throw std::runtime_error("Duplicate request name in a collection.");
      requests.push_back(std::move(request));
    }
    return requests;
  }

  std::optional<std::vector<SavedRequest>>
  CollectionStore::save(const SavedRequest &request, bool replace, bool require_existing) const {
    using namespace json_fields;
    validate_saved_names(request);
    if (request.topics.size() > kSavedTopicLimit ||
        (request.message && request.message->payload.size() > kPayloadBytes))
      throw std::runtime_error("The saved request exceeds its topic or payload limit.");
    auto value = encode(request);
    // Validate the exact persisted representation before replacing an existing file.
    auto stored = decode(value.get(), file_);
    const auto lock = lock_collection_update(file_);
    auto requests = load(require_existing);
    auto found = std::find_if(requests.begin(), requests.end(),
                              [&](const auto &entry) { return same_name(entry, stored); });
    if (found != requests.end()) {
      if (!replace) return std::nullopt;
      *found = std::move(stored);
    } else {
      if (requests.size() == kSavedRequestLimit)
        throw std::runtime_error("Collections already contain 64 requests.");
      requests.push_back(std::move(stored));
    }
    Json root(json_create_array(), json_free);
    if (!root) throw std::bad_alloc();
    for (const auto &entry : requests)
      add(root.get(), nullptr, encode(entry).release());
    size_t size = 0;
    std::unique_ptr<char, decltype(&json_serialize_free)> data(json_serialize(root.get(), &size),
                                                               json_serialize_free);
    if (!data) throw std::runtime_error("Could not serialize Collections.");
    if (size > kLibraryBytes) throw std::runtime_error("Collections exceed the 8 MiB limit.");
    write_collections_file(file_, data.get(), static_cast<DWORD>(size));
    return requests;
  }
} // namespace flowie::app
