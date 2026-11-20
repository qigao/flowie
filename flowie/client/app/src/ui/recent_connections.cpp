#include "recent_connections.h"
#include <algorithm>
#include <stdexcept>
#include <system_error>

namespace flowie::app {
  namespace {
    constexpr size_t kHistoryCharacters = kRecentConnectionLimit * (kBrokerHostLimit + 24) + 2;

    void check_registry(LSTATUS status, const char *operation) {
      if (status != ERROR_SUCCESS)
        throw std::system_error(static_cast<int>(status), std::system_category(), operation);
    }

    RecentConnection decode(std::wstring_view row) {
      const auto next = [&] {
        const auto delimiter = row.find(L'\t');
        if (delimiter == row.npos) throw std::runtime_error("Invalid recent connection record");
        const auto field = row.substr(0, delimiter);
        row.remove_prefix(delimiter + 1);
        return field;
      };
      const auto host = next();
      const auto port = read_port(next());
      const auto transport = next();
      const auto version = next();
      if (!valid_broker_host(host) || transport.size() != 1 || transport[0] < L'1' ||
          transport[0] > L'4' || (version != L"5" && version != L"4") || row.size() != 1 ||
          row[0] < L'0' || row[0] > L'2')
        throw std::runtime_error("Invalid recent connection record");
      return {static_cast<ClientRole>(row[0] - L'0'), std::wstring(host), port,
              static_cast<flowie_mqtt_client_transport_t>(transport[0] - L'0'),
              version == L"5" ? FLOWIE_MQTT_VERSION_5 : FLOWIE_MQTT_VERSION_3_1_1};
    }

    bool same_connection(const RecentConnection &a, const RecentConnection &b) {
      return a.role == b.role && a.port == b.port && a.transport == b.transport &&
             a.version == b.version &&
             CompareStringOrdinal(a.host.c_str(), -1, b.host.c_str(), -1, TRUE) == CSTR_EQUAL;
    }

    // Serialize the registry read/modify/write across Workbench instances in this desktop session.
    class HistoryLock {
    public:
      HistoryLock() {
        handle_ = CreateMutexW(nullptr, FALSE, L"Local\\FlowieMqttWorkbenchRecentConnections");
        if (!handle_) check_registry(GetLastError(), "Open recent connections lock");
        const DWORD result = WaitForSingleObject(handle_, 1000);
        if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) return;
        const auto error = result == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT;
        CloseHandle(handle_);
        check_registry(error, "Lock recent connections");
      }
      ~HistoryLock() {
        ReleaseMutex(handle_);
        CloseHandle(handle_);
      }
      HistoryLock(const HistoryLock &) = delete;
      HistoryLock &operator=(const HistoryLock &) = delete;

    private:
      HANDLE handle_;
    };
  } // namespace

  int read_port(std::wstring_view text) {
    int port = 0;
    for (const auto ch : text) {
      if (ch < L'0' || ch > L'9' || port > (65535 - (ch - L'0')) / 10)
        throw std::runtime_error("Port must be an integer from 1 to 65535");
      port = port * 10 + (ch - L'0');
    }
    if (port == 0) throw std::runtime_error("Port must be an integer from 1 to 65535");
    return port;
  }

  bool valid_broker_host(std::wstring_view host) {
    return !host.empty() && host.size() <= kBrokerHostLimit &&
           std::none_of(host.begin(), host.end(), [](wchar_t ch) {
             return ch <= L' ' || std::wstring_view(L"/\\?#@").find(ch) != std::wstring_view::npos;
           });
  }

  std::wstring connection_label(const RecentConnection &entry) {
    constexpr const wchar_t *transports[] = {L"TCP", L"TLS", L"WS", L"WSS"};
    constexpr const wchar_t *roles[] = {L"Client", L"Publisher", L"Subscriber"};
    auto host = entry.host;
    if (host.front() != L'[' && host.find(L':') != host.npos) host = L"[" + host + L"]";
    return std::wstring(roles[static_cast<int>(entry.role)]) + L"  " +
           transports[static_cast<int>(entry.transport) - 1] + L"  " + host + L":" +
           std::to_wstring(entry.port) +
           (entry.version == FLOWIE_MQTT_VERSION_5 ? L"  MQTT 5" : L"  MQTT 3.1.1");
  }

  std::vector<RecentConnection> RecentConnections::load() const {
    wchar_t raw[kHistoryCharacters]{};
    DWORD bytes = sizeof(raw);
    const auto status = RegGetValueW(HKEY_CURRENT_USER, key_.c_str(), kRecentConnectionValue,
                                     RRF_RT_REG_MULTI_SZ, nullptr, raw, &bytes);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) return {};
    check_registry(status, "Read recent connections");
    if (bytes < 2 * sizeof(wchar_t) || bytes % sizeof(wchar_t))
      throw std::runtime_error("Invalid recent connections data");
    const std::wstring_view data(raw, bytes / sizeof(wchar_t));
    std::vector<RecentConnection> entries;
    size_t offset = 0;
    while (offset < data.size()) {
      const auto end = data.find(L'\0', offset);
      if (end == offset && offset == data.size() - 1) return entries;
      if (end == data.npos || end == offset || entries.size() == kRecentConnectionLimit) break;
      entries.push_back(decode(data.substr(offset, end - offset)));
      offset = end + 1;
    }
    throw std::runtime_error("Invalid recent connections data");
  }

  void RecentConnections::remember(const RecentConnection &entry) const {
    const HistoryLock lock;
    auto entries = load();
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&](const auto &other) { return same_connection(entry, other); }),
                  entries.end());
    entries.insert(entries.begin(), entry);
    if (entries.size() > kRecentConnectionLimit) entries.pop_back();
    std::wstring data;
    for (const auto &item : entries) {
      data += item.host + L'\t' + std::to_wstring(item.port) + L'\t' +
              std::to_wstring(item.transport) + L'\t' +
              (item.version == FLOWIE_MQTT_VERSION_5 ? L'5' : L'4') + L'\t' +
              std::to_wstring(static_cast<int>(item.role));
      data += L'\0';
    }
    // One registry value keeps the ordered list consistent if the process exits during a write.
    HKEY key;
    check_registry(RegCreateKeyExW(HKEY_CURRENT_USER, key_.c_str(), 0, nullptr, 0, KEY_SET_VALUE,
                                   nullptr, &key, nullptr),
                   "Open recent connections");
    const auto result = RegSetValueExW(key, kRecentConnectionValue, 0, REG_MULTI_SZ,
                                       reinterpret_cast<const BYTE *>(data.c_str()),
                                       static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    check_registry(result, "Write recent connections");
  }
} // namespace flowie::app
