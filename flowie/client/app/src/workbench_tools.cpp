#include "workbench_tools.h"
#include <cnet/cnet.h>
#include <core/error.h>
#include <http_client/http.h>
#include <json_parser.h>
#include <memory/buffer.h>
#include <serial.h>
#include <uri_parser.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <utility>

namespace flowie::app {
  namespace {
    using namespace std::chrono_literals;
    constexpr uint32_t kTimeout = 10000;
    constexpr uint32_t kTick = 20;
    constexpr char kRpcId[] = "flowie-workbench";

    cnet_client_config network_config(bool tls, size_t extra_send_bytes = 0) {
      cnet_client_config config{};
      config.backend = NATIVE_IO_BACKEND_IOCP;
      config.connection_capacity = 1;
      config.command_capacity = 8;
      config.request_capacity = 8;
      config.completion_batch_capacity = 8;
      config.event_capacity = 16;
      config.max_send_bytes = kToolBytes + kToolHeadersBytes + 4096 + extra_send_bytes;
      config.receive_buffer_bytes = 4096;
      config.connect_timeout_ms = kTimeout;
      config.write_timeout_ms = kTimeout;
      if (tls) {
        config.tls_io_buffer_bytes = kToolBytes;
        config.tls_handshake_timeout_ms = kTimeout;
      }
      return config;
    }

    struct Endpoint {
      std::string uri, authority, target, host;
      bool tls;
    };
    Endpoint endpoint(const std::string &text, bool http) {
      uri_t uri{};
      if (!uri_parse(text.c_str(), &uri) || uri.overflow_flags || !uri.host[0] ||
          (uri.component_flags & (URI_COMPONENT_USERINFO | URI_COMPONENT_FRAGMENT)))
        throw std::runtime_error("Enter a complete URL without credentials or a fragment.");
      const std::string_view scheme(uri.scheme);
      const bool tls = scheme == "https";
      if ((http && scheme != "http" && !tls) || (!http && scheme != "tcp"))
        throw std::runtime_error(http ? "Use http:// or https://." : "Use tcp://host:port.");
      const bool has_port = (uri.component_flags & URI_COMPONENT_PORT) != 0;
      if ((!http && (!has_port || *uri.path || (uri.component_flags & URI_COMPONENT_QUERY))) ||
          (has_port && (uri.port <= 0 || uri.port > 65535)))
        throw std::runtime_error("The endpoint has an invalid port or path.");
      std::string host(uri.host);
      if (uri.host_type == URI_HOST_IPV6ADDR && host.front() != '[') host = "[" + host + "]";
      const auto port = std::to_string(has_port ? uri.port : tls ? 443 : 80);
      std::string target = *uri.path ? uri.path : "/";
      if (uri.component_flags & URI_COMPONENT_QUERY) target += std::string("?") + uri.query;
      return {(tls ? "tls://" : "tcp://") + host + ":" + port, has_port ? host + ":" + port : host,
              std::move(target), std::move(host), tls};
    }

