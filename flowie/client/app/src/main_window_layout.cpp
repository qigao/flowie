#include "main_window.h"
#include <algorithm>
#include <stdexcept>

namespace flowie::app {
  HWND MainWindow::control_parent(int id) const {
    if (id >= 1000) id -= 1000;
    switch (id) {
    case host:
    case port:
    case transport:
    case protocol:
    case connect_button:
    case disconnect_button:
      return connection_bar_;
    case request_tabs:
      return request_;
    case username:
    case password:
      return auth_;
    case path:
    case client_id:
    case ca:
    case cert:
    case key:
      return settings_;
    case filter:
    case subscribe_button:
    case unsubscribe_button:
    case sub_qos:
    case subscriptions:
    case saved_topic_picker:
    case subscription_title:
      return topics_;
    case topic:
    case pub_qos:
    case retain:
    case mode:
    case payload:
    case publish_button:
    case format_button:
    case validate_button:
      return composer_;
    case records:
    case history_title:
    case message_filter:
    case ping_button:
    case clear_button:
      return history_panel_;
    case details:
    case detail_title:
      return detail_panel_;
    default:
      return m_hWnd;
    }
  }

  void MainWindow::create_panels() {
    constexpr DWORD style = WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    RECT rect{};
    GetClientRect(&rect);
    if (!editor_.Create(m_hWnd, rect, nullptr, style, WS_EX_CONTROLPARENT, editor_splitter) ||
        !results_.Create(editor_, rect, nullptr, style, WS_EX_CONTROLPARENT, results_splitter))
      throw std::runtime_error("Could not create the request and message splitters");

    auto create = [&](UiPanel &panel, HWND parent, bool scrollable) {
      panel.owner = m_hWnd;
      panel.paper = theme_.paper;
      if (!panel.Create(parent, rect, nullptr, style | (scrollable ? WS_VSCROLL : 0),
                        WS_EX_CONTROLPARENT))
        throw std::runtime_error("Could not create a request pane");
    };
    create(connection_bar_, m_hWnd, false);
    create(request_, editor_, false);
    for (auto *page : {&composer_, &topics_, &auth_, &settings_})
      create(*page, request_, true);
    create(history_panel_, results_, false);
    create(detail_panel_, results_, false);
    editor_.SetSplitterPanes(request_, results_);
    results_.SetSplitterPanes(history_panel_, detail_panel_);
    editor_.SetSplitterExtendedStyle(SPLIT_PROPORTIONAL);
    results_.SetSplitterExtendedStyle(SPLIT_PROPORTIONAL);
    editor_.m_cxyMin = scale(96);
    results_.m_cxyMin = scale(52);
  }

  void MainWindow::create_request_tabs() {
    const HWND tabs =
        make(request_tabs, WC_TABCONTROLW, L"Request sections", WS_CLIPCHILDREN | TCS_SINGLELINE);
    style_tabs(tabs, theme_);
    TabCtrl_SetPadding(tabs, scale(12), scale(3));
    TabCtrl_SetItemSize(tabs, 0, scale(25));
    const auto append = [&](RequestPage page, const wchar_t *name) {
      TCITEMW item{};
      item.mask = TCIF_TEXT;
      item.pszText = const_cast<wchar_t *>(name);
      if (TabCtrl_InsertItem(tabs, static_cast<int>(request_pages_.size()), &item) == -1)
        throw std::runtime_error("Could not create the request tabs");
      request_pages_.push_back(page);
    };
    if (role_ != ClientRole::subscriber) append(RequestPage::message, L"Message");
    if (role_ != ClientRole::publisher) append(RequestPage::topics, L"Topics");
    append(RequestPage::authorization, L"Authorization");
    append(RequestPage::settings, L"Settings");
    request_page_ = request_pages_.front();
    TabCtrl_SetCurSel(tabs, 0);
  }

