#pragma once
#include "workbench.h"
#include <string_view>
#include <windows.h>

namespace flowie::app {
  enum class ClientRole { both, publisher, subscriber };
  struct RecentConnection {
    ClientRole role;
    std::wstring host;
    int port;
    flowie_mqtt_client_transport_t transport;
    flowie_mqtt_version_t version;
  };
  constexpr size_t kRecentConnectionLimit = 12;
  constexpr size_t kBrokerHostLimit = 255;
  constexpr auto kRecentConnectionKey = L"Software\\Flowie\\MQTT Workbench";
  constexpr auto kRecentConnectionValue = L"Recent Connections";
  int read_port(std::wstring_view text);
  bool valid_broker_host(std::wstring_view host);
  std::wstring connection_label(const RecentConnection &connection);

  class RecentConnections {
  public:
    explicit RecentConnections(std::wstring key) : key_(std::move(key)) {}
    std::vector<RecentConnection> load() const;
    void remember(const RecentConnection &entry) const;

  private:
    std::wstring key_;
  };
} // namespace flowie::app