    using Json = std::unique_ptr<json_value_t, decltype(&json_free)>;
    void add_json(json_value_t *obj, const char *key, Json value) {
      if (!value || !json_object_add_checked(obj, key, value.get())) throw std::bad_alloc();
      value.release();
    }
    std::string rpc_body(const ToolDraft &draft) {
      Json root(json_create_object(), json_free);
      if (!root) throw std::bad_alloc();
      if (draft.method.empty() || draft.method.size() > 128 || draft.method.rfind("rpc.", 0) == 0)
        throw std::runtime_error("Enter a JSON-RPC method (maximum 128 bytes).");
      add_json(root.get(), "jsonrpc", Json(json_create_string("2.0"), json_free));
      add_json(root.get(), "method", Json(json_create_string(draft.method.c_str()), json_free));
      // Each tab has just one in-flight call and a fresh HTTP owner per run.
      add_json(root.get(), "id", Json(json_create_string(kRpcId), json_free));
      if (!draft.body.empty()) {
        Json params(json_parse(draft.body.data(), draft.body.size()), json_free);
        if (!params ||
            (json_type(params.get()) != JSON_OBJECT && json_type(params.get()) != JSON_ARRAY))
          throw std::runtime_error("JSON-RPC params must be a JSON object or array.");
        add_json(root.get(), "params", std::move(params));
      }
      size_t size = 0;
      std::unique_ptr<char, decltype(&json_serialize_free)> output(
          json_serialize(root.get(), &size), json_serialize_free);
      if (!output) throw std::bad_alloc();
      return std::string(output.get(), size);
    }
    std::string_view rpc_result(std::string_view bytes) {
      Json root(json_parse(bytes.data(), bytes.size()), json_free);
      if (!root || json_type(root.get()) != JSON_OBJECT) return "Invalid JSON-RPC envelope";
      const auto *version = json_object_get(root.get(), "jsonrpc");
      const auto *id = json_object_get(root.get(), "id");
      if (!version || json_type(version) != JSON_STRING ||
          std::string_view(json_string(version), json_string_len(version)) != "2.0" || !id ||
          json_type(id) != JSON_STRING ||
          std::string_view(json_string(id), json_string_len(id)) != kRpcId)
        return "Invalid JSON-RPC version or response id";
      const auto *result = json_object_get(root.get(), "result");
      const auto *error = json_object_get(root.get(), "error");
      if (result && !error) return "JSON-RPC result";
      if (error && !result && json_type(error) == JSON_OBJECT) {
        const auto *code = json_object_get(error, "code");
        const auto *message = json_object_get(error, "message");
        if (code && json_type(code) == JSON_NUMBER && message && json_type(message) == JSON_STRING)
          return "JSON-RPC remote error";
      }
      return "Invalid JSON-RPC result/error";
    }

    chttp_method http_method(std::string_view name) {
      constexpr std::pair<std::string_view, chttp_method> methods[] = {
          {"GET", CHTTP_METHOD_GET},        {"HEAD", CHTTP_METHOD_HEAD},
          {"POST", CHTTP_METHOD_POST},      {"PUT", CHTTP_METHOD_PUT},
          {"DELETE", CHTTP_METHOD_DELETE},  {"PATCH", CHTTP_METHOD_PATCH},
          {"OPTIONS", CHTTP_METHOD_OPTIONS}};
      for (const auto &[label, method] : methods)
        if (name == label) return method;
      throw std::runtime_error("Choose an HTTP method.");
    }

    void lower_ascii(std::string &text) {
      for (auto &ch : text)
        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    }

    struct RequestHeaders {
      std::vector<std::pair<std::string, std::string>> entries;
      bool manual_cookie = false;
    };
    RequestHeaders http_headers(std::string_view text, bool json) {
      if (text.size() > kToolHeadersBytes)
        throw std::runtime_error("HTTP headers exceed 8 KiB.");
      RequestHeaders headers;
      bool content_type = false;
      while (!text.empty()) {
        const auto end = text.find('\n');
        auto line = text.substr(0, end);
        text = end == text.npos ? std::string_view{} : text.substr(end + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) continue;
        const auto colon = line.find(':');
        if (colon == line.npos || !colon || line.find('\r') != line.npos)
          throw std::runtime_error("Enter one Header: value per line.");
        const auto name = line.substr(0, colon);
        auto value = line.substr(colon + 1);
        constexpr std::string_view punctuation("!#$%&'*+-.^`_|~");
        if (std::any_of(name.begin(), name.end(), [&](unsigned char c) {
              return !(c >= '0' && c <= '9') && !(c >= 'A' && c <= 'Z') &&
                     !(c >= 'a' && c <= 'z') && punctuation.find(c) == punctuation.npos;
            }) ||
            std::any_of(value.begin(), value.end(), [](unsigned char c) {
              return (c < 0x20 && c != '\t') || c == 0x7f;
            }))
          throw std::runtime_error("Invalid HTTP header name or value.");
        const auto first = value.find_first_not_of(" \t");
        value.remove_prefix(first == value.npos ? value.size() : first);
        std::string lower(name);
        lower_ascii(lower);
        if (lower == "host" || lower == "content-length" || lower == "transfer-encoding")
          throw std::runtime_error(
              "Host and message framing headers are generated from the request.");
        content_type |= lower == "content-type";
        headers.manual_cookie |= lower == "cookie";
        headers.entries.emplace_back(name, value);
        if (headers.entries.size() > 30)
          throw std::runtime_error("At most 30 custom headers are supported.");
      }
      if (json && !content_type) headers.entries.emplace_back("Content-Type", "application/json");
      return headers;
    }