  void MainWindow::select_request_page(RequestPage page) {
    const auto item = std::find(request_pages_.begin(), request_pages_.end(), page);
    if (item == request_pages_.end()) return;
    const HWND tabs = controls_.at(request_tabs);
    const HWND focus = ::GetFocus();
    const int index = static_cast<int>(item - request_pages_.begin());
    if (TabCtrl_GetCurSel(tabs) != index) TabCtrl_SetCurSel(tabs, index);
    request_page_ = page;
    layout_panels();
    if (::IsChild(request_, focus) && !::IsWindowVisible(focus)) ::SetFocus(tabs);
  }

  void MainWindow::place(int id, int x, int y, int width, int height) {
    if (id == transport || id == protocol || id == sub_qos || id == pub_qos || id == mode ||
        id == saved_topic_picker || id == message_filter)
      height = 200;
    const HWND control = controls_.at(id);
    const HWND parent = ::GetParent(control);
    int offset = 0;
    for (const auto *page : {&composer_, &topics_, &auth_, &settings_})
      if (parent == *page) offset = page->scroll_y();
    ::MoveWindow(control, scale(x), scale(y) - offset, scale(std::max(0, width)),
                 scale(std::max(0, height)), TRUE);
  }

  void MainWindow::layout() {
    if (!panels_ready_) return;
    RECT rect{};
    GetClientRect(&rect);
    const int width = MulDiv(rect.right, 96, dpi_), height = MulDiv(rect.bottom, 96, dpi_);
    const int connection_height = width < 530 ? 94 : 60;
    constexpr int footer_height = 26;
    connection_bar_.MoveWindow(0, 0, rect.right, scale(connection_height), TRUE);
    editor_.MoveWindow(
        0, scale(connection_height + 2), rect.right,
        std::max(0, static_cast<int>(rect.bottom) - scale(connection_height + 2 + footer_height)),
        TRUE);
    place(status, 10, height - 24, std::max(120, width - 226), 24);
    place(counters, std::max(130, width - 214), height - 24, 210, 24);
    layout_panels();
  }

  void MainWindow::set_task_layout(TaskLayout task) {
    if (task != task_layout_) {
      const auto focus = ::GetFocus();
      const int pane = task == TaskLayout::editor   ? SPLIT_PANE_TOP
                       : task == TaskLayout::output ? SPLIT_PANE_BOTTOM
                                                    : SPLIT_PANE_NONE;
      task_layout_ = task;
      editor_.SetSinglePaneMode(pane);
      layout_panels();
      if (::IsChild(m_hWnd, focus) && !::IsWindowVisible(focus))
        ::SetFocus(controls_.at(task == TaskLayout::output ? records : request_tabs));
    }
    if (task == TaskLayout::editor)
      select_request_page(can_publish() ? RequestPage::message : RequestPage::topics);
  }

