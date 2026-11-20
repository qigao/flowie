#include "loopback_broker.h"
#include "workbench.h"
#include <core/error.h>
#include <functional>
#include <tinytest.hpp>

using namespace flowie::app;
namespace {
  bool until(Session &session, const std::function<bool(const std::vector<Event> &)> &predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    do {
      auto events = session.poll();
      if (predicate(events)) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
  }
  struct OwnedSession {
    Session session;
    ~OwnedSession() {
      session.close();
      until(session, [&](const auto &) { return session.state() == State::idle; });
    }
  };
  bool completed(Session &session, Operation operation) {
    return until(session, [&](const auto &events) {
      for (const auto &event : events)
        if (event.kind == Kind::completed && event.operation == operation)
          return event.status == SALTS_OK && event.reason == 0;
      return false;
    });
  }
} // namespace
suite("MQTT Workbench") {
  it("rejects full record mailboxes and latches overflow outside the queue") {
    Mailbox mailbox;
    for (size_t i = 0; i < kMailboxRecords; ++i)
      check_equal(mailbox.push(Event{}), SALTS_OK);
    check_equal(mailbox.push(Event{}), SALTS_ENOBUFS);
    check_equal(mailbox.take_failure(), SALTS_ENOBUFS);
    check_equal(mailbox.drain().size(), kMailboxRecords);
    check_equal(mailbox.push(Event{}), SALTS_OK);
  }
  it("enforces the byte budget and releases it on consumption") {
    Mailbox mailbox;
    Event event;
    event.payload.assign(kMailboxBytes, 'x');
    check_equal(mailbox.push(std::move(event)), SALTS_OK);
    Event extra;
    extra.topic = "x";
    check_equal(mailbox.push(std::move(extra)), SALTS_ENOBUFS);
    check_equal(mailbox.drain().size(), size_t(1));
    check_equal(mailbox.push(Event{}), SALTS_OK);
  }
  it("preserves per-producer ordering under concurrent callback publication") {
    Mailbox mailbox;
    std::atomic<int> failures{0};
    auto produce = [&](const char *topic) {
      for (int i = 0; i < 100; ++i) {
        Event event;
        event.topic = topic;
        event.payload = std::to_string(i);
        if (mailbox.push(std::move(event)) != SALTS_OK) ++failures;
      }
    };
    std::thread first(produce, "first"), second(produce, "second");
    first.join();
    second.join();
    int a = 0, b = 0;
    for (const auto &event : mailbox.drain()) {
      auto &sequence = event.topic == "first" ? a : b;
      check_equal(std::stoi(event.payload), sequence++);
    }
    check_equal(failures.load(), 0);
    check_equal(a, 100);
    check_equal(b, 100);
  }
  it("reports rolling history eviction and releases payloads on clear") {
    History history;
    for (size_t i = 0; i <= kHistoryRecords; ++i) {
      Event event;
      event.payload = std::to_string(i);
      history.append(std::move(event));
    }
    check_equal(history.records().size(), kHistoryRecords);
    check_equal(history.evicted(), size_t(1));
    check_equal(history.records().front().payload, std::string("1"));
    history.clear();
    check_true(history.records().empty());
    check_equal(history.evicted(), size_t(0));
    for (int i = 0; i < 9; ++i) {
      Event event;
      event.payload.assign(1024 * 1024, 'x');
      history.append(std::move(event));
    }
    check_equal(history.records().size(), size_t(8));
    check_equal(history.evicted(), size_t(1));
  }
  it("validates JSON without rewriting raw values and formats only on request") {
    const std::string input = "{\"id\":18446744073709551615,\"text\":\"你好\"}";
    std::string output, error;
    check_true(format_json(input, nullptr, error));
    check_true(format_json(input, &output, error));
    check_contains(output.c_str(), "18446744073709551615");
    check_contains(output.c_str(), "\r\n");
    check_false(format_json("{bad}", nullptr, error));
    check_false(error.empty());
    check_false(format_json(std::string(kPayloadBytes + 1, '0'), nullptr, error));
  }
  it("rejects invalid configuration before creating a client") {
    OwnedSession owner;
    Connection connection;
    connection.client_id = "workbench-test";
    connection.port = 65536;
    check_equal(owner.session.connect(connection), SALTS_EINVAL);
    check_true(owner.session.state() == State::idle);
    check_false(valid_topic("a/+", false));
    check_false(valid_topic("", true));
  }
  for (auto version : {FLOWIE_MQTT_VERSION_3_1_1, FLOWIE_MQTT_VERSION_5}) {
    it("connects, subscribes, echoes TXT/JSON at QoS 0/1/2 and disconnects (%d)",
       static_cast<int>(version)) {
      LoopbackBroker broker;
      OwnedSession owner;
      auto &session = owner.session;
      Connection connection;
      connection.port = broker.port;
      connection.client_id = "workbench-test";
      connection.version = version;
      check_equal(session.connect(connection), SALTS_OK);
      check_true(completed(session, Operation::connect));
      check_true(session.available());
      check_equal(session.subscribe("workbench/#", 2), SALTS_OK);
      check_true(completed(session, Operation::subscribe));
      for (uint8_t qos = 0; qos <= 2; ++qos) {
        const std::string input = qos == 0   ? "hello 你好"
                                  : qos == 1 ? "{\"reading\":42}"
                                             : std::string("a\0b", 3);
        check_equal(session.publish("workbench/test", input, qos, true), SALTS_OK);
        check_equal(session.ping(), SALTS_EBUSY);
        bool ack = false, received = false;
        check_true(until(session, [&](const auto &events) {
          for (const auto &event : events) {
            if (event.kind == Kind::completed && event.operation == Operation::publish)
              ack = event.status == SALTS_OK;
            if (event.kind == Kind::received)
              received = event.payload == input && event.topic == "workbench/test" && event.retain;
          }
          return ack && received;
        }));
      }
      check_equal(session.unsubscribe("workbench/#"), SALTS_OK);
      check_true(completed(session, Operation::unsubscribe));
      check_equal(session.ping(), SALTS_OK);
      check_true(completed(session, Operation::ping));
      check_equal(session.disconnect(), SALTS_OK);
      check_true(until(session, [&](const auto &) { return session.state() == State::idle; }));
      check_equal(broker.failure.load(), 0);
    }
  }
  it("reports authentication rejection and drains before allowing a new broker") {
    LoopbackBroker broker(true);
    OwnedSession owner;
    Connection connection;
    connection.port = broker.port;
    connection.client_id = "rejected";
    check_equal(owner.session.connect(connection), SALTS_OK);
    bool rejected = false;
    check_true(until(owner.session, [&](const auto &events) {
      for (const auto &event : events)
        if (event.operation == Operation::connect && event.reason == 0x87) rejected = true;
      return rejected && owner.session.state() == State::idle;
    }));
    LoopbackBroker next;
    connection.port = next.port;
    check_equal(owner.session.connect(connection), SALTS_OK);
    check_true(completed(owner.session, Operation::connect));
  }
  it("closes asynchronously while a CONNECT response is outstanding") {
    LoopbackBroker broker(false, true);
    OwnedSession owner;
    Connection connection;
    connection.port = broker.port;
    connection.client_id = "cancelled";
    check_equal(owner.session.connect(connection), SALTS_OK);
    const auto start = std::chrono::steady_clock::now();
    owner.session.close();
    check_less(std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - start)
                   .count(),
               1000LL);
    check_true(
        until(owner.session, [&](const auto &) { return owner.session.state() == State::idle; }));
  }
}