    struct HttpRequest {
      Endpoint destination;
      chttp_method method;
      std::string body;
      RequestHeaders headers;
    };

    HttpRequest prepare_http_request(const ToolDraft &draft) {
      auto destination = endpoint(draft.endpoint, true);
      const bool rpc = draft.kind == ToolKind::rpc;
      const auto method = rpc ? CHTTP_METHOD_POST : http_method(draft.method);
      auto body = rpc ? rpc_body(draft) : draft.body;
      if (body.size() > kToolBytes) throw std::runtime_error("Encoded body exceeds 64 KiB.");
      if (!rpc && draft.json_body && !body.empty()) {
        std::string error;
        if (!format_json(body, nullptr, error)) throw std::runtime_error(error);
      }
      return {std::move(destination), method, std::move(body),
              http_headers(draft.headers, rpc || draft.json_body)};
    }
  } // namespace

  const wchar_t *tool_label(ToolKind kind) {
    switch (kind) {
    case ToolKind::http:
      return L"HTTP";
    case ToolKind::rpc:
      return L"JSON-RPC";
    case ToolKind::tcp:
      return L"TCP";
    case ToolKind::serial:
      return L"Serial";
    }
    std::terminate();
  }
  bool request_tool(ToolKind kind) { return kind == ToolKind::http || kind == ToolKind::rpc; }

  std::string collection_request_headers(std::string_view text, bool omit_credentials) {
    const auto parsed = http_headers(text, false);
    std::string stored;
    for (const auto &[name, value] : parsed.entries) {
      auto lower = name;
      lower_ascii(lower);
      if (lower == "authorization" || lower == "proxy-authorization" || lower == "cookie" ||
          lower == "x-api-key" || lower == "x-auth-token" || lower == "x-access-token" ||
          lower == "x-csrf-token") {
        if (omit_credentials) continue;
        throw std::runtime_error("Collection files cannot contain credential headers: " + name);
      }
      if (!stored.empty()) stored += '\n';
      stored += name + (value.empty() ? ":" : ": " + value);
    }
    if (stored.size() > kToolHeadersBytes)
      throw std::runtime_error("HTTP headers exceed 8 KiB.");
    return stored;
  }

  std::string tool_remote(ToolKind kind, const std::string &address) {
    if (kind == ToolKind::serial) return address;
    const bool http = request_tool(kind);
    const auto remote = endpoint(address, http);
    return http ? std::string(remote.tls ? "https://" : "http://") + remote.authority
                : remote.uri;
  }

  void validate_http_request(const ToolDraft &draft) {
    (void)prepare_http_request(draft);
  }

  bool successful_http_result(const Event &event) {
    return event.kind == Kind::received && event.status == SALTS_OK && event.reason >= 200 &&
           event.reason < 400 && event.topic != "JSON-RPC remote error";
  }

  std::string tool_payload(const ToolDraft &draft) {
    if (!draft.hex) return draft.body;
    std::string output;
    int high = -1;
    for (const unsigned char ch : draft.body) {
      if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') continue;
      const int digit = ch >= '0' && ch <= '9'   ? ch - '0'
                        : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                        : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10
                                                 : -1;
      if (digit < 0) throw std::runtime_error("Hex input must contain complete byte pairs.");
      if (high < 0) high = digit;
      else {
        output.push_back(static_cast<char>((high << 4) | digit));
        high = -1;
      }
    }
    if (high >= 0) throw std::runtime_error("Hex input must contain complete byte pairs.");
    return output;
  }