  void MainWindow::layout_panels() {
    if (!panels_ready_ || laying_out_) return;
    struct Guard {
      bool &value;
      Guard(bool &v) : value(v) { value = true; }
      ~Guard() { value = false; }
    } guard(laying_out_);
    auto size = [&](HWND window) {
      RECT rect{};
      ::GetClientRect(window, &rect);
      return SIZE{MulDiv(rect.right, 96, dpi_), MulDiv(rect.bottom, 96, dpi_)};
    };
    auto show = [&](int id, bool visible) {
      ::ShowWindow(controls_.at(id), visible ? SW_SHOWNA : SW_HIDE);
    };
    auto field = [&](int id, bool visible) {
      show(id, visible);
      show(id + 1000, visible);
    };
    auto row = [&](int id, int x, int y, int width) {
      place(id + 1000, x, y, width, 17);
      place(id, x, y + 18, width, 26);
    };
    constexpr int margin = 12;
    const int bar_width = size(connection_bar_).cx;
    const bool wrap_connect = bar_width < 530;
    const int endpoint_end = bar_width - (wrap_connect ? margin : 132);
    row(transport, margin, 7, 76);
    row(host, 98, 7, std::max(0, endpoint_end - 98 - 178));
    row(port, endpoint_end - 168, 7, 60);
    row(protocol, endpoint_end - 98, 7, 98);
    for (int action : {connect_button, disconnect_button})
      place(action, bar_width - 122, wrap_connect ? 58 : 23, 110, 29);

    const auto request = size(request_);
    place(request_tabs, margin, 3, request.cx - 2 * margin, 29);
    constexpr int tabs_end = 38;
    const bool show_request = task_layout_ != TaskLayout::output;
    const std::pair<RequestPage, UiPanel *> pages[] = {{RequestPage::message, &composer_},
                                                       {RequestPage::topics, &topics_},
                                                       {RequestPage::authorization, &auth_},
                                                       {RequestPage::settings, &settings_}};
    for (auto [page, panel] : pages) {
      const bool visible = page == request_page_;
      panel->ShowWindow(visible ? SW_SHOWNA : SW_HIDE);
      if (visible)
        panel->MoveWindow(0, scale(tabs_end), scale(request.cx),
                          scale(std::max(0L, request.cy - tabs_end)), TRUE);
    }

    if (show_request && request_page_ == RequestPage::message) {
      const bool wrap = size(composer_).cx < 540;
      const bool wrap_tools = size(composer_).cx < 365;
      const int options_y = wrap ? 61 : 24;
      const int format_y = options_y + 40;
      const int payload_y = format_y + (wrap_tools ? 75 : 37);
      composer_.extent(scale(payload_y + 80));
      const auto compose = size(composer_);
      const int right = compose.cx - margin;
      row(topic, margin, 8, right - margin - (wrap ? 0 : 283));
      place(pub_qos, wrap ? margin : right - 271, options_y, 71, 26);
      place(retain, wrap ? 94 : right - 186, options_y, 78, 26);
      place(publish_button, right - 106, options_y - 1, 106, 29);
      place(mode, margin, format_y, 126, 26);
      place(validate_button, wrap_tools ? margin : 149, format_y + (wrap_tools ? 32 : 0), 109, 26);
      place(format_button, wrap_tools ? 132 : 267, format_y + (wrap_tools ? 32 : 0), 103, 26);
      place(payload, margin, payload_y, right - margin,
            std::max(68, static_cast<int>(compose.cy) - payload_y - margin));
    } else if (show_request && request_page_ == RequestPage::topics) {
      const bool saved = !saved_topics_.empty();
      const bool wrap = size(topics_).cx < 510;
      const int actions_y = wrap ? 59 : 24;
      const int saved_y = actions_y + 40;
      const int heading_y = saved ? saved_y + 54 : saved_y;
      const int list_top = heading_y + 23;
      topics_.extent(scale(list_top + 63));
      const auto area = size(topics_);
      const int right = area.cx - margin;
      row(filter, margin, 8, right - margin - (wrap ? 0 : 287));
      place(sub_qos, wrap ? margin : right - 275, actions_y, 71, 26);
      place(subscribe_button, right - 190, actions_y - 1, 90, 28);
      place(unsubscribe_button, right - 92, actions_y - 1, 92, 28);
      field(saved_topic_picker, saved);
      if (saved) row(saved_topic_picker, margin, saved_y, right - margin);
      place(subscription_title, margin, heading_y, right - margin, 18);
      place(subscriptions, margin, list_top, right - margin,
            std::max(50, static_cast<int>(area.cy) - list_top - margin));
    } else if (show_request && request_page_ == RequestPage::authorization) {
      auth_.extent(scale(68));
      const int width = size(auth_).cx - 3 * margin;
      row(username, margin, 10, width / 2);
      row(password, 2 * margin + width / 2, 10, width - width / 2);
    } else if (show_request && request_page_ == RequestPage::settings) {
      const int choice = selection(transport);
      const bool websocket = choice >= 2;
      const bool tls = choice == 1 || choice == 3;
      field(path, websocket);
      for (int id : {ca, cert, key})
        field(id, tls);
      settings_.extent(scale(68 + (websocket ? 52 : 0) + (tls ? 104 : 0)));
      const int width = size(settings_).cx - 2 * margin;
      int y = 10;
      row(client_id, margin, y, width);
      y += 52;
      if (websocket) {
        row(path, margin, y, width);
        y += 52;
      }
      if (tls) {
        row(ca, margin, y, width);
        const int half = (width - margin) / 2;
        row(cert, margin, y + 52, half);
        row(key, 2 * margin + half, y + 52, width - margin - half);
      }
    }

    const auto history = size(history_panel_), detail = size(detail_panel_);
    const int controls_y = history.cx < 386 ? 30 : 4;
    const int list_y = controls_y + 30;
    place(history_title, 0, 0, history.cx, 34);
    show(message_filter + 1000, history.cx >= 374);
    place(message_filter + 1000, history.cx - 356, controls_y, 42, 25);
    place(message_filter, std::max(0L, history.cx - 310), controls_y, 124, 26);
    place(ping_button, std::max(0L, history.cx - 176), controls_y, 58, 25);
    place(clear_button, std::max(0L, history.cx - 112), controls_y, 104, 25);
    place(records, 0, list_y, history.cx, std::max(0L, history.cy - list_y));
    place(detail_title, 0, 0, detail.cx, 26);
    place(details, 8, 32, detail.cx - 16, std::max(0L, detail.cy - 40));
  }

