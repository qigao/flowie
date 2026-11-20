#include "connection_dialog.h"
#include "connection_resource.h"
#include "import_files.h"
#include <stdexcept>

namespace flowie::app {
  namespace {
    std::wstring text(HWND dialog, int id) {
      const auto control = GetDlgItem(dialog, id);
      std::wstring value(static_cast<size_t>(GetWindowTextLengthW(control)) + 1, L'\0');
      value.resize(GetWindowTextW(control, value.data(), static_cast<int>(value.size())));
      return value;
    }

    std::string utf8(std::wstring_view value) {
      if (value.empty()) return {};
      const auto length = static_cast<int>(value.size());
      const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length,
                                            nullptr, 0, nullptr, nullptr);
      if (!size) throw std::runtime_error("The field contains invalid Unicode.");
      std::string encoded(static_cast<size_t>(size), '\0');
      if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length, encoded.data(),
                               size, nullptr, nullptr))
        throw std::runtime_error("Could not encode the field.");
      return encoded;
    }

    void append(HWND dialog, int id, const wchar_t *label) {
      if (SendDlgItemMessageW(dialog, id, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label)) < 0)
        throw std::runtime_error("Could not populate the connection dialog.");
    }
  } // namespace

  ConnectionDialog::~ConnectionDialog() {
    if (!connection_.password.empty())
      SecureZeroMemory(connection_.password.data(), connection_.password.size());
  }

  INT_PTR ConnectionDialog::show(HWND owner, HINSTANCE instance) {
    return DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_NEW_CONNECTION), owner, dispatch,
                           reinterpret_cast<LPARAM>(this));
  }

  INT_PTR CALLBACK ConnectionDialog::dispatch(HWND dialog, UINT message, WPARAM wparam,
                                              LPARAM lparam) {
    if (message == WM_INITDIALOG) SetWindowLongPtrW(dialog, DWLP_USER, lparam);
    auto *self = reinterpret_cast<ConnectionDialog *>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (!self) return FALSE;
    try {
      return self->message(dialog, message, wparam, lparam);
    } catch (const std::exception &error) {
      MessageBoxA(dialog, error.what(), "MQTT Connection", MB_OK | MB_ICONERROR);
      EndDialog(dialog, IDCANCEL);
      return TRUE;
    }
  }

  INT_PTR ConnectionDialog::message(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_INITDIALOG:
      populate(dialog);
      SetFocus(
          GetDlgItem(dialog, selected_ >= 0 || initial_ ? IDC_BROKER_PASSWORD : IDC_BROKER_HOST));
      return FALSE;
    case WM_CLOSE:
      EndDialog(dialog, IDCANCEL);
      return TRUE;
    case WM_COMMAND:
      switch (LOWORD(wparam)) {
      case IDC_RECENT_CONNECTION:
        if (HIWORD(wparam) == CBN_SELCHANGE) select_recent(dialog);
        break;
      case IDC_BROKER_TRANSPORT:
        if (HIWORD(wparam) == CBN_SELCHANGE) enable_transport_fields(dialog);
        break;
      case IDOK:
      case IDC_OPEN_CONNECTION_TAB:
        submit(dialog, LOWORD(wparam));
        return TRUE;
      case IDCANCEL:
        EndDialog(dialog, IDCANCEL);
        return TRUE;
      }
      break;
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
      const auto control = reinterpret_cast<HWND>(lparam);
      const auto ink =
          message == WM_CTLCOLORDLG || IsWindowEnabled(control) ? theme_.ink : theme_.disabled;
      return reinterpret_cast<INT_PTR>(color_brush(
          reinterpret_cast<HDC>(wparam),
          GetDlgCtrlID(control) == IDC_CONNECTION_ERROR ? theme_.red : ink, theme_.paper));
    }
    }
    return FALSE;
  }

  void ConnectionDialog::populate(HWND dialog) {
    const wchar_t *title = role_ == ClientRole::publisher    ? L"New MQTT Publisher"
                           : role_ == ClientRole::subscriber ? L"New MQTT Subscriber"
                                                             : L"New MQTT Client";
    SetWindowTextW(dialog, title);
    for (int id : {IDC_BROKER_PATH, IDC_BROKER_USER, IDC_BROKER_PASSWORD, IDC_BROKER_CA,
                   IDC_BROKER_CERT, IDC_BROKER_KEY})
      SendDlgItemMessageW(dialog, id, EM_SETLIMITTEXT, 4096, 0);
    SendDlgItemMessageW(dialog, IDC_BROKER_HOST, EM_SETLIMITTEXT, kBrokerHostLimit, 0);
    SendDlgItemMessageW(dialog, IDC_BROKER_PORT, EM_SETLIMITTEXT, 5, 0);
    append(dialog, IDC_RECENT_CONNECTION, L"Manual entry");
    for (const auto &entry : recent_)
      append(dialog, IDC_RECENT_CONNECTION, connection_label(entry).c_str());
    for (const auto *label : {L"TCP", L"TLS", L"WebSocket", L"Secure WebSocket"})
      append(dialog, IDC_BROKER_TRANSPORT, label);
    for (const auto *label : {L"MQTT 5", L"MQTT 3.1.1"})
      append(dialog, IDC_BROKER_PROTOCOL, label);
    SendDlgItemMessageW(dialog, IDC_RECENT_CONNECTION, CB_SETCURSEL, selected_ + 1, 0);
    select_recent(dialog);
    if (initial_) {
      const auto set = [&](int field, const std::string &value) {
        SetDlgItemTextW(dialog, field, utf8_to_wide(value).c_str());
      };
      set(IDC_BROKER_HOST, initial_->host);
      set(IDC_BROKER_PORT, std::to_string(initial_->port));
      set(IDC_BROKER_PATH, initial_->path);
      set(IDC_BROKER_USER, initial_->username);
      set(IDC_BROKER_CA, initial_->ca_file);
      set(IDC_BROKER_CERT, initial_->cert_file);
      set(IDC_BROKER_KEY, initial_->key_file);
      SendDlgItemMessageW(dialog, IDC_BROKER_TRANSPORT, CB_SETCURSEL, initial_->transport - 1, 0);
      SendDlgItemMessageW(dialog, IDC_BROKER_PROTOCOL, CB_SETCURSEL,
                          initial_->version == FLOWIE_MQTT_VERSION_5 ? 0 : 1, 0);
      enable_transport_fields(dialog);
    }
  }

  void ConnectionDialog::select_recent(HWND dialog) {
    const auto index = SendDlgItemMessageW(dialog, IDC_RECENT_CONNECTION, CB_GETCURSEL, 0, 0);
    const auto *entry = index > 0 ? &recent_.at(static_cast<size_t>(index - 1)) : nullptr;
    SetDlgItemTextW(dialog, IDC_BROKER_HOST, entry ? entry->host.c_str() : L"127.0.0.1");
    SetDlgItemTextW(dialog, IDC_BROKER_PORT,
                    entry ? std::to_wstring(entry->port).c_str() : L"1883");
    SendDlgItemMessageW(dialog, IDC_BROKER_TRANSPORT, CB_SETCURSEL,
                        entry ? entry->transport - 1 : 0, 0);
    SendDlgItemMessageW(dialog, IDC_BROKER_PROTOCOL, CB_SETCURSEL,
                        entry && entry->version == FLOWIE_MQTT_VERSION_3_1_1 ? 1 : 0, 0);
    SetDlgItemTextW(dialog, IDC_BROKER_PATH, L"/mqtt");
    for (int id : {IDC_BROKER_USER, IDC_BROKER_PASSWORD, IDC_BROKER_CA, IDC_BROKER_CERT,
                   IDC_BROKER_KEY, IDC_CONNECTION_ERROR})
      SetDlgItemTextW(dialog, id, L"");
    enable_transport_fields(dialog);
  }

  void ConnectionDialog::enable_transport_fields(HWND dialog) {
    const auto transport = SendDlgItemMessageW(dialog, IDC_BROKER_TRANSPORT, CB_GETCURSEL, 0, 0);
    EnableWindow(GetDlgItem(dialog, IDC_BROKER_PATH), transport >= 2);
    for (int id : {IDC_BROKER_CA, IDC_BROKER_CERT, IDC_BROKER_KEY})
      EnableWindow(GetDlgItem(dialog, id), transport == 1 || transport == 3);
  }

  void ConnectionDialog::submit(HWND dialog, int action) {
    int field = IDC_BROKER_HOST;
    try {
      const auto host = text(dialog, field);
      if (!valid_broker_host(host))
        throw std::runtime_error("Enter a broker host without a scheme or path.");
      connection_.host = utf8(host);
      connection_.port = read_port(text(dialog, field = IDC_BROKER_PORT));
      const auto transport = SendDlgItemMessageW(dialog, IDC_BROKER_TRANSPORT, CB_GETCURSEL, 0, 0);
      connection_.transport = static_cast<flowie_mqtt_client_transport_t>(transport + 1);
      connection_.version =
          SendDlgItemMessageW(dialog, IDC_BROKER_PROTOCOL, CB_GETCURSEL, 0, 0) == 0
              ? FLOWIE_MQTT_VERSION_5
              : FLOWIE_MQTT_VERSION_3_1_1;
      if (transport >= 2) {
        connection_.path = utf8(text(dialog, field = IDC_BROKER_PATH));
        if (connection_.path.empty() || connection_.path.front() != '/')
          throw std::runtime_error("WebSocket path must start with /.");
      } else connection_.path = "/mqtt";
      const bool tls = transport == 1 || transport == 3;
      connection_.ca_file = tls ? utf8(text(dialog, field = IDC_BROKER_CA)) : "";
      connection_.cert_file = tls ? utf8(text(dialog, field = IDC_BROKER_CERT)) : "";
      connection_.key_file = tls ? utf8(text(dialog, field = IDC_BROKER_KEY)) : "";
      if (connection_.cert_file.empty() != connection_.key_file.empty()) {
        field = connection_.cert_file.empty() ? IDC_BROKER_CERT : IDC_BROKER_KEY;
        throw std::runtime_error("Enter both the client certificate and key, or leave both blank.");
      }
      connection_.username = utf8(text(dialog, field = IDC_BROKER_USER));
      auto password = text(dialog, field = IDC_BROKER_PASSWORD);
      struct ErasePassword {
        std::wstring &value;
        ~ErasePassword() {
          if (!value.empty()) SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t));
        }
      } erase{password};
      connection_.password = utf8(password);
      SetDlgItemTextW(dialog, IDC_BROKER_PASSWORD, L"");
      EndDialog(dialog, action);
    } catch (const std::exception &error) {
      SetDlgItemTextA(dialog, IDC_CONNECTION_ERROR, error.what());
      SetFocus(GetDlgItem(dialog, field));
    }
  }
} // namespace flowie::app