  ToolSession::~ToolSession() {
    stop();
    if (worker_.joinable()) worker_.join();
  }
  void ToolSession::reap() {
    if (state() == State::idle && worker_.joinable()) worker_.join();
  }
  void ToolSession::clear_cookies() {
    if (state() != State::idle) throw std::runtime_error("Wait for the HTTP request to finish.");
    reap();
    cookies_.clear();
  }
  void ToolSession::start(ToolDraft draft) {
    if (state() != State::idle) throw std::runtime_error("The tab is busy.");
    reap();
    stopping_ = retry_ = sending_ = false;
    state_ = State::connecting;
    try {
      worker_ = std::thread([this, draft = std::move(draft)]() mutable { run(std::move(draft)); });
    } catch (...) {
      state_ = State::idle;
      throw;
    }
  }
  void ToolSession::send(std::string bytes) {
    if (bytes.empty() || bytes.size() > kToolBytes)
      throw std::runtime_error("Send between 1 byte and 64 KiB.");
    std::lock_guard lock(commands_);
    if (state() != State::connected || stopping_ || sending_)
      throw std::runtime_error("This stream is unavailable or already sending.");
    pending_.emplace(std::move(bytes));
    sending_ = true;
  }
  std::optional<std::string> ToolSession::take_send() {
    std::lock_guard lock(commands_);
    return std::exchange(pending_, std::nullopt);
  }
  void ToolSession::stop() noexcept {
    stopping_ = true;
    if (state() == State::close_failed) retry_ = true;
  }
  std::vector<Event> ToolSession::poll() {
    reap();
    auto records = events_.drain();
    if (const auto failure = events_.take_failure()) {
      Event event;
      event.kind = Kind::error;
      event.status = failure;
      event.topic = "Event capacity exceeded; I/O stopped";
      records.push_back(std::move(event));
    }
    return records;
  }
  void ToolSession::record(Kind kind, std::string_view label, std::string_view data, int code,
                           unsigned reason, std::string_view metadata) noexcept {
    try {
      if (data.size() > kToolBytes || metadata.size() > kToolHeadersBytes) {
        events_.fail(SALTS_ENOBUFS);
        stopping_ = true;
        return;
      }
      Event event;
      event.kind = kind;
      event.topic = label;
      event.payload = data;
      event.reasons = metadata;
      event.status = code;
      event.reason = reason;
      if (events_.push(std::move(event)) != SALTS_OK) stopping_ = true;
    } catch (...) {
      events_.fail(SALTS_ENOMEM);
      stopping_ = true;
    }
  }
  void ToolSession::run(ToolDraft draft) noexcept {
    try {
      if (draft.endpoint.empty() || draft.endpoint.size() > 4096 ||
          draft.body.size() > kToolBytes || draft.headers.size() > kToolHeadersBytes)
        throw std::runtime_error("Check the endpoint, body (64 KiB), and headers (8 KiB).");
      if (request_tool(draft.kind)) http(draft);
      else if (draft.kind == ToolKind::tcp) tcp(draft);
      else serial(draft);
    } catch (const std::exception &error) {
      record(Kind::error, error.what(), {}, SALTS_EINVAL);
    }
    {
      std::lock_guard lock(commands_);
      if (pending_) record(Kind::error, "Queued write canceled", {}, SALTS_ECANCELED);
      pending_.reset();
      sending_ = false;
      state_ = State::idle;
    }
  }

  template <class Owner, class Stop, class Destroy>
  void ToolSession::drain_owner(Owner &owner, Stop stop, Destroy destroy) noexcept {
    state_ = State::closing;
    for (;;) {
      int status;
      do {
        status = stop(&owner, 100);
      } while (status == SALTS_ETIMEDOUT);
      if (status != SALTS_OK) record(Kind::error, "Stop I/O", {}, status);
      status = destroy(&owner);
      if (status == SALTS_OK) return;
      record(Kind::error, "Close failed. Retry close.", {}, status);
      state_ = State::close_failed;
      while (!retry_.exchange(false))
        std::this_thread::sleep_for(25ms);
      state_ = State::closing;
    }
  }

