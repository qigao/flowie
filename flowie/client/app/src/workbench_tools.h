#pragma once
#include "workbench.h"

namespace flowie::app {
  enum class ToolKind { http, rpc, tcp, serial };
  constexpr size_t kToolBytes = 64 * 1024;
  constexpr size_t kToolHeadersBytes = 8 * 1024;
  struct ToolDraft {
    ToolKind kind = ToolKind::http;
    std::string endpoint, method, headers, body;
    bool hex = false;
    int baud = 115200, bits = 8, parity = 0, stop_bits = 1, flow = 0;
  };
  const wchar_t *tool_label(ToolKind kind);
  bool request_tool(ToolKind kind);
  std::string tool_payload(const ToolDraft &draft);

  // UI submits at most one write. The worker owns all SDK calls and borrowed callbacks.
  class ToolSession {
  public:
    ~ToolSession();
    void start(ToolDraft draft);
    void send(std::string bytes);
    void stop() noexcept;
    void reap();
    State state() const { return state_.load(); }
    bool busy() const { return sending_.load(); }
    bool stopping() const { return stopping_.load(); }
    std::vector<Event> poll();

  private:
    void run(ToolDraft draft) noexcept;
    void http(const ToolDraft &draft);
    void tcp(const ToolDraft &draft);
    void serial(const ToolDraft &draft);
    template <class Owner, class Stop, class Destroy>
    void drain_owner(Owner &owner, Stop stop, Destroy destroy) noexcept;
    void record(Kind kind, std::string_view label, std::string_view data = {}, int code = 0,
                unsigned reason = 0, std::string_view metadata = {}) noexcept;
    std::optional<std::string> take_send();
    Mailbox events_;
    std::thread worker_;
    std::mutex commands_;
    std::optional<std::string> pending_;
    std::atomic<State> state_{State::idle};
    std::atomic<bool> stopping_{false}, sending_{false}, retry_{false};
  };
} // namespace flowie::app
