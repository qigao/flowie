#include "collections.h"
#include "json_fields.h"
#include <algorithm>
#include <iterator>
#include <shlobj.h>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace flowie::app {
  namespace {
    using namespace json_fields;
    constexpr size_t kLibraryBytes = 8 * 1024 * 1024;

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

    bool same_name(const SavedRequest &left, const SavedRequest &right) {
      return left.collection == right.collection && left.name == right.name;
    }

    bool valid_saved_filter(const std::string &filter) {
      return valid_topic(filter, true) && utf8_to_wide(filter).size() <= 4096 &&
             (filter.front() != '$' || filter.rfind("$SYS/", 0) == 0);
    }

    SavedRequest decode(const json_value_t *value, const std::filesystem::path &file) {
      require_fields(value, {"collection", "name", "connection", "topics", "message", "tool"});
      SavedRequest request;
      request.collection = read_string(value, "collection");
      request.name = read_string(value, "name");
      validate_saved_names(request);
      if (const auto *tool = json_object_get(value, "tool")) {
        require_fields(value, {"collection", "name", "tool"});
        require_fields(tool, {"kind", "endpoint", "method", "body", "hex", "baud", "bits", "parity",
                              "stop_bits", "flow"});
        ToolDraft draft;
        const int kind = read_integer(tool, "kind", -1, 0, 3);
        if (kind < 0) throw std::runtime_error("A saved tool requires its protocol kind.");
        draft.kind = static_cast<ToolKind>(kind);
        draft.endpoint = read_string(tool, "endpoint");
        draft.method = read_string(tool, "method");
        draft.body = read_string(tool, "body");
        if (draft.endpoint.size() > 4096 || draft.method.size() > 128 ||
            draft.body.size() > kToolBytes)
          throw std::runtime_error("Saved request fields exceed their size limits.");
        (void)utf8_to_wide(draft.endpoint);
        (void)utf8_to_wide(draft.method);
        (void)utf8_to_wide(draft.body);
        const auto *hex = json_object_get(tool, "hex");
        if (!hex || json_type(hex) != JSON_BOOL) throw std::runtime_error("hex must be a boolean.");
        draft.hex = json_bool(hex);
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
      if (request.tool) {
        const auto &draft = *request.tool;
        auto *tool = json_create_object();
        add(value.get(), "tool", tool);
        add(tool, "kind", json_create_int64(static_cast<int>(draft.kind)));
        string(tool, "endpoint", draft.endpoint);
        string(tool, "method", draft.method);
        string(tool, "body", draft.body);
        add(tool, "hex", json_create_bool(draft.hex));
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

    void write_library(const std::filesystem::path &file, const char *bytes, DWORD size) {
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
  } // namespace

  std::filesystem::path default_collections_file() {
    PWSTR folder = nullptr;
    const HRESULT status = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder);
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owner(folder, CoTaskMemFree);
    if (FAILED(status)) throw std::runtime_error("Could not locate Local AppData for Collections.");
    return std::filesystem::path(folder) / L"Flowie" / L"MQTT Workbench" / L"collections.json";
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
    File input(CreateFileW(file_.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (input.handle == INVALID_HANDLE_VALUE) {
      const auto error = GetLastError();
      if (!require_existing && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND))
        return {};
      windows_error(error, "Open Collections");
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(input.handle, &size)) windows_error(GetLastError(), "Size Collections");
    if (size.QuadPart <= 0 || static_cast<unsigned long long>(size.QuadPart) > kLibraryBytes)
      throw std::runtime_error("Invalid Collections file size (8 MiB maximum).");
    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    DWORD received = 0;
    if (!ReadFile(input.handle, bytes.data(), static_cast<DWORD>(bytes.size()), &received, nullptr))
      windows_error(GetLastError(), "Read Collections");
    if (received != bytes.size()) throw std::runtime_error("Incomplete Collections read.");
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
    std::filesystem::create_directories(file_.parent_path());
    auto lock_name = file_;
    lock_name += L".lock";
    const File lock(CreateFileW(lock_name.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
    if (lock.handle == INVALID_HANDLE_VALUE) windows_error(GetLastError(), "Lock Collections");
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
    write_library(file_, data.get(), static_cast<DWORD>(size));
    return requests;
  }
} // namespace flowie::app