  void ToolSession::http(const ToolDraft &draft) {
    auto [destination, method, body, request_headers] = prepare_http_request(draft);
    const bool rpc = draft.kind == ToolKind::rpc;
    auto origin = destination.uri;
    lower_ascii(origin);
    const auto path = std::string_view(destination.target).substr(0, destination.target.find('?'));
    if (draft.use_cookies && !request_headers.manual_cookie) {
      auto cookie = cookies_.header(origin, path);
      if (!cookie.empty()) request_headers.entries.emplace_back("Cookie", std::move(cookie));
    }
    std::vector<chttp_header> headers;
    for (const auto &[key, value] : request_headers.entries)
      headers.push_back({key.c_str(), value.c_str()});
    struct Callback {
      ToolSession *session;
      const Endpoint &destination;
      std::string_view origin, path;
      bool rpc, use_cookies, complete = false;
      std::string response_body;
    } callback{this, destination, origin, path, rpc, draft.use_cookies};
    const chttp_body_sink sink{[](void *context, const void *data, size_t size) noexcept -> int {
                                 auto &bytes = static_cast<Callback *>(context)->response_body;
                                 if (size > kToolBytes - bytes.size()) return SALTS_ENOBUFS;
                                 try {
                                   if (size) bytes.append(static_cast<const char *>(data), size);
                                   return SALTS_OK;
                                 } catch (...) {
                                   return SALTS_ENOMEM;
                                 }
                               },
                               &callback};
    chttp_request_options options{};
    options.connection_uri = destination.uri.c_str();
    options.authority = destination.authority.c_str();
    options.target = destination.target.c_str();
    options.method = method;
    options.headers = headers.data();
    options.header_count = headers.size();
    options.body = body.empty() ? nullptr : body.data();
    options.body_size = body.size();
    options.body_sink = &sink;
    options.user = &callback;
    options.on_complete = [](void *context, chttp_request, const chttp_response_view *response,
                             const chttp_error *error) noexcept {
      auto &cb = *static_cast<Callback *>(context);
      cb.complete = true;
      if (!response) {
        cb.session->record(Kind::error,
                           error && error->stage ? error->stage : "HTTP request failed", {},
                           error ? error->status : SALTS_EIO);
        return;
      }
      try {
        std::string header_text;
        std::optional<HttpCookies> updated_cookies;
        bool cookie_capacity = true;
        for (size_t i = 0; i < response->header_count; ++i) {
          const auto &header = response->headers[i];
          std::string lower(header.name);
          lower_ascii(lower);
          const bool set_cookie = lower == "set-cookie";
          if (set_cookie && cb.use_cookies && cookie_capacity) {
            if (!updated_cookies) updated_cookies.emplace(cb.session->cookies_);
            cookie_capacity = updated_cookies->store(cb.origin, cb.destination.host, cb.path,
                                                      cb.destination.tls, header.value);
          }
          header_text += header.name;
          header_text += ": ";
          header_text += set_cookie ? "[redacted]" : header.value;
          header_text += "\r\n";
        }
        if (updated_cookies && cookie_capacity) cb.session->cookies_ = std::move(*updated_cookies);
        const std::string_view data = cb.response_body;
        const auto label = !cookie_capacity ? std::string_view("HTTP cookie limit exceeded")
                           : cb.rpc        ? rpc_result(data)
                                           : std::string_view("HTTP response");
        const int status = !cookie_capacity ? SALTS_ENOBUFS
                           : label.rfind("Invalid", 0) == 0 ? SALTS_EPROTO : SALTS_OK;
        cb.session->record(Kind::received, label, data, status, response->status_code, header_text);
      } catch (...) {
        cb.session->record(Kind::error, "Could not copy the HTTP response", {}, SALTS_ENOMEM);
      }
    };
    chttp_client_config config{};
    config.network = network_config(destination.tls, draft.use_cookies ? HttpCookies::header_limit
                                                                     : 0);
    config.request_capacity = 1;
    config.max_start_line_bytes = 4096;
    config.max_header_count = 32;
    config.max_header_bytes = kToolHeadersBytes;
    config.max_request_body_bytes = kToolBytes;
    config.max_response_body_bytes = kToolBytes;
    config.max_informational_responses = 8;
    chttp_async_client client{};
    const int init = chttp_async_client_init(&client, &config);
    if (init != SALTS_OK) {
      record(Kind::error, "Initialize HTTP client", {}, init);
      return;
    }
    if (!stopping_) {
      chttp_request token{};
      const chttp_request_timeouts limits{kTimeout, 0};
      const int submitted =
          chttp_async_client_submit_with_timeouts(&client, &options, &limits, &token);
      if (submitted != SALTS_OK) record(Kind::error, "Submit HTTP request", {}, submitted);
      else {
        record(Kind::completed, rpc ? "RPC call queued" : "HTTP request queued");
        while (!callback.complete && !stopping_) {
          size_t observed = 0;
          const int result = chttp_async_client_poll(&client, kTick, &observed);
          if (result != SALTS_OK) {
            record(Kind::error, "HTTP progress", {}, result);
            break;
          }
        }
        if (!callback.complete && stopping_) {
          const int canceled = chttp_async_request_cancel(&client, token);
          if (canceled != SALTS_OK) record(Kind::error, "Cancel HTTP request", {}, canceled);
        }
      }
    }
    drain_owner(client, chttp_async_client_stop, chttp_async_client_destroy);
  }

