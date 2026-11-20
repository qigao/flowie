#include "main_window.h"
#include "ui/import_files.h"
#include <algorithm>
#include <core/error.h>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace flowie::app {
  namespace {
    struct ErasePassword {
      std::string &value;
      ~ErasePassword() {
        if (!value.empty()) SecureZeroMemory(value.data(), value.size());
      }
    };

    std::wstring wide(const std::string &input) { return utf8_to_wide(input); }
    std::wstring display(const std::string &input) {
      if (std::none_of(input.begin(), input.end(), [](unsigned char c) {
            return (c < 32 && c != '\r' && c != '\n' && c != '\t') || c == 127;
          })) {
        try {
          return wide(input);
        } catch (const std::runtime_error &) { /* Binary view below. */
        }
      }
      std::wostringstream out;
      out << L"[binary / hexadecimal]\r\n" << std::hex << std::setfill(L'0');
      for (size_t i = 0; i < input.size(); ++i) {
        out << std::setw(2) << static_cast<unsigned>(static_cast<unsigned char>(input[i]))
            << ((i + 1) % 16 ? L" " : L"\r\n");
      }
      return out.str();
    }
    std::wstring timestamp(const Event &event) {
      const auto value = std::chrono::system_clock::to_time_t(event.time);
      std::tm local{};
      localtime_s(&local, &value);
      wchar_t result[20];
      std::wcsftime(result, 20, L"%H:%M:%S", &local);
      return result;
    }
    std::wstring outcome(const Event &event) {
      if (event.kind == Kind::received) return L"RX";
      if (event.kind == Kind::transmitted) return L"TX queued";
      if (event.kind == Kind::closed) return event.status ? L"Close failed" : L"Closed";
      if (event.kind == Kind::error) return L"ERROR";
      return wide(operation_name(event.operation));
    }
    std::wstring result_text(const Event &event) {
      if (event.kind == Kind::received || event.kind == Kind::transmitted)
        return L"QoS " + std::to_wstring(event.qos) + (event.retain ? L" / retained" : L"");
      auto result = L"status " + std::to_wstring(event.status);
      if (event.status) result += L" (" + wide(error_code_string(event.status)) + L")";
      if (event.kind == Kind::completed) {
        result += L" / reason " + std::to_wstring(event.reason);
        for (unsigned char reason : event.reasons)
          result += L" / ACK " + std::to_wstring(reason);
        if (event.reason >= 0x80 || (event.operation == Operation::connect && event.reason != 0) ||
            std::any_of(event.reasons.begin(), event.reasons.end(),
                        [](unsigned char reason) { return reason >= 0x80; }))
          result += L" / broker rejected";
      }
      return result;
    }
  } // namespace
  int MainWindow::scale(int value) const { return MulDiv(value, dpi_, 96); }
  HWND MainWindow::make(Id id, const wchar_t *kind, const wchar_t *value, DWORD style) {
    const bool interactive = std::wcscmp(kind, L"STATIC") != 0;
    if (std::wcscmp(kind, L"BUTTON") == 0) style |= BS_NOTIFY;
    const auto window =
        ::CreateWindowExW(std::wcscmp(kind, L"EDIT") == 0 ? WS_EX_CLIENTEDGE : 0, kind, value,
                          WS_CHILD | WS_VISIBLE | (interactive ? WS_TABSTOP : 0) | style, 0, 0, 0,
                          0, control_parent(id), reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                          _Module.GetModuleInstance(), nullptr);
    if (!window) throw std::runtime_error("Could not create a window control");
    controls_[id] = window;
    ::SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    return window;
  }
  void MainWindow::field(Id id, const wchar_t *label, const wchar_t *value, bool secret) {
    make(static_cast<Id>(id + 1000), L"STATIC", label);
    auto edit = make(id, L"EDIT", value, ES_AUTOHSCROLL | (secret ? ES_PASSWORD : 0));
    ::SendMessageW(edit, EM_SETLIMITTEXT, 4096, 0);
  }
  void MainWindow::choice(Id id, const std::initializer_list<const wchar_t *> &choices) {
    auto combo = make(id, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL);
    for (auto value : choices)
      ::SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value));
    ::SendMessageW(combo, CB_SETCURSEL, 0, 0);
  }
  std::wstring MainWindow::text(int id) const {
    auto control = controls_.at(id);
    const int length = ::GetWindowTextLengthW(control);
    std::wstring result(static_cast<size_t>(length) + 1, L'\0');
    ::GetWindowTextW(control, result.data(), length + 1);
    result.resize(length);
    return result;
  }
  std::string MainWindow::utf8(int id) const {
    const auto input = text(id);
    if (input.empty()) return {};
    const int size =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
                            static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (!size) throw std::runtime_error("Input contains invalid Unicode");
    std::string result(size, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
                             static_cast<int>(input.size()), result.data(), size, nullptr, nullptr))
      throw std::runtime_error("UTF-8 conversion failed");
    return result;
  }
  int MainWindow::selection(int id) const {
    return static_cast<int>(::SendMessageW(controls_.at(id), CB_GETCURSEL, 0, 0));
  }
  void MainWindow::report(const std::wstring &message) {
    ::SetWindowTextW(controls_.at(status), message.c_str());
  }
  void MainWindow::checked(int result, const wchar_t *operation) {
    report(std::wstring(operation) + (result == SALTS_OK
                                          ? L" queued"
                                          : L" failed: " + wide(error_code_string(result)) + L" (" +
                                                std::to_wstring(result) + L")"));
  }
  LRESULT MainWindow::OnCreate(UINT, WPARAM, LPARAM, BOOL &) {
    try {
      HDC dc = GetDC();
      dpi_ = GetDeviceCaps(dc, LOGPIXELSX);
      ReleaseDC(dc);
      NONCLIENTMETRICSW metrics{sizeof(metrics)};
      if (!::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
        throw std::runtime_error("Could not read system UI font");
      font_ = ::CreateFontIndirectW(&metrics.lfMessageFont);
      LOGFONTW heading = metrics.lfMessageFont;
      heading.lfWeight = FW_SEMIBOLD;
      heading_font_ = ::CreateFontIndirectW(&heading);
      LOGFONTW mono = metrics.lfMessageFont;
      ::lstrcpynW(mono.lfFaceName, L"Consolas", LF_FACESIZE);
      mono_font_ = ::CreateFontIndirectW(&mono);
      if (!font_ || !heading_font_ || !mono_font_)
        throw std::runtime_error("Could not create UI fonts");
      theme_ = current_theme();
      create_panels();
      create_request_tabs();
      visible_records_.reserve(kHistoryRecords);
      field(host, L"&Broker host", L"127.0.0.1");
      field(port, L"&Port", L"1883");
      ::SendMessageW(controls_.at(host), EM_SETLIMITTEXT, kBrokerHostLimit, 0);
      ::SendMessageW(controls_.at(port), EM_SETLIMITTEXT, 5, 0);
      make(static_cast<Id>(transport + 1000), L"STATIC", L"Transport");
      choice(transport, {L"TCP", L"TLS", L"WS", L"WSS"});
      make(static_cast<Id>(protocol + 1000), L"STATIC", L"MQTT version");
      choice(protocol, {L"MQTT 5", L"MQTT 3.1.1"});
      field(path, L"WebSocket path", L"/mqtt");
      const auto identifier = identifier_.empty()
                                  ? L"workbench-" + std::to_wstring(GetCurrentProcessId())
                                  : identifier_;
      field(client_id, L"Client &ID", identifier.c_str());
      field(username, L"&Username", L"");
      field(password, L"Pass&word", L"", true);
      field(ca, L"CA file (TLS / WSS)", L"");
      field(cert, L"Client certificate (optional)", L"");
      field(key, L"Client private key (optional)", L"");
      make(connect_button, L"BUTTON", L"&Connect");
      make(disconnect_button, L"BUTTON", L"&Disconnect");
      make(subscription_title, L"STATIC", L"Active subscriptions");
      field(filter, L"Topic &filter", L"workbench/#");
      choice(sub_qos, {L"QoS 0", L"QoS 1", L"QoS 2"});
      make(subscribe_button, L"BUTTON", L"&Subscribe");
      make(unsubscribe_button, L"BUTTON", L"&Unsubscribe");
      make(subscriptions, L"LISTBOX", L"", LBS_NOTIFY | WS_BORDER | WS_VSCROLL);
      make(static_cast<Id>(saved_topic_picker + 1000), L"STATIC", L"Saved &topic");
      choice(saved_topic_picker, {});
      field(topic, L"&Topic", L"workbench/test");
      choice(pub_qos, {L"QoS 0", L"QoS 1", L"QoS 2"});
      choice(mode, {L"TXT / UTF-8", L"JSON"});
      make(retain, L"BUTTON", L"&Retain", BS_AUTOCHECKBOX);
      make(payload, L"EDIT", L"Hello from Flowie",
           ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_VSCROLL |
               WS_HSCROLL);
      ::SendMessageW(controls_.at(payload), WM_SETFONT, reinterpret_cast<WPARAM>(mono_font_), TRUE);
      ::SendMessageW(controls_.at(payload), EM_SETLIMITTEXT, kPayloadBytes, 0);
      make(publish_button, L"BUTTON", L"&Publish");
      make(format_button, L"BUTTON", L"Format JSON");
      make(validate_button, L"BUTTON", L"Validate JSON");
      make(history_title, L"STATIC", L"  Messages");
      make(static_cast<Id>(message_filter + 1000), L"STATIC", L"&Show:", SS_CENTERIMAGE);
      choice(message_filter, {L"All events", L"Received", L"Sent", L"Operations"});
      make(ping_button, L"BUTTON", L"Ping");
      make(clear_button, L"BUTTON", L"Clear history");
      auto list = make(records, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS);
      ::SendMessageW(list, CCM_SETVERSION, 5, 0);
      ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                                                  LVS_EX_LABELTIP);
      const wchar_t *columns[] = {L"Time", L"Event", L"Topic / operation", L"Bytes", L"Result"};
      const int widths[] = {75, 100, 220, 65, 230};
      for (int i = 0; i < 5; ++i) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = const_cast<wchar_t *>(columns[i]);
        column.cx = scale(widths[i]);
        ListView_InsertColumn(list, i, &column);
      }
      make(detail_title, L"STATIC", L"  Original payload");
      make(details, L"EDIT", L"Select a message to inspect its payload.",
           ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_VSCROLL | WS_HSCROLL);
      ::SendMessageW(controls_.at(details), WM_SETFONT, reinterpret_cast<WPARAM>(mono_font_), TRUE);
      ::SendMessageW(controls_.at(details), EM_SETLIMITTEXT, 4 * 1024 * 1024, 0);
      make(status, L"STATIC", L"Disconnected. Choose a broker, then Connect.");
      make(counters, L"STATIC", L"RX 0   TX 0   Removed 0");
      for (int id : {subscription_title, history_title, detail_title})
        ::SendMessageW(controls_.at(id), WM_SETFONT, reinterpret_cast<WPARAM>(heading_font_), TRUE);
      panels_ready_ = true;
      layout();
      editor_.SetSplitterPosPct(54);
      results_.SetSplitterPosPct(55);
      refresh_theme();
      if (!SetTimer(1, 50)) throw std::runtime_error("Could not start UI event timer");
      enable();
      layout();
      return 0;
    } catch (const std::exception &error) {
      ::MessageBoxW(m_hWnd, wide(error.what()).c_str(), L"MQTT Workbench", MB_OK | MB_ICONERROR);
      return -1;
    }
  }
  void MainWindow::enable() {
    const auto state = session_.state();
    const auto shown = controls_.at(state == State::idle ? connect_button : disconnect_button);
    const auto hidden = controls_.at(state == State::idle ? disconnect_button : connect_button);
    const bool action_focused = ::GetFocus() == hidden;
    const bool idle = state == State::idle && !closing_;
    for (int id : {host, port, transport, protocol, path, client_id, username, password, ca, cert,
                   key, connect_button})
      ::EnableWindow(controls_.at(id), idle);
    const bool tls = selection(transport) == 1 || selection(transport) == 3;
    for (int id : {ca, cert, key})
      ::EnableWindow(controls_.at(id), idle && tls);
    ::EnableWindow(controls_.at(path), idle && selection(transport) >= 2);
    ::EnableWindow(controls_.at(disconnect_button),
                   state != State::idle && state != State::closing);
    ::SetWindowTextW(controls_.at(disconnect_button),
                     state == State::close_failed ? L"Retry close" : L"&Disconnect");
    if (::GetWindowLongPtrW(hidden, GWL_STYLE) & WS_VISIBLE) {
      ::ShowWindow(hidden, SW_HIDE);
      ::ShowWindow(shown, SW_SHOWNA);
      if (action_focused && ::IsWindowEnabled(shown)) ::SetFocus(shown);
    }
    for (int id : {publish_button, subscribe_button, unsubscribe_button, ping_button})
      ::EnableWindow(controls_.at(id), session_.available() && !closing_);
    if (role_ == ClientRole::subscriber) ::EnableWindow(controls_.at(publish_button), FALSE);
    if (role_ == ClientRole::publisher) {
      ::EnableWindow(controls_.at(subscribe_button), FALSE);
      ::EnableWindow(controls_.at(unsubscribe_button), FALSE);
    }
  }
  void MainWindow::apply_connection(Connection &connection, bool connect_now) {
    const auto set = [&](Id id, const std::string &value) {
      ::SetWindowTextW(controls_.at(id), wide(value).c_str());
    };
    set(host, connection.host);
    set(port, std::to_string(connection.port));
    set(path, connection.path);
    set(username, connection.username);
    set(ca, connection.ca_file);
    set(cert, connection.cert_file);
    set(key, connection.key_file);
    ::SendMessageW(controls_.at(transport), CB_SETCURSEL, connection.transport - 1, 0);
    ::SendMessageW(controls_.at(protocol), CB_SETCURSEL,
                   connection.version == FLOWIE_MQTT_VERSION_5 ? 0 : 1, 0);
    if (connect_now) {
      connection.client_id = utf8(client_id);
      connect(connection);
    } else {
      auto secret = wide(connection.password);
      ::SetWindowTextW(controls_.at(password), secret.c_str());
      if (!secret.empty()) SecureZeroMemory(secret.data(), secret.size() * sizeof(wchar_t));
    }
    enable();
    layout_panels();
  }

  bool MainWindow::action_enabled(Id id) const {
    return !closing_ && ::IsWindowEnabled(controls_.at(id));
  }

  RequestCommand MainWindow::request_command(RequestAction action) const {
    switch (action) {
    case RequestAction::open:
      return {L"Connect", state() == State::idle && action_enabled(connect_button)};
    case RequestAction::stop:
      return {state() == State::close_failed ? L"Retry Close" : L"Disconnect",
              action_enabled(disconnect_button)};
    case RequestAction::send:
      return can_publish() ? RequestCommand{L"Publish", action_enabled(publish_button)}
                           : RequestCommand{};
    case RequestAction::sample:
      return can_publish() ? RequestCommand{L"Load Sample...", true} : RequestCommand{};
    case RequestAction::validate_json:
      return can_publish() ? RequestCommand{L"Validate JSON", action_enabled(validate_button)}
                           : RequestCommand{};
    case RequestAction::format_json:
      return can_publish() ? RequestCommand{L"Format JSON", action_enabled(format_button)}
                           : RequestCommand{};
    default:
      return {};
    }
  }

  void MainWindow::invoke_request(RequestAction action) {
    if (!request_command(action).enabled) return;
    switch (action) {
    case RequestAction::open:
      SendMessage(WM_COMMAND, MAKEWPARAM(connect_button, BN_CLICKED));
      break;
    case RequestAction::stop:
      SendMessage(WM_COMMAND, MAKEWPARAM(disconnect_button, BN_CLICKED));
      break;
    case RequestAction::send:
      SendMessage(WM_COMMAND, MAKEWPARAM(publish_button, BN_CLICKED));
      break;
    case RequestAction::validate_json:
      SendMessage(WM_COMMAND, MAKEWPARAM(validate_button, BN_CLICKED));
      break;
    case RequestAction::format_json:
      SendMessage(WM_COMMAND, MAKEWPARAM(format_button, BN_CLICKED));
      break;
    default:
      break;
    }
  }

  void MainWindow::apply_sample(const MessageSample &sample) {
    if (!can_publish())
      throw std::runtime_error("Select a Publisher or Client tab to load a sample.");
    const auto body = wide(sample.payload);
    const auto sample_topic = sample.options ? wide(sample.options->topic) : std::wstring{};
    if (!::SetWindowTextW(controls_.at(payload), body.c_str()))
      throw std::runtime_error("Could not load the sample into the editor.");
    ::SendMessageW(controls_.at(mode), CB_SETCURSEL, sample.json ? 1 : 0, 0);
    if (sample.options) {
      ::SetWindowTextW(controls_.at(topic), sample_topic.c_str());
      ::SendMessageW(controls_.at(pub_qos), CB_SETCURSEL, sample.options->qos, 0);
      ::SendMessageW(controls_.at(retain), BM_SETCHECK,
                     sample.options->retain ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    if (task_layout_ == TaskLayout::output) set_task_layout(TaskLayout::editor);
    else select_request_page(RequestPage::message);
    report(L"Sample loaded into the publish draft.");
    ::SetFocus(controls_.at(payload));
  }

  void MainWindow::connect(const Connection &connection) {
    RecentConnection endpoint{role_, wide(connection.host), connection.port, connection.transport,
                              connection.version};
    if (!valid_broker_host(endpoint.host))
      throw std::runtime_error("Enter a broker host without a scheme or path.");
    const auto result = session_.connect(connection);
    if (result == SALTS_OK) pending_connection_ = std::move(endpoint);
    checked(result, L"Connect");
  }

  Connection MainWindow::connection_fields() const {
    Connection connection;
    connection.host = utf8(host);
    connection.port = read_port(text(port));
    connection.transport = static_cast<flowie_mqtt_client_transport_t>(selection(transport) + 1);
    connection.version =
        selection(protocol) == 0 ? FLOWIE_MQTT_VERSION_5 : FLOWIE_MQTT_VERSION_3_1_1;
    if (selection(transport) >= 2) connection.path = utf8(path);
    const bool tls = selection(transport) == 1 || selection(transport) == 3;
    if (tls) {
      connection.ca_file = utf8(ca);
      connection.cert_file = utf8(cert);
      connection.key_file = utf8(key);
    }
    return connection;
  }

  SavedRequest MainWindow::capture_request() const {
    SavedRequest request;
    request.profile = {role_, connection_fields()};
    auto &connection = request.profile.connection;
    const auto query = connection.path.find_first_of("?#");
    if (query != std::string::npos) connection.path.resize(query);
    for (auto *file : {&connection.ca_file, &connection.cert_file, &connection.key_file})
      if (!file->empty())
        *file = std::filesystem::absolute(std::filesystem::u8path(*file)).u8string();
    if (role_ != ClientRole::publisher) {
      std::map<std::string, int> topics;
      for (const auto &preset : saved_topics_)
        topics.emplace(preset.filter, preset.qos);
      for (const auto &[name, qos] : subscriptions_)
        topics[name] = qos;
      const auto draft = utf8(filter);
      if (!draft.empty()) topics[draft] = selection(sub_qos);
      if (topics.size() > kSavedTopicLimit)
        throw std::runtime_error("This request has more than 128 distinct topics.");
      for (const auto &[name, qos] : topics)
        request.topics.push_back({name, qos});
    }
    if (can_publish())
      request.message = MessageSample{
          utf8(payload), selection(mode) == 1,
          MessageOptions{utf8(topic), selection(pub_qos),
                         ::SendMessageW(controls_.at(retain), BM_GETCHECK, 0, 0) == BST_CHECKED}};
    return request;
  }

  void MainWindow::restore_request(SavedRequest request) {
    apply_connection(request.profile.connection, false);
    if (request.message) apply_sample(*request.message);
    saved_topics_ = std::move(request.topics);
    const auto picker = controls_.at(saved_topic_picker);
    for (const auto &entry : saved_topics_) {
      const auto label = wide(entry.filter) + L"  [QoS " + std::to_wstring(entry.qos) + L"]";
      if (::SendMessageW(picker, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str())) < 0)
        throw std::runtime_error("Could not load saved topics.");
    }
    if (!saved_topics_.empty()) {
      ::SendMessageW(picker, CB_SETCURSEL, 0, 0);
      select_saved_topic();
    }
    layout_panels();
    report(L"Request opened. Connect when ready.");
    ::SetFocus(controls_.at(host));
  }

  void MainWindow::select_saved_topic() {
    const auto index = selection(saved_topic_picker);
    if (index < 0 || static_cast<size_t>(index) >= saved_topics_.size()) return;
    const auto &entry = saved_topics_[index];
    ::SetWindowTextW(controls_.at(filter), wide(entry.filter).c_str());
    ::SendMessageW(controls_.at(sub_qos), CB_SETCURSEL, entry.qos, 0);
  }

  void MainWindow::command(int id) {
    if ((id == publish_button && role_ == ClientRole::subscriber) ||
        ((id == subscribe_button || id == unsubscribe_button) && role_ == ClientRole::publisher)) {
      report(L"This operation is disabled for this client role.");
      return;
    }
    switch (id) {
    case connect_button: {
      auto connection = connection_fields();
      connection.client_id = utf8(client_id);
      connection.username = utf8(username);
      connection.password = utf8(password);
      ErasePassword erase{connection.password};
      connect(connection);
      break;
    }
    case disconnect_button:
      if (session_.available()) checked(session_.disconnect(), L"Disconnect");
      else {
        session_.close();
        report(L"Closing connection...");
      }
      break;
    case subscribe_button:
    case unsubscribe_button: {
      const auto value = utf8(filter);
      const auto qos = selection(sub_qos);
      if (id == subscribe_button && !subscriptions_.count(value) &&
          subscriptions_.size() == kSavedTopicLimit) {
        report(L"Subscription limit reached (128). Unsubscribe first.");
        break;
      }
      const auto result = id == subscribe_button
                              ? session_.subscribe(value, static_cast<uint8_t>(qos))
                              : session_.unsubscribe(value);
      if (result == SALTS_OK) {
        pending_filter_ = value;
        pending_qos_ = qos;
      }
      checked(result, id == subscribe_button ? L"Subscribe" : L"Unsubscribe");
      break;
    }
    case publish_button: {
      const auto data = utf8(payload), name = utf8(topic);
      std::string error;
      if (selection(mode) == 1 && !format_json(data, nullptr, error)) {
        report(wide(error));
        break;
      }
      const auto qos = static_cast<uint8_t>(selection(pub_qos));
      const bool retained = ::SendMessageW(controls_.at(retain), BM_GETCHECK, 0, 0) == BST_CHECKED;
      const auto result = session_.publish(name, data, qos, retained);
      checked(result, L"Publish");
      if (result == SALTS_OK) {
        Event event;
        event.kind = Kind::transmitted;
        event.topic = name;
        event.payload = data;
        event.qos = qos;
        event.retain = retained;
        history_.append(std::move(event));
        ++transmitted_;
        refresh();
      }
      break;
    }
    case validate_button:
    case format_button: {
      std::string formatted, error;
      if (!format_json(utf8(payload), id == format_button ? &formatted : nullptr, error))
        report(wide(error));
      else {
        if (id == format_button) ::SetWindowTextW(controls_.at(payload), wide(formatted).c_str());
        report(id == format_button ? L"JSON formatted. Original message history is unchanged."
                                   : L"Valid JSON.");
      }
      break;
    }
    case clear_button:
      history_.clear();
      received_ = 0;
      transmitted_ = 0;
      refresh();
      break;
    case ping_button:
      checked(session_.ping(), L"Ping");
      break;
    }
    enable();
  }
  void MainWindow::refresh() {
    auto list = controls_.at(records);
    ::SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    visible_records_.clear();
    ListView_DeleteAllItems(list);
    const int filter_index = selection(message_filter);
    int index = 0;
    for (size_t record = 0; record < history_.records().size(); ++record) {
      const auto &event = history_.records()[record];
      if (!show_event(event.kind, filter_index)) continue;
      visible_records_.push_back(record);
      auto time = timestamp(event);
      LVITEMW item{};
      item.mask = LVIF_TEXT;
      item.iItem = index;
      item.pszText = time.data();
      ListView_InsertItem(list, &item);
      std::wstring fields[] = {outcome(event),
                               event.topic.empty() ? wide(operation_name(event.operation))
                                                   : display(event.topic),
                               std::to_wstring(event.payload.size()), result_text(event)};
      for (int column = 1; column < 5; ++column)
        ListView_SetItemText(list, index, column, fields[column - 1].data());
      ++index;
    }
    if (index) {
      ListView_SetItemState(list, index - 1, LVIS_SELECTED | LVIS_FOCUSED,
                            LVIS_SELECTED | LVIS_FOCUSED);
      ListView_EnsureVisible(list, index - 1, FALSE);
    }
    ::SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    ::InvalidateRect(list, nullptr, TRUE);
    const auto value = L"RX " + std::to_wstring(received_) + L"   TX " +
                       std::to_wstring(transmitted_) + L"   Removed " +
                       std::to_wstring(history_.evicted());
    ::SetWindowTextW(controls_.at(counters), value.c_str());
    show_detail();
  }
  bool MainWindow::show_event(Kind kind, int filter_index) const {
    switch (filter_index) {
    case 1:
      return kind == Kind::received;
    case 2:
      return kind == Kind::transmitted;
    case 3:
      return kind != Kind::received && kind != Kind::transmitted;
    default:
      return true;
    }
  }
  void MainWindow::show_detail() {
    const auto index = ListView_GetNextItem(controls_.at(records), -1, LVNI_SELECTED);
    if (index < 0 || static_cast<size_t>(index) >= visible_records_.size() ||
        visible_records_[index] >= history_.records().size()) {
      ::SetWindowTextW(controls_.at(details), L"Select a message to inspect its payload.");
      return;
    }
    const auto &event = history_.records()[visible_records_[index]];
    const auto value = outcome(event) + L"  " + result_text(event) + L"\r\n" +
                       display(event.topic) + L"\r\n\r\n" + display(event.payload);
    ::SetWindowTextW(controls_.at(details), value.c_str());
  }
  LRESULT MainWindow::OnTimer(UINT, WPARAM, LPARAM, BOOL &) {
    try {
      auto events = session_.poll();
      for (auto &event : events) {
        if (event.kind == Kind::completed && event.operation == Operation::connect &&
            pending_connection_) {
          if (event.status == SALTS_OK && event.reason == 0 && !closing_ && remember_)
            remember_(*pending_connection_);
          pending_connection_.reset();
        }
        if (event.kind == Kind::completed && (event.operation == Operation::subscribe ||
                                              event.operation == Operation::unsubscribe)) {
          event.topic = pending_filter_;
          const bool accepted = event.status == SALTS_OK && event.reason < 0x80 &&
                                std::all_of(
                                    event.reasons.begin(), event.reasons.end(),
                                    [](unsigned char code) { return code < 0x80; });
          if (accepted) {
            if (event.operation == Operation::subscribe)
              subscriptions_[pending_filter_] =
                  event.reasons.empty() ? pending_qos_
                                        : static_cast<unsigned char>(event.reasons.front());
            else subscriptions_.erase(pending_filter_);
            ::SendMessageW(controls_.at(subscriptions), LB_RESETCONTENT, 0, 0);
            for (const auto &row : subscriptions_) {
              const auto value = wide(row.first) + L"  [QoS " + std::to_wstring(row.second) + L"]";
              ::SendMessageW(controls_.at(subscriptions), LB_ADDSTRING, 0,
                             reinterpret_cast<LPARAM>(value.c_str()));
            }
          }
          pending_filter_.clear();
        }
        if (event.kind == Kind::received) ++received_;
        else report(outcome(event) + L": " + result_text(event));
        history_.append(std::move(event));
      }
      if (!events.empty()) refresh();
      if (session_.state() != shown_state_) {
        shown_state_ = session_.state();
        if (shown_state_ == State::connected) report(L"Connected. Subscribe or publish a message.");
        if (shown_state_ == State::idle) {
          pending_connection_.reset();
          subscriptions_.clear();
          ::SendMessageW(controls_.at(subscriptions), LB_RESETCONTENT, 0, 0);
        }
        enable();
      } else enable();
      if (closing_ && session_.state() == State::idle) DestroyWindow();
    } catch (const std::exception &error) {
      report(wide(error.what()));
      session_.close();
    }
    return 0;
  }
  LRESULT MainWindow::OnCommand(UINT, WPARAM wparam, LPARAM, BOOL &) {
    try {
      if (HIWORD(wparam) == BN_CLICKED) command(LOWORD(wparam));
      else if (LOWORD(wparam) == transport && HIWORD(wparam) == CBN_SELCHANGE) {
        enable();
        layout_panels();
      } else if (LOWORD(wparam) == message_filter && HIWORD(wparam) == CBN_SELCHANGE) {
        refresh();
      } else if (LOWORD(wparam) == saved_topic_picker && HIWORD(wparam) == CBN_SELCHANGE) {
        select_saved_topic();
      } else if (LOWORD(wparam) == subscriptions && HIWORD(wparam) == LBN_SELCHANGE) {
        const auto selected = ::SendMessageW(controls_.at(subscriptions), LB_GETCURSEL, 0, 0);
        if (selected >= 0 && static_cast<size_t>(selected) < subscriptions_.size()) {
          auto row = subscriptions_.begin();
          std::advance(row, selected);
          ::SetWindowTextW(controls_.at(filter), wide(row->first).c_str());
          ::SendMessageW(controls_.at(sub_qos), CB_SETCURSEL, row->second, 0);
        }
      }
    } catch (const std::exception &error) {
      report(wide(error.what()));
    }
    return 0;
  }
  LRESULT MainWindow::OnNotify(UINT, WPARAM, LPARAM lparam, BOOL &) {
    const auto *notification = reinterpret_cast<NMHDR *>(lparam);
    if (notification->idFrom == request_tabs && notification->code == TCN_SELCHANGE) {
      const auto index = TabCtrl_GetCurSel(controls_.at(request_tabs));
      if (index >= 0 && static_cast<size_t>(index) < request_pages_.size())
        select_request_page(request_pages_[index]);
      return 0;
    }
    if (notification->code == NM_CUSTOMDRAW) {
      switch (notification->idFrom) {
      case connect_button:
      case disconnect_button:
      case subscribe_button:
      case unsubscribe_button:
      case publish_button:
      case format_button:
      case validate_button:
      case ping_button:
      case clear_button:
        return draw_form_button(*reinterpret_cast<NMCUSTOMDRAW *>(lparam), theme_, dpi_,
                                notification->idFrom == connect_button ||
                                    notification->idFrom == publish_button);
      }
    }
    if (notification->idFrom == records && notification->code == NM_CUSTOMDRAW && !theme_.system) {
      auto &draw = *reinterpret_cast<NMLVCUSTOMDRAW *>(lparam);
      if (draw.nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
      if (draw.nmcd.dwDrawStage == CDDS_ITEMPREPAINT) return CDRF_NOTIFYSUBITEMDRAW;
      if (draw.nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM) &&
          draw.nmcd.dwItemSpec < visible_records_.size() &&
          !(ListView_GetItemState(controls_.at(records), static_cast<int>(draw.nmcd.dwItemSpec),
                                  LVIS_SELECTED) &
            LVIS_SELECTED)) {
        draw.clrText = theme_.ink;
        if (draw.iSubItem == 1) {
          const auto &event = history_.records()[visible_records_[draw.nmcd.dwItemSpec]];
          const bool rejected = event.status != 0 || event.reason >= 0x80 ||
                                (event.operation == Operation::connect && event.reason != 0) ||
                                std::any_of(event.reasons.begin(), event.reasons.end(),
                                            [](unsigned char code) { return code >= 0x80; });
          draw.clrText = rejected                          ? theme_.red
                         : event.kind == Kind::received    ? theme_.green
                         : event.kind == Kind::transmitted ? theme_.blue
                                                           : theme_.muted;
        }
        // Each subitem resets its color; native report views require NEWFONT for color changes too.
        return CDRF_NEWFONT;
      }
      return CDRF_DODEFAULT;
    }

    if (notification->idFrom == records && notification->code == LVN_ITEMCHANGED) {
      try {
        show_detail();
      } catch (const std::exception &error) {
        report(wide(error.what()));
      }
    }
    return 0;
  }
  LRESULT MainWindow::OnSize(UINT, WPARAM wparam, LPARAM, BOOL &) {
    if (wparam != SIZE_MINIMIZED && !controls_.empty()) layout();
    return 0;
  }
  LRESULT MainWindow::OnMinMax(UINT, WPARAM, LPARAM lparam, BOOL &) {
    auto &info = *reinterpret_cast<MINMAXINFO *>(lparam);
    info.ptMinTrackSize = {scale(1050), scale(840)};
    return 0;
  }
  LRESULT MainWindow::OnClose(UINT, WPARAM, LPARAM, BOOL &) {
    closing_ = true;
    ::SetWindowTextW(controls_.at(password), L"");
    session_.close();
    enable();
    if (session_.state() == State::idle) DestroyWindow();
    else report(L"Closing: waiting for client I/O and callbacks to drain...");
    return 0;
  }
  LRESULT MainWindow::OnDestroy(UINT, WPARAM, LPARAM, BOOL &) {
    KillTimer(1);
    DeleteObject(font_);
    DeleteObject(heading_font_);
    DeleteObject(mono_font_);
    if (!(GetWindowLongPtr(GWL_STYLE) & WS_CHILD)) PostQuitMessage(0);
    return 0;
  }
  bool MainWindow::translate(MSG *message) {
    if (message->message == WM_KEYDOWN && GetKeyState(VK_CONTROL) < 0) {
      try {
        if (message->wParam == VK_RETURN) {
          if (session_.available() && !closing_) command(publish_button);
          return true;
        }
        if (message->wParam == 'L') {
          ::SetFocus(controls_.at(host));
          return true;
        }
        if (message->wParam == 'D') {
          command(disconnect_button);
          return true;
        }
      } catch (const std::exception &error) {
        report(wide(error.what()));
        return true;
      }
    }
    return ::IsDialogMessageW(m_hWnd, message) != FALSE;
  }
} // namespace flowie::app
