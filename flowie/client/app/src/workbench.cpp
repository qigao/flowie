#include "workbench.h"
#include <core/error.h>
#include <json_parser.h>
#include <memory>
#include <stdexcept>
#include <system_error>

namespace flowie::app {
  namespace {
    flowie_mqtt_span_t span(const std::string &value) {
      return {reinterpret_cast<const uint8_t *>(value.data()), value.size()};
    }
    std::string copy(flowie_mqtt_span_t value) {
      return value.size ? std::string(reinterpret_cast<const char *>(value.data), value.size)
                        : std::string{};
    }
  } // namespace

  void Mailbox::fail(int status) noexcept {
    int expected = 0;
    failure_.compare_exchange_strong(expected, status);
  }
  int Mailbox::push(Event event) noexcept {
    // Subtraction keeps accounting valid even for hostile size combinations.
    size_t remaining = kMailboxBytes;
    for (const auto *field : {&event.topic, &event.payload, &event.reasons}) {
      if (field->size() > remaining) {
        fail(SALTS_ENOBUFS);
        return SALTS_ENOBUFS;
      }
      remaining -= field->size();
    }
    const auto size = kMailboxBytes - remaining;
    std::lock_guard<std::mutex> lock(mutex_);
    if (count_ == kMailboxRecords || size > kMailboxBytes - bytes_) {
      fail(SALTS_ENOBUFS);
      return SALTS_ENOBUFS;
    }
    slots_[(head_ + count_) % kMailboxRecords].emplace(std::move(event));
    ++count_;
    bytes_ += size;
    return SALTS_OK;
  }
  std::vector<Event> Mailbox::drain() {
    std::vector<Event> result;
    result.reserve(kMailboxRecords);
    std::lock_guard<std::mutex> lock(mutex_);
    while (count_) {
      auto &slot = slots_[head_];
      result.push_back(std::move(*slot));
      slot.reset();
      head_ = (head_ + 1) % kMailboxRecords;
      --count_;
    }
    bytes_ = 0;
    return result;
  }
  void History::append(Event event) {
    if (event.topic.size() > kHistoryBytes ||
        event.payload.size() > kHistoryBytes - event.topic.size() ||
        event.reasons.size() > kHistoryBytes - event.topic.size() - event.payload.size())
      throw std::length_error("History record exceeds byte budget");
    const auto size = event.bytes();
    while (records_.size() >= kHistoryRecords || size > kHistoryBytes - bytes_) {
      bytes_ -= records_.front().bytes();
      records_.pop_front();
      ++evicted_;
    }
    records_.push_back(std::move(event));
    bytes_ += size;
  }
  void History::clear() {
    records_.clear();
    bytes_ = 0;
    evicted_ = 0;
  }