  void ToolSession::tcp(const ToolDraft &draft) {
    const auto destination = endpoint(draft.endpoint, false);
    cnet_client client{};
    const auto config = network_config(false);
    const int init = cnet_client_init(&client, &config);
    if (init != SALTS_OK) {
      record(Kind::error, "Initialize CNet", {}, init);
      return;
    }
    struct Callback {
      ToolSession *session;
      bool terminal = false;
      std::string writing;
    } callback{this};
    cnet_connect_options options{};
    options.uri = destination.uri.c_str();
    options.observer.user = &callback;
    options.observer.on_state = [](void *context, cnet_connection, cnet_connection_state state,
                                   const cnet_error *error) noexcept {
      auto &cb = *static_cast<Callback *>(context);
      if (state == CNET_CONNECTION_CONNECTED) {
        cb.session->state_ = State::connected;
        cb.session->record(Kind::completed, "TCP connected");
      } else if (state == CNET_CONNECTION_CLOSED || state == CNET_CONNECTION_FAILED) {
        cb.terminal = true;
        if (error)
          cb.session->record(Kind::error, error->stage ? error->stage : "TCP connection", {},
                             error->status);
        else cb.session->record(Kind::closed, "TCP closed");
        if (!cb.writing.empty())
          cb.session->record(Kind::error, "Write ended before local completion", {},
                             SALTS_ECANCELED);
      }
    };
    options.observer.on_receive = [](void *context, cnet_connection,
                                     const cnet_receive_view *view) noexcept {
      auto &cb = *static_cast<Callback *>(context);
      if (view->size)
        cb.session->record(Kind::received, "TCP bytes",
                           {static_cast<const char *>(view->data), view->size});
    };
    options.observer.on_send = [](void *context, cnet_connection, size_t) noexcept {
      auto &cb = *static_cast<Callback *>(context);
      cb.session->record(Kind::transmitted, "TCP local write complete", cb.writing);
      cb.writing.clear();
      cb.session->sending_ = false;
    };
    cnet_connection connection{};
    const int connected = cnet_connect(&client, &options, &connection);
    if (connected != SALTS_OK) record(Kind::error, "Connect TCP", {}, connected);
    else
      while (!stopping_ && !callback.terminal) {
        if (auto next = take_send()) {
          callback.writing = std::move(*next);
          auto *buf = mem_get_buffer(mem_global(), callback.writing.size());
          int result = SALTS_ENOMEM;
          if (buf) {
            std::memcpy(mem_buffer_data(buf), callback.writing.data(), callback.writing.size());
            mem_set_used(buf, callback.writing.size());
            result = cnet_send_buffer(&client, connection, buf);
            mem_buffer_release(buf);
          }
          if (result != SALTS_OK) {
            record(Kind::error, "Send TCP", {}, result);
            callback.writing.clear();
            sending_ = false;
          }
        }
        size_t observed = 0;
        const int result = cnet_client_poll(&client, kTick, &observed);
        if (result != SALTS_OK) {
          record(Kind::error, "TCP progress", {}, result);
          break;
        }
      }
    drain_owner(client, cnet_client_stop, cnet_client_destroy);
  }