  void MainWindow::refresh_theme() {
    theme_ = current_theme();
    for (auto *panel : {&connection_bar_, &request_, &composer_, &topics_, &auth_, &settings_,
                        &history_panel_, &detail_panel_}) {
      panel->paper = theme_.paper;
      if (panel->IsWindow()) panel->Invalidate(TRUE);
    }
    if (controls_.count(records)) {
      ListView_SetBkColor(controls_.at(records), theme_.paper);
      ListView_SetTextBkColor(controls_.at(records), theme_.paper);
      ListView_SetTextColor(controls_.at(records), theme_.ink);
    }
    if (IsWindow())
      ::RedrawWindow(m_hWnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
  }
  LRESULT MainWindow::OnPanelLayout(UINT, WPARAM, LPARAM, BOOL &) {
    layout_panels();
    return 0;
  }
  LRESULT MainWindow::OnErase(UINT, WPARAM wparam, LPARAM, BOOL &) {
    RECT rect{};
    GetClientRect(&rect);
    ::FillRect(reinterpret_cast<HDC>(wparam), &rect,
               color_brush(reinterpret_cast<HDC>(wparam), theme_.ink, theme_.surface));
    return 1;
  }
  LRESULT MainWindow::OnControlColor(UINT message, WPARAM wparam, LPARAM lparam, BOOL &) {
    const int id = ::GetDlgCtrlID(reinterpret_cast<HWND>(lparam));
    const bool section = id == history_title || id == detail_title || id == message_filter + 1000 ||
                         id == status || id == counters;
    COLORREF ink = id == counters || id == detail_title ? theme_.muted : theme_.ink;
    if (theme_.system) {
      const bool face = section || message == WM_CTLCOLORBTN;
      return reinterpret_cast<LRESULT>(color_brush(reinterpret_cast<HDC>(wparam),
                                                   face ? theme_.chrome_text : theme_.ink,
                                                   face ? theme_.surface : theme_.paper));
    }
    if (!::IsWindowEnabled(reinterpret_cast<HWND>(lparam))) ink = theme_.disabled;
    return reinterpret_cast<LRESULT>(
        color_brush(reinterpret_cast<HDC>(wparam), ink, section ? theme_.surface : theme_.paper));
  }
  LRESULT MainWindow::OnTheme(UINT, WPARAM, LPARAM, BOOL &) {
    try {
      refresh_theme();
    } catch (const std::exception &error) {
      ::SetWindowTextA(controls_.at(status), error.what());
      session_.close();
    }
    return 0;
  }
} // namespace flowie::app
