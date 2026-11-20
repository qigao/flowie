#pragma once
#include "flowie_mqtt_protocol.h"
#include <array>
#include <atomic>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <winsock2.h>

// Bounded test peer for real multi-client routing (QoS 0/1, one filter per request).
// One owner thread accepts and progresses every socket; no shared socket writes.
class RoutingBroker {
public:
  RoutingBroker() {
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data)) throw std::runtime_error("WSAStartup failed");
    listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (listener_ == INVALID_SOCKET ||
        bind(listener_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) ||
        listen(listener_, 8)) {
      if (listener_ != INVALID_SOCKET) closesocket(listener_);
      WSACleanup();
      throw std::runtime_error("Routing peer bind/listen failed");
    }
    int length = sizeof(address);
    if (getsockname(listener_, reinterpret_cast<sockaddr *>(&address), &length)) {
      closesocket(listener_);
      WSACleanup();
      throw std::runtime_error("Routing peer address failed");
    }
    port = ntohs(address.sin_port);
    try {
      thread_ = std::thread([this] { run(); });
    } catch (...) {
      closesocket(listener_);
      WSACleanup();
      throw;
    }
  }
  ~RoutingBroker() {
    stop_.store(true);
    thread_.join();
    closesocket(listener_);
    WSACleanup();
  }
  int port = 0;
  std::atomic<int> failure{0};