  void ToolSession::serial(const ToolDraft &draft) {
    if (draft.endpoint.size() < 4 || draft.endpoint.size() > 8 ||
        draft.endpoint.compare(0, 3, "COM") != 0 ||
        draft.endpoint.find_first_not_of("0123456789", 3) != std::string::npos || draft.baud <= 0 ||
        draft.bits < 5 || draft.bits > 8 || draft.parity < 0 || draft.parity > 4 ||
        draft.stop_bits < 1 || draft.stop_bits > 2 || draft.flow < 0 || draft.flow > 3)
      throw std::runtime_error("Enter a Windows COM port and valid UART settings.");
    serial_config_t config{};
    serial_config_default(&config);
    config.baudrate = draft.baud;
    config.bits = draft.bits;
    config.parity = static_cast<serial_parity_t>(draft.parity);
    config.stopbits = draft.stop_bits;
    config.flowcontrol = static_cast<serial_flowcontrol_t>(draft.flow);
    serial_t *raw = nullptr;
    const auto created = serial_create(&raw, &config);
    if (created != SALTS_SERIAL_OK) {
      record(Kind::error, serial_result_name(created), {}, created);
      return;
    }
    std::unique_ptr<serial_t, decltype(&serial_destroy)> port(raw, serial_destroy);
    const auto opened =
        serial_open(port.get(), draft.endpoint.c_str(), SALTS_SERIAL_MODE_READ_WRITE);
    if (opened != SALTS_SERIAL_OK) {
      record(Kind::error, serial_result_name(opened), {}, opened);
      return;
    }
    state_ = State::connected;
    record(Kind::completed, "Serial port open");
    char bytes[4096];
    while (!stopping_) {
      if (auto next = take_send()) {
        size_t count = 0;
        const auto result = serial_write(port.get(), next->data(), next->size(), 1000, &count);
        if (count) record(Kind::transmitted, "Serial bytes written", {next->data(), count});
        if (result != SALTS_SERIAL_OK || count != next->size())
          record(Kind::error,
                 result == SALTS_SERIAL_OK ? "Partial serial write" : serial_result_name(result),
                 {}, result == SALTS_SERIAL_OK ? SALTS_EIO : result);
        sending_ = false;
        if (result != SALTS_SERIAL_OK && result != SALTS_SERIAL_WOULD_BLOCK) break;
      }
      size_t count = 0;
      const auto result = serial_read(port.get(), bytes, sizeof(bytes), kTick, &count);
      if (count) record(Kind::received, "Serial bytes", {bytes, count});
      if (result != SALTS_SERIAL_OK && result != SALTS_SERIAL_WOULD_BLOCK) {
        record(Kind::error, serial_result_name(result), {}, result);
        break;
      }
    }
    state_ = State::closing;
    const auto closed = serial_close(port.get());
    if (closed == SALTS_SERIAL_OK) record(Kind::closed, "Serial port closed");
    else record(Kind::error, serial_result_name(closed), {}, closed);
  }
} // namespace flowie::app
