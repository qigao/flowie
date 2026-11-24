#include "main_protocol_window.h"
#include <algorithm>
#include <core/error.h>
#include <serial.h>
#include <stdexcept>

namespace flowie::app {
  namespace {
    std::wstring read_wide(HWND control) {
      if (!control) return {};
      std::wstring wide(static_cast<size_t>(GetWindowTextLengthW(control)) + 1, L'\0');
      wide.resize(GetWindowTextW(control, wide.data(), static_cast<int>(wide.size())));
      return wide;
    }
    std::string read_text(HWND control) {
      const auto wide = read_wide(control);
      if (wide.empty()) return {};
      const int length = static_cast<int>(wide.size());
      const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), length,
                                            nullptr, 0, nullptr, nullptr);
      if (!bytes) throw std::runtime_error("Invalid Unicode input.");
      std::string text(static_cast<size_t>(bytes), '\0');
      if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), length, text.data(),
                              bytes, nullptr, nullptr) != bytes)
        throw std::runtime_error("Could not encode the input.");
      return text;
    }
    std::wstring show_bytes(std::string_view bytes) {
      if (std::none_of(bytes.begin(), bytes.end(), [](unsigned char c) {
            return c < 32 && c != '\t' && c != '\r' && c != '\n';
          })) {
        try {
          return utf8_to_wide(bytes);
        } catch (const std::runtime_error &) {
        }
      }
      constexpr wchar_t digits[] = L"0123456789ABCDEF";
      std::wstring output = L"[Hexadecimal]\r\n";
      size_t count = 0;
      for (const unsigned char c : bytes) {
        output += digits[c >> 4];
        output += digits[c & 15];
        output += ++count % 16 ? L" " : L"\r\n";
      }
      return output;
    }
    std::wstring event_line(const Event &event) {
      const auto time = std::chrono::system_clock::to_time_t(event.time);
      std::tm local{};
      wchar_t stamp[12]{};
      localtime_s(&local, &time);
      std::wcsftime(stamp, std::size(stamp), L"%H:%M:%S", &local);
      auto line = std::wstring(stamp) + L"  " + utf8_to_wide(event.topic);
      if (event.reason) line += L"  HTTP " + std::to_wstring(event.reason);
      if (event.status) line += L"  (" + std::to_wstring(event.status) + L")";
      return line;
    }
    int selected(HWND combo) { return static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0)); }
    constexpr DWORD kMultiline = ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL;

    std::wstring windows_lines(std::wstring_view text) {
      std::wstring lines;
      lines.reserve(text.size());
      for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\n' && (i == 0 || text[i - 1] != L'\r')) lines += L'\r';
        lines += text[i];
      }
      return lines;
    }

    void check_json(ToolKind kind, const std::string &payload, std::string *formatted) {
      std::string error;
      if (!format_json(payload, formatted, error)) throw std::runtime_error(error);
      if (kind == ToolKind::rpc) {
        const auto start = payload.find_first_not_of(" \t\r\n");
        if (start == payload.npos || (payload[start] != '{' && payload[start] != '['))
          throw std::runtime_error("JSON-RPC params must be a JSON object or array.");
      }
    }
  } // namespace

  HWND ProtocolWindow::make(Id id, HWND parent, const wchar_t *kind, const wchar_t *value,
                            DWORD style, bool focusable) {
    const auto edit = std::wcscmp(kind, L"EDIT") == 0;
    const auto button = std::wcscmp(kind, L"BUTTON") == 0;
    auto control = ::CreateWindowExW(
        edit ? WS_EX_CLIENTEDGE : 0, kind, value,
        WS_CHILD | WS_VISIBLE | (focusable ? WS_TABSTOP : 0) | (button ? BS_NOTIFY : 0) | style, 0,
        0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        _Module.GetModuleInstance(), nullptr);
    if (!control) throw std::runtime_error("Could not create the protocol request controls.");
    controls_[id - target] = control;
    const bool monospace = id == body || id == detail || id == headers || id == query_text ||
                           id == response_body || id == response_headers;
    ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(monospace ? mono_ : font_), TRUE);
    return control;
  }
  void ProtocolWindow::choose(Id id, std::initializer_list<const wchar_t *> items) {
    auto control = make(id, request_, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL);
    for (const auto *item : items)
      if (::SendMessageW(control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item)) < 0)
        throw std::runtime_error("Could not populate the protocol options.");
    ::SendMessageW(control, CB_SETCURSEL, 0, 0);
  }
  void ProtocolWindow::create_tabs(Id id, HWND parent, const wchar_t *name,
                                  std::initializer_list<const wchar_t *> labels) {
    const auto tabs = make(id, parent, WC_TABCONTROLW, name, TCS_SINGLELINE | TCS_FOCUSONBUTTONDOWN);
    style_tabs(tabs, theme_);
    TabCtrl_SetPadding(tabs, scale(12), scale(4));
    for (const auto *label : labels) {
      TCITEMW item{};
      item.mask = TCIF_TEXT;
      item.pszText = const_cast<wchar_t *>(label);
      if (TabCtrl_InsertItem(tabs, TabCtrl_GetItemCount(tabs), &item) < 0)
        throw std::runtime_error("Could not create the HTTP workspace sections.");
    }
  }
  LRESULT ProtocolWindow::on_create(UINT, WPARAM, LPARAM, BOOL &) {
    try {
      NONCLIENTMETRICSW metrics{sizeof(metrics)};
      if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
        throw std::runtime_error("Could not read the Windows UI font.");
      font_ = CreateFontIndirectW(&metrics.lfMessageFont);
      lstrcpynW(metrics.lfMessageFont.lfFaceName, L"Consolas", LF_FACESIZE);
      mono_ = CreateFontIndirectW(&metrics.lfMessageFont);
      if (!font_ || !mono_) throw std::runtime_error("Could not create request fonts.");
      HDC dc = GetDC();
      dpi_ = GetDeviceCaps(dc, LOGPIXELSX);
      ReleaseDC(dc);
      theme_ = current_theme();
      RECT rect{};
      constexpr DWORD style = WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
      if (!split_.Create(m_hWnd, rect, nullptr, style, WS_EX_CONTROLPARENT))
        throw std::runtime_error("Could not create the request splitter.");
      request_.owner = response_.owner = m_hWnd;
      request_.paper = response_.paper = theme_.paper;
      if (!request_.Create(split_, rect, nullptr, style | WS_VSCROLL, WS_EX_CONTROLPARENT) ||
          !response_.Create(split_, rect, nullptr, style, WS_EX_CONTROLPARENT))
        throw std::runtime_error("Could not create request and output panels.");
      split_.SetSplitterPanes(request_, response_);
      split_.SetSplitterExtendedStyle(SPLIT_PROPORTIONAL);
      const bool serial = kind_ == ToolKind::serial;
      const bool http = request_tool(kind_);
      split_.m_cxyMin = scale(http ? 130 : 110);
      if (kind_ == ToolKind::http) {
        make(method_title, request_, L"STATIC", L"&Method", 0, false);
        choose(method, {L"GET", L"HEAD", L"POST", L"PUT", L"DELETE", L"PATCH", L"OPTIONS"});
      } else if (kind_ == ToolKind::rpc) {
        make(method_title, request_, L"STATIC", L"RPC &method", 0, false);
        ::SendMessageW(make(method, request_, L"EDIT", L"", ES_AUTOHSCROLL), EM_SETLIMITTEXT, 128,
                       0);
      }
      make(target_title, request_, L"STATIC",
           serial ? L"COM &port" : http ? L"Request &URL" : L"&Endpoint", 0, false);
      const wchar_t *initial = serial ? L"COM1"
                               : http ? L"http://127.0.0.1:8080/"
                                      : L"tcp://127.0.0.1:9000";
      if (serial) {
        auto menu = make(target, request_, L"COMBOBOX", L"", CBS_DROPDOWN | WS_VSCROLL);
        ::SetWindowTextW(menu, initial);
        make(ports, request_, L"BUTTON", L"Refresh ports");
        ::SendMessageW(menu, CB_LIMITTEXT, 8, 0);
      } else {
        auto url = make(target, request_, L"EDIT", initial, ES_AUTOHSCROLL);
        ::SendMessageW(url, EM_SETLIMITTEXT, 4096, 0);
      }
      make(run, request_, L"BUTTON",
           http     ? L"&Send request"
           : serial ? L"&Open port"
                    : L"&Connect");
      if (http) {
        if (kind_ == ToolKind::http)
          create_tabs(request_tabs, request_, L"Request sections",
                      {L"Query", L"Headers", L"Body", L"Session"});
        else
          create_tabs(request_tabs, request_, L"Request sections",
                      {L"Query", L"Headers", L"Params"});
        make(query_title, request_, L"STATIC", L"&Query string", 0, false);
        ::SendMessageW(make(query_text, request_, L"EDIT", L"", ES_AUTOHSCROLL), EM_SETLIMITTEXT,
                       4096, 0);
        make(header_title, request_, L"STATIC", L"&Headers (one Name: value per line)", 0, false);
        ::SendMessageW(make(headers, request_, L"EDIT", L"", kMultiline), EM_SETLIMITTEXT,
                       kToolHeadersBytes, 0);
        if (kind_ == ToolKind::http) {
          make(json_body, request_, L"BUTTON", L"&JSON body", BS_AUTOCHECKBOX);
          make(use_cookies, request_, L"BUTTON", L"Use &cookies", BS_AUTOCHECKBOX);
          make(clear_cookies, request_, L"BUTTON", L"Clear cookies");
        }
      } else {
        make(hex, request_, L"BUTTON", L"&Hex input", BS_AUTOCHECKBOX);
        make(send, request_, L"BUTTON", L"Send &bytes");
      }
      if (serial) {
        choose(baud, {L"115200", L"9600", L"19200", L"38400", L"57600", L"230400"});
        choose(bits, {L"8 data bits", L"7 data bits", L"6 data bits", L"5 data bits"});
        choose(parity,
               {L"No parity", L"Odd parity", L"Even parity", L"Mark parity", L"Space parity"});
        choose(stop_bits, {L"1 stop bit", L"2 stop bits"});
        choose(flow, {L"No flow control", L"XON/XOFF", L"RTS/CTS", L"DTR/DSR"});
      }
      make(body_title, request_, L"STATIC",
           kind_ == ToolKind::rpc ? L"&Params (JSON)"
           : http                 ? L"&Body"
                                  : L"&Bytes",
           0, false);
      ::SendMessageW(make(body, request_, L"EDIT", L"", kMultiline), EM_SETLIMITTEXT, kToolBytes,
                     0);
      make(message_title, response_, L"STATIC", http ? L"Response" : L"Byte stream", 0, false);
      if (http) {
        make(response_summary, response_, L"STATIC", L"No response yet", SS_RIGHT, false);
        create_tabs(response_tabs, response_, L"Response sections", {L"Body", L"Headers", L"Activity"});
        ::SendMessageW(make(response_body, response_, L"EDIT", L"", kMultiline | ES_READONLY),
                       EM_SETLIMITTEXT, kToolBytes * 5, 0);
        ::SendMessageW(make(response_headers, response_, L"EDIT", L"", kMultiline | ES_READONLY),
                       EM_SETLIMITTEXT, kToolHeadersBytes * 5, 0);
      }
      make(rows, response_, L"LISTBOX", L"", LBS_NOTIFY | WS_BORDER | WS_VSCROLL);
      ::SendMessageW(make(detail, response_, L"EDIT", L"", kMultiline | ES_READONLY),
                     EM_SETLIMITTEXT, kToolBytes * 5, 0);
      make(report, m_hWnd, L"STATIC", L"Ready", 0, false);
      ready_ = true;
      if (http) {
        select_request_page(kind_ == ToolKind::rpc ? RequestPage::body : RequestPage::query);
        select_response_page(runner_ ? ResponsePage::activity : ResponsePage::body);
      }
      BOOL handled = TRUE;
      on_layout(0, 0, 0, handled);
      split_.SetSplitterPosPct(http ? 45 : 56);
      if (!SetTimer(1, 50)) throw std::runtime_error("Could not start the request timer.");
      update();
      return 0;
    } catch (const std::exception &error) {
      ::MessageBoxA(m_hWnd, error.what(), "Flowie Workbench", MB_OK | MB_ICONERROR);
      return -1;
    }
  }

  void ProtocolWindow::place(Id id, int x, int y, int width, int height) {
    if (const auto control = get(id)) {
      const int offset = ::GetParent(control) == request_ ? request_.scroll_y() : 0;
      if (id == baud || id == bits || id == parity || id == stop_bits || id == flow ||
          (id == method && kind_ == ToolKind::http) || (id == target && kind_ == ToolKind::serial))
        height = 180;
      ::MoveWindow(control, scale(x), scale(y) - offset, scale(std::max(0, width)),
                   scale(std::max(0, height)), TRUE);
    }
  }
  void ProtocolWindow::select_request_page(RequestPage page) {
    request_page_ = page;
    TabCtrl_SetCurSel(get(request_tabs), static_cast<int>(page));
    const auto focus = ::GetFocus();
    const auto show = [&](Id id, RequestPage owner) {
      if (const auto control = get(id)) {
        if (page != owner && focus == control) ::SetFocus(get(request_tabs));
        ::ShowWindow(control, page == owner ? SW_SHOWNA : SW_HIDE);
      }
    };
    for (const auto id : {query_title, query_text}) show(id, RequestPage::query);
    for (const auto id : {header_title, headers}) show(id, RequestPage::headers);
    for (const auto id : {body_title, body, json_body}) show(id, RequestPage::body);
    for (const auto id : {use_cookies, clear_cookies}) show(id, RequestPage::session);
    request_.reset_scroll();
    BOOL handled = TRUE;
    on_layout(kPanelLayout, 0, 0, handled);
  }

  void ProtocolWindow::select_response_page(ResponsePage page) {
    TabCtrl_SetCurSel(get(response_tabs), static_cast<int>(page));
    const auto focus = ::GetFocus();
    const auto show = [&](Id id, ResponsePage owner) {
      const auto control = get(id);
      if (page != owner && focus == control) ::SetFocus(get(response_tabs));
      ::ShowWindow(control, page == owner ? SW_SHOWNA : SW_HIDE);
    };
    show(response_body, ResponsePage::body);
    show(response_headers, ResponsePage::headers);
    show(rows, ResponsePage::activity);
    show(detail, ResponsePage::activity);
  }

  void ProtocolWindow::sync_query(Id edited) {
    if (syncing_query_ || !get(query_text)) return;
    const auto url = read_wide(get(target));
    const auto fragment = url.find(L'#');
    const auto separator = url.find(L'?');
    const auto end = fragment == url.npos ? url.size() : fragment;
    const bool has_query = separator < end;
    if (edited == target) {
      const auto query = has_query ? url.substr(separator + 1, end - separator - 1)
                                   : std::wstring{};
      if (query == read_wide(get(query_text))) return;
      syncing_query_ = true;
      ::SetWindowTextW(get(query_text), query.c_str());
    } else {
      const auto query = read_wide(get(query_text));
      auto updated = url.substr(0, has_query ? separator : end);
      if (!query.empty()) updated += L"?" + query;
      updated += url.substr(end);
      if (updated == url) return;
      syncing_query_ = true;
      ::SetWindowTextW(get(target), updated.c_str());
    }
    syncing_query_ = false;
  }

  void ProtocolWindow::layout_web_request(SIZE size) {
    constexpr int inset = 12, gutter = 8, action_width = 110;
    constexpr int input_to_tabs = 38, tabs_to_content = 37;
    const int method_width = kind_ == ToolKind::http ? 104 : 180;
    const int inline_width = method_width + action_width + 180;
    const auto url_top = [](bool inline_url) { return inline_url ? 26 : 78; };
    const auto set_extent = [&](bool inline_url) {
      const int content_y = url_top(inline_url) + input_to_tabs + tabs_to_content;
      const int extent = request_page_ == RequestPage::query     ? content_y + 62
                         : request_page_ == RequestPage::session ? content_y + 46
                                                                 : content_y + 24 + 64 + inset;
      request_.extent(scale(extent));
    };
    const auto measure_width = [&] {
      RECT viewport{};
      ::GetClientRect(request_, &viewport);
      return MulDiv(viewport.right, 96, dpi_) - 2 * inset;
    };
    const bool initial_inline = size.cx - 2 * inset >= inline_width;
    set_extent(initial_inline);
    int width = measure_width(); // The vertical scrollbar can change the available width.
    if ((width >= inline_width) != initial_inline) {
      set_extent(width >= inline_width);
      width = measure_width();
    }
    const bool inline_url = width >= inline_width;
    const int url_x = inline_url ? inset + method_width + gutter : inset;
    const int url_y = url_top(inline_url);
    const int bar_y = url_y + input_to_tabs, content_y = bar_y + tabs_to_content;
    const int action_x = inset + width - action_width;
    const int edit_y = content_y + 24;
    const int field_height = std::max(64, static_cast<int>(size.cy) - edit_y - inset);
    place(method_title, inset, 6, method_width, 18);
    place(method, inset, 26, std::min(method_width, width - action_width - gutter), 28);
    place(target_title, url_x, url_y - 20, 150, 18);
    place(target, url_x, url_y, inline_url ? action_x - gutter - url_x : width, 29);
    place(run, action_x, 26, action_width, 30);
    place(request_tabs, inset, bar_y, width, 29);

    place(query_title, inset, content_y, width, 20);
    place(query_text, inset, edit_y, width, 28);
    place(header_title, inset, content_y, width, 20);
    place(headers, inset, edit_y, width, field_height);
    place(body_title, inset, content_y, width - 122, 20);
    place(json_body, inset + width - 110, content_y - 3, 108, 24);
    place(body, inset, edit_y, width, field_height);
    place(use_cookies, inset, content_y, 132, 28);
    place(clear_cookies, inset + 144, content_y, 110, 29);
  }

  void ProtocolWindow::layout_web_response(SIZE size) {
    constexpr int inset = 12, content_y = 65;
    const int width = static_cast<int>(size.cx) - 2 * inset;
    const int height = static_cast<int>(size.cy) - content_y - inset;
    place(message_title, inset, 7, 90, 20);
    place(response_summary, inset + 96, 7, width - 96, 20);
    place(response_tabs, inset, 29, width, 28);
    place(response_body, inset, content_y, width, height);
    place(response_headers, inset, content_y, width, height);
    const int events_width = std::min(270, width * 2 / 5);
    place(rows, inset, content_y, events_width, height);
    place(detail, inset + events_width + 8, content_y, width - events_width - 8, height);
  }

  LRESULT ProtocolWindow::on_layout(UINT message, WPARAM, LPARAM, BOOL &) {
    if (!ready_ || layout_) return 0;
    struct Layout {
      bool &busy;
      ~Layout() { busy = false; }
    } guard{layout_};
    layout_ = true;
    RECT bounds{};
    GetClientRect(&bounds);
    if (message != kPanelLayout)
      split_.MoveWindow(0, 0, bounds.right, std::max(0L, bounds.bottom - scale(25)), TRUE);
    const auto logical = [&](HWND pane) {
      RECT rect{};
      ::GetClientRect(pane, &rect);
      return SIZE{MulDiv(rect.right, 96, dpi_), MulDiv(rect.bottom, 96, dpi_)};
    };
    const auto r = logical(request_);
    const auto p = logical(response_);
    place(report, 12, MulDiv(bounds.bottom, 96, dpi_) - 24, MulDiv(bounds.right, 96, dpi_) - 24,
          22);
    if (request_tool(kind_)) {
      layout_web_request(r);
      layout_web_response(p);
      return 0;
    }
    const bool serial = kind_ == ToolKind::serial;
    const int width = r.cx - 24;
    const int body_y = serial ? 138 : 103;
    request_.extent(scale(body_y + 76));
    place(target_title, 12, 8, 120, 18);
    place(target, 12, 27, width - 122 - (serial ? 112 : 0), 27);
    place(ports, r.cx - 238, 25, 103, 30);
    place(run, r.cx - 124, 25, 112, 30);
    if (serial) {
      place(baud, 12, 64, 90, 26);
      place(bits, 110, 64, 92, 26);
      place(parity, 210, 64, 108, 26);
      place(stop_bits, 326, 64, 93, 26);
      place(flow, 427, 64, 120, 26);
    }
    place(hex, r.cx - 220, body_y - 32, 87, 26);
    place(send, r.cx - 124, body_y - 32, 112, 28);
    place(body_title, 12, body_y - 22, 190, 18);
    place(body, 12, body_y, width, std::max(64, static_cast<int>(r.cy) - body_y - 12));
    const int event_width = std::min(290L, p.cx * 2 / 5);
    place(message_title, 12, 4, p.cx - 24, 22);
    place(rows, 12, 28, event_width - 12, p.cy - 35);
    place(detail, event_width + 9, 28, p.cx - event_width - 21, p.cy - 35);
    return 0;
  }

  ToolDraft ProtocolWindow::draft() const {
    ToolDraft value;
    value.kind = kind_;
    value.endpoint = read_text(get(target));
    if (kind_ == ToolKind::http) {
      constexpr const char *methods[] = {"GET",    "HEAD",  "POST",   "PUT",
                                         "DELETE", "PATCH", "OPTIONS"};
      const int index = selected(get(method));
      if (index < 0 || index >= static_cast<int>(std::size(methods)))
        throw std::runtime_error("Select an HTTP method.");
      value.method = methods[index];
      value.json_body = ::SendMessageW(get(json_body), BM_GETCHECK, 0, 0) == BST_CHECKED;
      value.use_cookies = ::SendMessageW(get(use_cookies), BM_GETCHECK, 0, 0) == BST_CHECKED;
    } else value.method = read_text(get(method));
    value.headers = read_text(get(headers));
    value.body = read_text(get(body));
    value.hex = get(hex) && ::SendMessageW(get(hex), BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (kind_ == ToolKind::serial) {
      constexpr int rates[] = {115200, 9600, 19200, 38400, 57600, 230400};
      const int index = selected(get(baud));
      if (index < 0 || index >= static_cast<int>(std::size(rates)))
        throw std::runtime_error("Select a baud rate.");
      value.baud = rates[index];
      value.bits = 8 - selected(get(bits));
      value.parity = selected(get(parity));
      value.stop_bits = 1 + selected(get(stop_bits));
      value.flow = selected(get(flow));
    }
    return value;
  }
  SavedRequest ProtocolWindow::capture_request() const {
    SavedRequest request;
    auto value = draft();
    const auto extra = value.endpoint.find_first_of("?#");
    if (extra != value.endpoint.npos) value.endpoint.resize(extra);
    if (value.endpoint.find('@') != value.endpoint.npos)
      throw std::runtime_error("Remove credentials from the endpoint before saving.");
    if (request_tool(kind_)) value.headers = collection_request_headers(value.headers, true);
    request.tool = std::move(value);
    return request;
  }
  void ProtocolWindow::restore_request(const ToolDraft &draft) {
    ::SetWindowTextW(get(target), utf8_to_wide(draft.endpoint).c_str());
    ::SetWindowTextW(get(body), utf8_to_wide(draft.body).c_str());
    if (get(headers)) {
      std::string lines;
      for (const auto ch : draft.headers) {
        if (ch == '\n') lines += '\r';
        lines += ch;
      }
      ::SetWindowTextW(get(headers), utf8_to_wide(lines).c_str());
    }
    if (kind_ == ToolKind::http) {
      const auto method = utf8_to_wide(draft.method);
      if (::SendMessageW(get(ProtocolWindow::method), CB_SELECTSTRING, static_cast<WPARAM>(-1),
                         reinterpret_cast<LPARAM>(method.c_str())) == CB_ERR)
        throw std::runtime_error("The saved request has an unsupported method.");
      ::SendMessageW(get(json_body), BM_SETCHECK, draft.json_body ? BST_CHECKED : BST_UNCHECKED, 0);
      ::SendMessageW(get(use_cookies), BM_SETCHECK, draft.use_cookies ? BST_CHECKED : BST_UNCHECKED,
                     0);
    } else if (kind_ == ToolKind::rpc)
      ::SetWindowTextW(get(method), utf8_to_wide(draft.method).c_str());
    else ::SendMessageW(get(hex), BM_SETCHECK, draft.hex ? BST_CHECKED : BST_UNCHECKED, 0);
    if (kind_ == ToolKind::serial) {
      const auto speed = std::to_wstring(draft.baud);
      if (::SendMessageW(get(baud), CB_SELECTSTRING, static_cast<WPARAM>(-1),
                         reinterpret_cast<LPARAM>(speed.c_str())) == CB_ERR)
        throw std::runtime_error("The saved baud rate is not available.");
      ::SendMessageW(get(bits), CB_SETCURSEL, 8 - draft.bits, 0);
      ::SendMessageW(get(parity), CB_SETCURSEL, draft.parity, 0);
      ::SendMessageW(get(stop_bits), CB_SETCURSEL, draft.stop_bits - 1, 0);
      ::SendMessageW(get(flow), CB_SETCURSEL, draft.flow, 0);
    }
  }

  std::wstring ProtocolWindow::remote_endpoint() const {
    if (runner_) return collection_remote_;
    try {
      return utf8_to_wide(tool_remote(kind_, read_text(get(target))));
    } catch (const std::runtime_error &) {
      return {};
    }
  }

  void ProtocolWindow::start_collection(std::vector<CollectionStep> steps) {
    if (!runner_ || steps.empty() || !collection_steps_.empty())
      throw std::logic_error("Could not start the collection.");
    session_.clear_cookies();
    set_task_layout(TaskLayout::output);
    collection_index_ = 0;
    collection_canceled_ = false;
    collection_steps_ = std::move(steps);
    start_collection_step();
  }

  void ProtocolWindow::start_collection_step() {
    auto &step = collection_steps_[collection_index_];
    collection_remote_ = utf8_to_wide(tool_remote(step.request.kind, step.request.endpoint));
    connection_status_ = L"Step " + std::to_wstring(collection_index_ + 1) + L" / " +
                         std::to_wstring(collection_steps_.size()) + L" · " +
                         utf8_to_wide(step.name);
    ::SetWindowTextW(get(report), connection_status_.c_str());
    step_finished_ = step_failed_ = false;
    session_.start(std::move(step.request));
    update();
  }

  void ProtocolWindow::advance_collection() {
    if (collection_steps_.empty()) return;
    if (!collection_canceled_ && step_finished_ && !step_failed_ &&
        collection_index_ + 1 < collection_steps_.size()) {
      ++collection_index_;
      start_collection_step();
      return;
    }
    if (collection_canceled_) connection_status_ = L"Collection canceled";
    else if (!step_finished_ || step_failed_)
      connection_status_ = L"Collection stopped at step " +
                           std::to_wstring(collection_index_ + 1) + L" · " +
                           utf8_to_wide(collection_steps_[collection_index_].name);
    else
      connection_status_ = L"Collection completed · " + std::to_wstring(collection_steps_.size()) +
                           L" requests";
    ::SetWindowTextW(get(report), connection_status_.c_str());
    collection_steps_.clear();
    session_.clear_cookies();
  }
  void ProtocolWindow::refresh_ports() {
    const auto current = utf8_to_wide(read_text(get(target)));
    serial_port_list_t *raw = nullptr;
    const auto status = serial_list_ports(&raw);
    if (status != SALTS_SERIAL_OK) throw std::runtime_error(serial_result_name(status));
    const std::unique_ptr<serial_port_list_t, decltype(&serial_port_list_destroy)> list(
        raw, serial_port_list_destroy);
    ::SendMessageW(get(target), CB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < serial_port_list_count(list.get()); ++i) {
      const auto *port = serial_port_list_get(list.get(), i);
      const auto name = utf8_to_wide(port->name);
      if (::SendMessageW(get(target), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str())) < 0)
        throw std::runtime_error("Could not display the serial ports.");
    }
    ::SetWindowTextW(get(target), current.c_str());
    ::SendMessageW(get(target), CB_SHOWDROPDOWN, TRUE, 0);
  }
  void ProtocolWindow::set_task_layout(TaskLayout layout) {
    if (layout == task_layout_) return;
    const auto focus = ::GetFocus();
    const int pane = layout == TaskLayout::editor   ? SPLIT_PANE_TOP
                     : layout == TaskLayout::output ? SPLIT_PANE_BOTTOM
                                                    : SPLIT_PANE_NONE;
    task_layout_ = layout;
    split_.SetSinglePaneMode(pane);
    BOOL handled = TRUE;
    on_layout(kPanelLayout, 0, 0, handled);
    if (pane == SPLIT_PANE_TOP && ::IsChild(response_, focus))
      ::SetFocus(get(request_tabs) ? get(request_tabs) : get(body));
    if (pane == SPLIT_PANE_BOTTOM && ::IsChild(request_, focus))
      ::SetFocus(get(response_tabs) ? get(response_tabs) : get(rows));
  }

  RequestCommand ProtocolWindow::request_command(RequestAction action) const {
    const bool http = request_tool(kind_);
    const auto state = session_.state();
    const bool idle = state == State::idle && !closing_;
    const bool active = state == State::connecting || state == State::connected;
    if (runner_) {
      if (action != RequestAction::stop) return {};
      if (state == State::close_failed) return {L"Retry Close", true};
      return {L"Cancel Run", !closing_ && !collection_canceled_ && !collection_steps_.empty()};
    }
    switch (action) {
    case RequestAction::open:
      return http ? RequestCommand{}
                  : RequestCommand{kind_ == ToolKind::serial ? L"Open Port" : L"Connect", idle};
    case RequestAction::send:
      return http
                 ? RequestCommand{kind_ == ToolKind::rpc ? L"Send RPC Call" : L"Send Request", idle}
                 : RequestCommand{L"Send Bytes", state == State::connected && !session_.busy() &&
                                                     !session_.stopping() && !closing_};
    case RequestAction::stop:
      return {state == State::close_failed ? L"Retry Close"
              : http                       ? L"Cancel Request"
              : kind_ == ToolKind::serial  ? L"Close Port"
                                           : L"Disconnect",
              state == State::close_failed || (active && !session_.stopping() && !closing_)};
    case RequestAction::refresh_ports:
      return kind_ == ToolKind::serial ? RequestCommand{L"Refresh Ports", idle} : RequestCommand{};
    case RequestAction::sample:
      return {L"Load Sample...", !closing_};
    case RequestAction::validate_json:
    case RequestAction::format_json:
      return {action == RequestAction::format_json ? L"Format JSON" : L"Validate JSON",
              !closing_ &&
                  (!get(hex) || ::SendMessageW(get(hex), BM_GETCHECK, 0, 0) != BST_CHECKED)};
    }
    return {};
  }

  void ProtocolWindow::apply_sample(std::string_view payload) {
    if (!request_command(RequestAction::sample).enabled) return;
    if (payload.size() > kToolBytes) throw std::runtime_error("Payload exceeds 64 KiB.");
    if (kind_ == ToolKind::rpc) check_json(kind_, std::string(payload), nullptr);
    const auto text = utf8_to_wide(payload);
    ::SetWindowTextW(get(body), text.c_str());
    if (get(request_tabs)) select_request_page(RequestPage::body);
    if (get(hex)) ::SendMessageW(get(hex), BM_SETCHECK, BST_UNCHECKED, 0);
    ::SetWindowTextW(get(report), L"Sample loaded");
    if (task_layout_ == TaskLayout::output) set_task_layout(TaskLayout::workspace);
  }

  void ProtocolWindow::invoke_request(RequestAction action) {
    if (!request_command(action).enabled) return;
    switch (action) {
    case RequestAction::open:
    case RequestAction::send:
      if (action == RequestAction::send && !request_tool(kind_)) {
        session_.send(tool_payload(draft()));
      } else {
        session_.start(draft());
        connection_status_.clear();
        ::SetWindowTextW(get(report), L"Starting...");
      }
      break;
    case RequestAction::stop:
      if (runner_) collection_canceled_ = true;
      session_.stop();
      break;
    case RequestAction::refresh_ports:
      if (task_layout_ == TaskLayout::output) set_task_layout(TaskLayout::workspace);
      refresh_ports();
      break;
    case RequestAction::validate_json:
    case RequestAction::format_json: {
      std::string formatted;
      const bool format = action == RequestAction::format_json;
      if (get(request_tabs)) select_request_page(RequestPage::body);
      check_json(kind_, read_text(get(body)), format ? &formatted : nullptr);
      if (format) apply_sample(formatted);
      ::SetWindowTextW(get(report), format ? L"JSON formatted" : L"JSON is valid");
      break;
    }
    case RequestAction::sample:
      break; // Workbench owns the file picker.
    }
    update();
  }

  void ProtocolWindow::update() {
    const auto state = session_.state();
    const bool idle = state == State::idle && !closing_;
    for (const auto id : {target, ports, method, query_text, headers, baud, bits, parity, stop_bits,
                          flow, json_body, use_cookies, clear_cookies})
      if (get(id)) ::EnableWindow(get(id), idle);
    const bool http = request_tool(kind_);
    const auto action = state == State::idle ? (http ? RequestAction::send : RequestAction::open)
                                             : RequestAction::stop;
    const auto command = request_command(action);
    const wchar_t *label = state == State::closing        ? L"Closing..."
                           : http && state == State::idle ? L"Send request"
                                                          : command.label;
    wchar_t before[32]{};
    ::GetWindowTextW(get(run), before, 32);
    if (std::wcscmp(before, label) != 0) ::SetWindowTextW(get(run), label);
    ::EnableWindow(get(run), command.enabled);
    if (get(send)) ::EnableWindow(get(send), request_command(RequestAction::send).enabled);
  }
  LRESULT ProtocolWindow::on_command(UINT, WPARAM wparam, LPARAM, BOOL &) {
    try {
      const int id = LOWORD(wparam);
      if (HIWORD(wparam) == EN_CHANGE && (id == target || id == query_text)) {
        sync_query(static_cast<Id>(id));
        return 0;
      }
      if (id == rows && HIWORD(wparam) == LBN_SELCHANGE) show_detail();
      if (HIWORD(wparam) == BN_CLICKED) {
        if (id == run) {
          invoke_request(state() != State::idle ? RequestAction::stop
                         : request_tool(kind_)  ? RequestAction::send
                                                : RequestAction::open);
        } else if (id == send) invoke_request(RequestAction::send);
        else if (id == ports) invoke_request(RequestAction::refresh_ports);
        else if (id == clear_cookies) {
          session_.clear_cookies();
          ::SetWindowTextW(get(report), L"Cookies cleared");
        }
      }
    } catch (const std::exception &error) {
      ::SetWindowTextA(get(report), error.what());
    }
    update();
    return 0;
  }
  LRESULT ProtocolWindow::on_notify(UINT, WPARAM, LPARAM lparam, BOOL &handled) {
    const auto *notice = reinterpret_cast<const NMHDR *>(lparam);
    if (notice->code == TCN_SELCHANGE) {
      const auto index = TabCtrl_GetCurSel(notice->hwndFrom);
      if (index >= 0 && notice->idFrom == request_tabs)
        select_request_page(static_cast<RequestPage>(index));
      if (index >= 0 && notice->idFrom == response_tabs)
        select_response_page(static_cast<ResponsePage>(index));
      return 0;
    }
    if (notice->code == NM_CUSTOMDRAW &&
        (notice->idFrom == run || notice->idFrom == send || notice->idFrom == ports))
      return draw_form_button(*reinterpret_cast<const NMCUSTOMDRAW *>(notice), theme_, dpi_,
                              notice->idFrom != ports);
    handled = FALSE;
    return 0;
  }
  void ProtocolWindow::show_detail() {
    const auto index = ::SendMessageW(get(rows), LB_GETCURSEL, 0, 0);
    if (index < 0 || static_cast<size_t>(index) >= history_.records().size()) return;
    const auto &event = history_.records()[index];
    auto detail_text = event_line(event);
    if (event.status < 0) {
      detail_text += L" ";
      detail_text += utf8_to_wide(error_code_string(event.status));
    }
    const auto headers_text = show_bytes(event.reasons);
    const auto payload = show_bytes(event.payload);
    if (request_tool(kind_)) {
      const auto body_text = event.kind == Kind::error ? detail_text : windows_lines(payload);
      std::wstring summary;
      if (event.reason) {
        summary = L"HTTP " + std::to_wstring(event.reason) + L"   ·   " +
                  std::to_wstring(event.payload.size()) + L" B";
      } else {
        summary = event.kind == Kind::error ? L"Request failed" : L"Waiting for response";
      }
      ::SetWindowTextW(get(response_body), body_text.c_str());
      ::SetWindowTextW(get(response_headers), windows_lines(headers_text).c_str());
      ::SetWindowTextW(get(response_summary), summary.c_str());
    }
    detail_text += L"\r\n" + headers_text + L"\r\n" + payload;
    ::SetWindowTextW(get(detail), detail_text.c_str());
  }
  LRESULT ProtocolWindow::on_timer(UINT, WPARAM, LPARAM, BOOL &) {
    try {
      // An idle observation before draining events guarantees the result has been published.
      const bool idle = session_.state() == State::idle;
      auto events = session_.poll();
      const auto list = get(rows);
      for (auto &event : events) {
        if (runner_ && !collection_steps_.empty()) {
          if (event.kind == Kind::received) {
            step_finished_ = true;
            step_failed_ |= !successful_http_result(event);
          } else if (event.kind == Kind::error) step_failed_ = true;
          event.topic = collection_steps_[collection_index_].name + " · " + event.topic;
        } else if (request_tool(kind_) && !runner_) {
          if (event.kind == Kind::received)
            connection_status_ = L"HTTP " + std::to_wstring(event.reason);
          else if (event.kind == Kind::error)
            connection_status_ = L"Request failed · " + utf8_to_wide(event.topic);
        }
        const auto label = event_line(event);
        const auto before = history_.records().size(), evicted = history_.evicted();
        history_.append(std::move(event));
        for (size_t i = evicted; i < history_.evicted() && i - evicted < before; ++i)
          ::SendMessageW(list, LB_DELETESTRING, 0, 0);
        if (::SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str())) < 0)
          throw std::runtime_error("Could not display protocol history.");
        ::SetWindowTextW(get(report), label.c_str());
      }
      if (!events.empty()) {
        ::SendMessageW(list, LB_SETCURSEL, history_.records().size() - 1, 0);
        show_detail();
      }
      if (idle && runner_ && !closing_) advance_collection();
      update();
      if (closing_ && session_.state() == State::idle) DestroyWindow();
    } catch (const std::exception &error) {
      ::SetWindowTextA(get(report), error.what());
      if (runner_) {
        connection_status_ = L"Collection failed";
        collection_steps_.clear();
      }
      session_.stop();
    }
    return 0;
  }
  LRESULT ProtocolWindow::on_close(UINT, WPARAM, LPARAM, BOOL &) {
    closing_ = true;
    session_.stop();
    if (session_.state() == State::idle) DestroyWindow();
    else update();
    return 0;
  }
  LRESULT ProtocolWindow::on_destroy(UINT, WPARAM, LPARAM, BOOL &) {
    KillTimer(1);
    DeleteObject(font_);
    DeleteObject(mono_);
    return 0;
  }
  LRESULT ProtocolWindow::on_erase(UINT, WPARAM dc, LPARAM, BOOL &) {
    RECT rect{};
    GetClientRect(&rect);
    auto hdc = reinterpret_cast<HDC>(dc);
    ::FillRect(hdc, &rect, color_brush(hdc, theme_.ink, theme_.surface));
    return 1;
  }
  LRESULT ProtocolWindow::on_color(UINT, WPARAM dc, LPARAM control, BOOL &) {
    const auto id = ::GetDlgCtrlID(reinterpret_cast<HWND>(control));
    return reinterpret_cast<LRESULT>(color_brush(reinterpret_cast<HDC>(dc), theme_.ink,
                                                 id == report ? theme_.surface : theme_.paper));
  }
  LRESULT ProtocolWindow::on_theme(UINT, WPARAM, LPARAM, BOOL &) {
    try {
      theme_ = current_theme();
      request_.paper = response_.paper = theme_.paper;
      ::RedrawWindow(m_hWnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    } catch (const std::exception &error) {
      ::SetWindowTextA(get(report), error.what());
    }
    return 0;
  }
  bool ProtocolWindow::translate(MSG *message) {
    return ::IsDialogMessageW(m_hWnd, message) != FALSE;
  }
} // namespace flowie::app