  template <Operation Op>
  void Session::complete(flowie_mqtt_client_t *, int status,
                         const flowie_mqtt_control_packet_view_t *response,
                         void *context) noexcept {
    auto &self = *static_cast<Session *>(context);
    try {
      Event event;
      event.operation = Op;
      event.status = status;
      if (response) {
        event.reason = response->reason_code;
        event.reasons = copy(response->reason_codes);
      }
      self.mailbox_.push(std::move(event));
    } catch (...) {
      self.mailbox_.fail(SALTS_ENOMEM);
    }
  }
  int Session::received(flowie_mqtt_client_t *, const flowie_mqtt_publish_view_t *message,
                        void *context) noexcept {
    auto &self = *static_cast<Session *>(context);
    try {
      Event event;
      event.kind = Kind::received;
      event.topic = copy(message->topic);
      event.payload = copy(message->payload);
      event.qos = message->qos;
      event.retain = message->retain != 0;
      return self.mailbox_.push(std::move(event));
    } catch (...) {
      self.mailbox_.fail(SALTS_ENOMEM);
      return SALTS_ENOMEM;
    }
  }
  void Session::error(flowie_mqtt_client_t *, int status, void *context) noexcept {
    auto &self = *static_cast<Session *>(context);
    Event event;
    event.kind = Kind::error;
    event.status = status;
    self.mailbox_.push(std::move(event));
  }
  Session::~Session() {
    // The window may be released only after poll observes successful drain.
    if (closer_.joinable()) closer_.join();
    if (client_ != nullptr) std::terminate(); // Never release a still-live callback context.
  }
  int Session::admit(int status, Operation operation) {
    if (status == SALTS_OK) pending_ = operation;
    if (status == SALTS_OK)
      next_ping_ = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    return status;
  }
  int Session::connect(const Connection &connection) {
    if (state_ != State::idle) return SALTS_EBUSY;
    if (connection.host.empty() || connection.host.find('\0') != std::string::npos ||
        connection.port < 1 || connection.port > 65535 || connection.client_id.empty() ||
        connection.client_id.size() > 65535 || connection.username.size() > 65535 ||
        connection.password.size() > 65535 ||
        connection.cert_file.empty() != connection.key_file.empty())
      return SALTS_EINVAL;
    flowie_mqtt_client_config_t config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
    const flowie_mqtt_client_topic_handler_t handlers[] = {
        {{reinterpret_cast<const uint8_t *>("#"), 1}, received},
        {{reinterpret_cast<const uint8_t *>("$SYS/#"), 6}, received}};
    config.host = connection.host.c_str();
    config.port = connection.port;
    config.path = connection.path.c_str();
    config.transport = connection.transport;
    config.timeout_ms = 5000;
    config.topic_handlers = {handlers, 2};
    config.tls = {connection.ca_file.empty() ? nullptr : connection.ca_file.c_str(),
                  connection.cert_file.empty() ? nullptr : connection.cert_file.c_str(),
                  connection.key_file.empty() ? nullptr : connection.key_file.c_str(), nullptr};
    config.on_connect = complete<Operation::connect>;
    config.on_publish = complete<Operation::publish>;
    config.on_subscribe = complete<Operation::subscribe>;
    config.on_unsubscribe = complete<Operation::unsubscribe>;
    config.on_ping = complete<Operation::ping>;
    config.on_disconnect = complete<Operation::disconnect>;
    config.on_error = error;
    config.user_data = this;
    auto status = flowie_mqtt_client_create(&config, &client_);
    if (status != SALTS_OK) return status;
    state_ = State::connecting;
    version_ = connection.version;
    status = flowie_mqtt_client_set_version(client_, version_);
    flowie_mqtt_connect_packet_t packet = FLOWIE_MQTT_CONNECT_PACKET_INIT;
    packet.version = version_;
    packet.clean_start = 1;
    packet.keep_alive = 30;
    packet.client_id = span(connection.client_id);
    packet.has_username = !connection.username.empty();
    packet.has_password = !connection.password.empty();
    packet.username = span(connection.username);
    packet.password = span(connection.password);
    if (status == SALTS_OK) status = flowie_mqtt_client_connect(client_, &packet);
    if (status != SALTS_OK) close();
    return admit(status, Operation::connect);
  }
  bool valid_topic(const std::string &topic, bool filter) {
    if (topic.empty() || topic.size() > 65535 || topic.find('\0') != std::string::npos)
      return false;
    if (!filter) return topic.find_first_of("+#") == std::string::npos;
    // The SDK validates the complete MQTT UTF-8/filter grammar at admission.
    return true;
  }
  int Session::publish(const std::string &topic, const std::string &payload, uint8_t qos,
                       bool retain) {
    if (!available()) return SALTS_EBUSY;
    if (!valid_topic(topic, false) || payload.size() > kPayloadBytes || qos > 2)
      return SALTS_EINVAL;
    flowie_mqtt_client_publish_topic_t entry{};
    entry.topic = span(topic);
    entry.payload = span(payload);
    entry.qos = qos;
    entry.retain = retain;
    flowie_mqtt_client_publish_topic_vec_t packet = FLOWIE_MQTT_CLIENT_PUBLISH_TOPIC_VEC_INIT;
    packet.version = version_;
    packet.data = &entry;
    packet.count = 1;
    return admit(flowie_mqtt_client_publish(client_, &packet), Operation::publish);
  }
  int Session::subscribe(const std::string &filter, uint8_t qos) {
    if (!available()) return SALTS_EBUSY;
    if (!valid_topic(filter, true) || qos > 2) return SALTS_EINVAL;
    // '#' intentionally excludes reserved '$' topics; this release captures $SYS only.
    if (filter[0] == '$' && filter.rfind("$SYS/", 0) != 0) return SALTS_ENOTSUP;
    flowie_mqtt_subscription_t entry{};
    entry.filter = span(filter);
    entry.qos = qos;
    flowie_mqtt_subscribe_packet_t packet = FLOWIE_MQTT_SUBSCRIBE_PACKET_INIT;
    packet.version = version_;
    packet.subscriptions = &entry;
    packet.subscription_count = 1;
    return admit(flowie_mqtt_client_subscribe(client_, &packet), Operation::subscribe);
  }
  int Session::unsubscribe(const std::string &filter) {
    if (!available()) return SALTS_EBUSY;
    if (!valid_topic(filter, true)) return SALTS_EINVAL;
    auto entry = span(filter);
    flowie_mqtt_unsubscribe_packet_t packet = FLOWIE_MQTT_UNSUBSCRIBE_PACKET_INIT;
    packet.version = version_;
    packet.filters = &entry;
    packet.filter_count = 1;
    return admit(flowie_mqtt_client_unsubscribe(client_, &packet), Operation::unsubscribe);
  }
  int Session::ping() {
    return available() ? admit(flowie_mqtt_client_ping(client_), Operation::ping) : SALTS_EBUSY;
  }
  int Session::disconnect() {
    return available() ? admit(flowie_mqtt_client_disconnect(client_, 0, {nullptr, 0}),
                               Operation::disconnect)
                       : SALTS_EBUSY;
  }
  void Session::close() noexcept {
    if (!client_ || state_ == State::closing) return;
    close_done_.store(false);
    int status = SALTS_OK;
    try {
      closer_ = std::thread([this] {
        close_status_ = flowie_mqtt_client_try_destroy(client_, 30000);
        close_done_.store(true, std::memory_order_release);
      });
      state_ = State::closing;
      return;
    } catch (const std::system_error &error) {
      status = -error.code().value();
    } catch (const std::bad_alloc &) {
      status = SALTS_ENOMEM;
    }
    state_ = State::close_failed;
    Event event;
    event.kind = Kind::closed;
    event.status = status;
    mailbox_.push(std::move(event));
  }
  std::vector<Event> Session::poll() {
    auto result = mailbox_.drain();
    const int failure = mailbox_.take_failure();
    if (failure) {
      Event event;
      event.kind = Kind::error;
      event.status = failure;
      result.push_back(std::move(event));
    }
    for (const auto &event : result) {
      if (event.kind == Kind::completed) {
        pending_ = Operation::none;
        if (event.operation == Operation::connect && state_ == State::connecting) {
          if (event.status == SALTS_OK && event.reason == 0) state_ = State::connected;
          else close();
        } else if (event.operation == Operation::disconnect ||
                   (event.status != SALTS_OK && state_ == State::connected))
          close();
      } else if (event.kind == Kind::error &&
                 (state_ == State::connecting || state_ == State::connected))
        close();
    }
    if (state_ == State::closing && close_done_.load(std::memory_order_acquire)) {
      closer_.join();
      // No callbacks remain after successful destruction. Drain before next generation.
      auto tail = mailbox_.drain();
      for (auto &event : tail)
        result.push_back(std::move(event));
      const auto tail_failure = mailbox_.take_failure();
      if (tail_failure) {
        Event event;
        event.kind = Kind::error;
        event.status = tail_failure;
        result.push_back(std::move(event));
      }
      state_ = close_status_ == SALTS_OK ? State::idle : State::close_failed;
      if (state_ == State::idle) {
        client_ = nullptr;
        pending_ = Operation::none;
      }
      Event event;
      event.kind = Kind::closed;
      event.status = close_status_;
      result.push_back(std::move(event));
    }
    if (available() && std::chrono::steady_clock::now() >= next_ping_) {
      const int status = ping();
      if (status != SALTS_OK) {
        Event event;
        event.kind = Kind::error;
        event.status = status;
        result.push_back(std::move(event));
        close();
      }
    }
    return result;
  }
  bool format_json(const std::string &input, std::string *formatted, std::string &error_text) {
    if (input.size() > kPayloadBytes) {
      error_text = "Payload exceeds 960 KiB limit";
      return false;
    }
    std::unique_ptr<json_value_t, decltype(&json_free)> value(
        json_parse(input.data(), input.size()), json_free);
    if (!value) {
      const auto *error = json_get_error();
      error_text = error ? error : "Invalid JSON";
      return false;
    }
    if (formatted) {
      size_t size = 0;
      std::unique_ptr<char, decltype(&json_serialize_free)> output(
          json_serialize_pretty_crlf(value.get(), &size), json_serialize_free);
      if (!output) {
        error_text = "JSON serialization failed";
        return false;
      }
      if (size > kPayloadBytes) {
        error_text = "Formatted JSON exceeds 960 KiB limit";
        return false;
      }
      formatted->assign(output.get(), size);
    }
    error_text.clear();
    return true;
  }
  const char *operation_name(Operation operation) {
    switch (operation) {
    case Operation::connect:
      return "CONNECT";
    case Operation::publish:
      return "PUBLISH";
    case Operation::subscribe:
      return "SUBSCRIBE";
    case Operation::unsubscribe:
      return "UNSUBSCRIBE";
    case Operation::ping:
      return "PING";
    case Operation::disconnect:
      return "DISCONNECT";
    default:
      return "EVENT";
    }
  }
} // namespace flowie::app