private:
  static constexpr size_t kFrameBytes = 1024 * 1024;
  struct Peer {
    SOCKET socket = INVALID_SOCKET;
    flowie_mqtt_version_t version = FLOWIE_MQTT_VERSION_UNSPECIFIED;
    std::vector<uint8_t> input;
    std::set<std::string> filters;
  };
  static flowie_mqtt_span_t span(const std::string &value) {
    return {reinterpret_cast<const uint8_t *>(value.data()), value.size()};
  }
  static std::string copy(flowie_mqtt_span_t value) {
    return {reinterpret_cast<const char *>(value.data), value.size};
  }
  static void require(bool value) {
    if (!value) throw std::runtime_error("Routing fixture protocol/socket failure");
  }
  static void send_bytes(Peer &peer, const uint8_t *data, size_t size) {
    while (size) {
      const int count =
          send(peer.socket, reinterpret_cast<const char *>(data), static_cast<int>(size), 0);
      require(count > 0);
      data += count;
      size -= static_cast<size_t>(count);
    }
  }
  static void control(Peer &peer, flowie_mqtt_packet_type_t type, uint16_t id = 0,
                      int reason = -1) {
    flowie_mqtt_control_packet_t packet = FLOWIE_MQTT_CONTROL_PACKET_INIT;
    packet.version = peer.version;
    packet.type = type;
    packet.packet_id = id;
    const uint8_t code = static_cast<uint8_t>(reason);
    if (reason >= 0) packet.reason_codes = {&code, 1};
    std::array<uint8_t, 64> output{};
    size_t size = 0;
    require(flowie_mqtt_control_packet_encode(&packet, output.data(), output.size(), &size) ==
            FLOWIE_MQTT_PARSE_OK);
    send_bytes(peer, output.data(), size);
  }
  void dispatch(Peer &peer, const flowie_mqtt_packet_view_t &packet) {
    if (packet.type == FLOWIE_MQTT_PACKET_CONNECT) {
      flowie_mqtt_connect_view_t view = FLOWIE_MQTT_CONNECT_VIEW_INIT;
      require(flowie_mqtt_connect_parse(&packet, &view) == FLOWIE_MQTT_PARSE_OK);
      peer.version = view.version;
      control(peer, FLOWIE_MQTT_PACKET_CONNACK);
    } else if (packet.type == FLOWIE_MQTT_PACKET_SUBSCRIBE) {
      flowie_mqtt_subscribe_view_t view = FLOWIE_MQTT_SUBSCRIBE_VIEW_INIT;
      flowie_mqtt_subscription_iterator_t iterator = FLOWIE_MQTT_SUBSCRIPTION_ITERATOR_INIT;
      flowie_mqtt_subscription_view_t entry{};
      require(flowie_mqtt_subscribe_parse(&packet, &view) == FLOWIE_MQTT_PARSE_OK &&
              view.entry_count == 1);
      require(flowie_mqtt_subscription_iterator_init(&packet, &view, &iterator) ==
              FLOWIE_MQTT_PARSE_OK);
      require(flowie_mqtt_subscription_iterator_next(&iterator, &entry) == FLOWIE_MQTT_PARSE_OK);
      const auto filter = copy(entry.filter);
      require(peer.filters.size() < 128 || peer.filters.count(filter));
      peer.filters.insert(filter);
      control(peer, FLOWIE_MQTT_PACKET_SUBACK, view.packet_id, entry.qos);
    } else if (packet.type == FLOWIE_MQTT_PACKET_UNSUBSCRIBE) {
      flowie_mqtt_unsubscribe_view_t view = FLOWIE_MQTT_UNSUBSCRIBE_VIEW_INIT;
      flowie_mqtt_topic_filter_iterator_t iterator = FLOWIE_MQTT_TOPIC_FILTER_ITERATOR_INIT;
      flowie_mqtt_span_t filter{};
      require(flowie_mqtt_unsubscribe_parse(&packet, &view) == FLOWIE_MQTT_PARSE_OK &&
              view.filter_count == 1);
      require(flowie_mqtt_topic_filter_iterator_init(&view, &iterator) == FLOWIE_MQTT_PARSE_OK);
      require(flowie_mqtt_topic_filter_iterator_next(&iterator, &filter) == FLOWIE_MQTT_PARSE_OK);
      peer.filters.erase(copy(filter));
      control(peer, FLOWIE_MQTT_PACKET_UNSUBACK, view.packet_id,
              peer.version == FLOWIE_MQTT_VERSION_5 ? 0 : -1);
    } else if (packet.type == FLOWIE_MQTT_PACKET_PUBLISH) {
      flowie_mqtt_publish_view_t view = FLOWIE_MQTT_PUBLISH_VIEW_INIT;
      require(flowie_mqtt_publish_parse(&packet, &view) == FLOWIE_MQTT_PARSE_OK && view.qos <= 1);
      for (auto &destination : peers_) {
        if (destination.socket == INVALID_SOCKET) continue;
        bool matched = false;
        for (const auto &filter : destination.filters) {
          int result = 0;
          require(flowie_mqtt_topic_matches(span(filter), view.topic, &result) ==
                  FLOWIE_MQTT_PARSE_OK);
          matched = matched || result;
        }
        if (!matched) continue;
        flowie_mqtt_publish_packet_t outgoing = FLOWIE_MQTT_PUBLISH_PACKET_INIT;
        outgoing.version = destination.version;
        outgoing.topic = view.topic;
        outgoing.payload = view.payload;
        std::vector<uint8_t> output(kFrameBytes);
        size_t size = 0;
        require(flowie_mqtt_publish_packet_encode(&outgoing, output.data(), output.size(), &size) ==
                FLOWIE_MQTT_PARSE_OK);
        send_bytes(destination, output.data(), size);
      }
      if (view.qos) control(peer, FLOWIE_MQTT_PACKET_PUBACK, view.packet_id);
    } else if (packet.type == FLOWIE_MQTT_PACKET_PINGREQ)
      control(peer, FLOWIE_MQTT_PACKET_PINGRESP);
    else if (packet.type != FLOWIE_MQTT_PACKET_DISCONNECT) require(false);
  }
  void receive(Peer &peer) {
    std::array<uint8_t, 16384> bytes{};
    const int count = recv(peer.socket, reinterpret_cast<char *>(bytes.data()),
                           static_cast<int>(bytes.size()), 0);
    if (count <= 0) {
      require(count == 0 || WSAGetLastError() == WSAECONNRESET);
      closesocket(peer.socket);
      peer = Peer{};
      return;
    }
    require(static_cast<size_t>(count) <= kFrameBytes - peer.input.size());
    peer.input.insert(peer.input.end(), bytes.begin(), bytes.begin() + count);
    while (!peer.input.empty()) {
      flowie_mqtt_parse_options_t options = FLOWIE_MQTT_PARSE_OPTIONS_INIT;
      options.version = peer.version;
      options.max_packet_size = kFrameBytes;
      flowie_mqtt_packet_view_t view = FLOWIE_MQTT_PACKET_VIEW_INIT;
      size_t consumed = 0;
      const int result = flowie_mqtt_packet_parse(peer.input.data(), peer.input.size(), &options,
                                                  &view, &consumed, nullptr);
      if (result == FLOWIE_MQTT_PARSE_NEED_MORE) return;
      require(result == FLOWIE_MQTT_PARSE_OK);
      dispatch(peer, view); // all views remain borrowed until this input is erased
      peer.input.erase(peer.input.begin(), peer.input.begin() + consumed);
    }
  }
  void run() noexcept {
    try {
      while (!stop_.load()) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listener_, &readable);
        for (const auto &peer : peers_)
          if (peer.socket != INVALID_SOCKET) FD_SET(peer.socket, &readable);
        timeval interval{0, 100000};
        require(select(0, &readable, nullptr, nullptr, &interval) != SOCKET_ERROR);
        if (FD_ISSET(listener_, &readable)) {
          Peer *available = nullptr;
          for (auto &peer : peers_)
            if (peer.socket == INVALID_SOCKET) {
              available = &peer;
              break;
            }
          require(available != nullptr);
          available->socket = accept(listener_, nullptr, nullptr);
          require(available->socket != INVALID_SOCKET);
          const DWORD timeout = 2000;
          require(setsockopt(available->socket, SOL_SOCKET, SO_SNDTIMEO,
                             reinterpret_cast<const char *>(&timeout), sizeof(timeout)) == 0);
        }
        for (auto &peer : peers_)
          if (peer.socket != INVALID_SOCKET && FD_ISSET(peer.socket, &readable)) receive(peer);
      }
    } catch (...) {
      failure.store(1);
    }
    for (auto &peer : peers_)
      if (peer.socket != INVALID_SOCKET) closesocket(peer.socket);
  }
  SOCKET listener_ = INVALID_SOCKET;
  std::array<Peer, 8> peers_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};
