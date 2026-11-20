#pragma once

#include "flowie_mqtt_client.h"
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace flowie::app {
  constexpr size_t kMailboxRecords = 256;
  constexpr size_t kMailboxBytes = 4 * 1024 * 1024;
  constexpr size_t kHistoryRecords = 500;
  constexpr size_t kHistoryBytes = 8 * 1024 * 1024;
  constexpr size_t kPayloadBytes = 1024 * 1024 - 65536;

  enum class Operation { none, connect, publish, subscribe, unsubscribe, ping, disconnect };
  enum class Kind { received, transmitted, completed, error, closed };
  enum class State { idle, connecting, connected, closing, close_failed };

  struct Event {
    Kind kind = Kind::completed;
    Operation operation = Operation::none;
    int status = 0;
    unsigned reason = 0;
    uint8_t qos = 0;
    bool retain = false;
    std::string topic, payload, reasons;
    std::chrono::system_clock::time_point time = std::chrono::system_clock::now();
    size_t bytes() const { return topic.size() + payload.size() + reasons.size(); }
  };

  // Fixed slots: publishing moves owned strings under the lock without allocating.
  class Mailbox {
  public:
    int push(Event event) noexcept;
    std::vector<Event> drain();
    void fail(int status) noexcept;
    int take_failure() noexcept { return failure_.exchange(0); }

  private:
    std::mutex mutex_;
    std::array<std::optional<Event>, kMailboxRecords> slots_;
    size_t head_ = 0, count_ = 0, bytes_ = 0;
    std::atomic<int> failure_{0};
  };

  class History {
  public:
    void append(Event event);
    void clear();
    const std::deque<Event> &records() const { return records_; }
    size_t evicted() const { return evicted_; }

  private:
    std::deque<Event> records_;
    size_t bytes_ = 0, evicted_ = 0;
  };

  struct Connection {
    std::string host = "127.0.0.1", path = "/mqtt", client_id;
    std::string username, password, ca_file, cert_file, key_file;
    int port = 1883;
    flowie_mqtt_client_transport_t transport = FLOWIE_MQTT_CLIENT_TRANSPORT_TCP;
    flowie_mqtt_version_t version = FLOWIE_MQTT_VERSION_5;
  };

  // Private application model. All methods except callbacks/closer run on the UI thread.
  class Session {
  public:
    ~Session();
    int connect(const Connection &connection);
    int publish(const std::string &topic, const std::string &payload, uint8_t qos, bool retain);
    int subscribe(const std::string &filter, uint8_t qos);
    int unsubscribe(const std::string &filter);
    int ping();
    int disconnect();
    void close() noexcept;
    std::vector<Event> poll();
    State state() const { return state_; }
    bool available() const { return state_ == State::connected && pending_ == Operation::none; }

  private:
    template <Operation Op>
    static void complete(flowie_mqtt_client_t *, int, const flowie_mqtt_control_packet_view_t *,
                         void *) noexcept;
    static int received(flowie_mqtt_client_t *, const flowie_mqtt_publish_view_t *,
                        void *) noexcept;
    static void error(flowie_mqtt_client_t *, int, void *) noexcept;
    int admit(int status, Operation operation);
    Mailbox mailbox_;
    flowie_mqtt_client_t *client_ = nullptr;
    State state_ = State::idle;
    Operation pending_ = Operation::none;
    flowie_mqtt_version_t version_ = FLOWIE_MQTT_VERSION_5;
    std::thread closer_;
    std::atomic<bool> close_done_{false};
    int close_status_ = 0; // published by close_done_ release/acquire
    std::chrono::steady_clock::time_point next_ping_;
  };

  bool format_json(const std::string &input, std::string *formatted, std::string &error);
  bool valid_topic(const std::string &topic, bool filter);
  const char *operation_name(Operation operation);
} // namespace flowie::app
