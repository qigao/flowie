#include "main_workbench.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace flowie::app {
  namespace {
    const wchar_t *state_label(State state, bool request) {
      switch (state) {
      case State::idle:
        return request ? L"Ready" : L"Disconnected";
      case State::connecting:
        return request ? L"Sending" : L"Connecting";
      case State::connected:
        return L"Connected";
      case State::closing:
        return L"Closing";
      case State::close_failed:
        return L"Close failed";
      }
      std::terminate();
    }
  } // namespace
  MainWindow &WorkbenchWindow::add(ClientRole role, std::wstring name) {
    if (closing_) throw std::logic_error("The workbench is closing");
    if (clients_.size() == kMaxClients) throw std::length_error("Maximum of eight clients reached");
    if (next_id_ == (std::numeric_limits<uint64_t>::max)())
      throw std::overflow_error("Client IDs exhausted");
    const auto id = next_id_++;
    if (name.empty())
      name = (role == ClientRole::publisher    ? L"Publisher "
              : role == ClientRole::subscriber ? L"Subscriber "
                                               : L"Client ") +
             std::to_wstring(id);
    const auto identifier =
        L"workbench-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(id);
    auto &client = add_tab(std::make_unique<Client>(*this, role, identifier, std::move(name)));
    return std::get<MainWindow>(client.window);
  }

  ProtocolWindow &WorkbenchWindow::add_tool(ToolKind kind, std::wstring name) {
    if (name.empty()) name = std::wstring(tool_label(kind)) + L" " + std::to_wstring(next_id_);
    auto &client = add_tab(std::make_unique<Client>(kind, std::move(name)));
    if (next_id_ != (std::numeric_limits<uint64_t>::max)()) ++next_id_;
    return std::get<ProtocolWindow>(client.window);
  }

  size_t WorkbenchWindow::request_count() const {
    return std::count_if(clients_.begin(), clients_.end(),
                         [](const auto &tab) { return !tab->manager(); });
  }

  WorkbenchWindow::Client &WorkbenchWindow::add_tab(std::unique_ptr<Client> client) {
    if (opening_ || closing_ || (!client->manager() && !can_add()))
      throw std::runtime_error("The workspace cannot open another tab.");
    const auto &name = client->label;
    RECT rect{};
    GetClientRect(&rect);
    rect.top = scale(70);
    constexpr DWORD style = WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    const auto created = std::visit(
        [&](auto &pane) {
          return pane.Create(m_hWnd, rect, name.c_str(), style, WS_EX_CONTROLPARENT);
        },
        client->window);
    if (!created) throw std::runtime_error("Could not create request window");
    const auto close_label = L"Close " + name;
    client->close_button =
        ::CreateWindowExW(0, L"BUTTON", close_label.c_str(),
                          WS_CHILD | WS_TABSTOP | WS_CLIPSIBLINGS | BS_OWNERDRAW | BS_NOTIFY, 0, 0,
                          scale(kTabCloseSize), scale(kTabCloseSize), m_hWnd,
                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(tab_close)),
                          _Module.GetModuleInstance(), nullptr);
    if (!client->close_button) {
      ::DestroyWindow(client->hwnd());
      throw std::runtime_error("Could not create tab close button");
    }
    ::SendMessageW(client->close_button, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    const auto caption = padded_tab_label(name);
    TCITEMW item{};
    item.mask = TCIF_TEXT | TCIF_PARAM;
    item.pszText = const_cast<wchar_t *>(caption.c_str());
    item.lParam = reinterpret_cast<LPARAM>(client->close_button);
    if (TabCtrl_InsertItem(tabs_, static_cast<int>(clients_.size()), &item) == -1) {
      ::DestroyWindow(client->hwnd()); // No connection has been admitted during creation.
      throw std::runtime_error("Could not create client tab");
    }
    TOOLINFOW tool{sizeof(tool)};
    tool.hwnd = m_hWnd;
    tool.uId = reinterpret_cast<UINT_PTR>(client->close_button);
    tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    tool.lpszText = LPSTR_TEXTCALLBACKW;
    if (!::SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool))) {
      TabCtrl_DeleteItem(tabs_, static_cast<int>(clients_.size()));
      ::DestroyWindow(client->hwnd());
      throw std::runtime_error("Could not create tab close tooltip");
    }
    clients_.push_back(std::move(client)); // capacity was reserved before creating any HWND
    activate(clients_.size() - 1);
    return *clients_.back();
  }
  void WorkbenchWindow::activate(size_t index) {
    if (index >= clients_.size()) throw std::out_of_range("Invalid client tab");
    active_ = index;
    for (size_t i = 0; i < clients_.size(); ++i)
      if (::IsWindow(clients_[i]->hwnd())) {
        ::ShowWindow(clients_[i]->hwnd(), i == active_ ? SW_SHOW : SW_HIDE);
        ::InvalidateRect(clients_[i]->close_button, nullptr, FALSE);
      }
    layout();
    update();
    // Scroll the selection into view after its status caption sets the final tab width.
    TabCtrl_SetCurSel(tabs_, static_cast<int>(index));
  }
  void WorkbenchWindow::update() {
    for (size_t i = 0; i < clients_.size(); ++i) {
      auto &client = *clients_[i];
      const auto *tool = std::get_if<ProtocolWindow>(&client.window);
      auto label = client.label;
      if (!client.manager())
        label +=
            std::wstring(L" / ") + state_label(client.state(), tool && request_tool(tool->kind()));
      if (label != client.shown_label) {
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        const auto caption = padded_tab_label(label);
        item.pszText = const_cast<wchar_t *>(caption.c_str());
        if (!TabCtrl_SetItem(tabs_, static_cast<int>(i), &item))
          throw std::runtime_error("Could not update client tab");
        client.shown_label = label;
      }
      const bool closable = can_close(client);
      ::EnableWindow(client.close_button, closable);
      const auto close_label = (client.state() == State::close_failed ? L"Retry closing "
                                : client.closing()                    ? L"Closing "
                                                                      : L"Close ") +
                               client.label;
      wchar_t previous[96]{};
      ::GetWindowTextW(client.close_button, previous, 96);
      if (close_label != previous) ::SetWindowTextW(client.close_button, close_label.c_str());
    }
    ::EnableWindow(new_request_, can_add());
    update_request_toolbar();
    update_collection_actions();
    RECT status{};
    ::GetClientRect(summary_, &status);
    const bool compact = status.right < scale(112);
    const auto summary = closing_           ? std::wstring(L"Closing...")
                         : clients_.empty() ? std::wstring(compact ? L"" : L"No open tabs")
                                            : std::to_wstring(request_count()) +
                                                  (compact ? L" / 8" : L" / 8 requests");
    if (!has_error_ || closing_) ::SetWindowTextW(summary_, summary.c_str());
    position_close_buttons();
  }
  void WorkbenchWindow::layout() {
    RECT rect{};
    GetClientRect(&rect);
    const int width = MulDiv(rect.right, 96, dpi_);
    auto place = [&](HWND control, int x, int y, int w, int h) {
      ::MoveWindow(control, scale(x), scale(y), scale(w), scale(h), TRUE);
    };
    place(new_request_, 12, 4, 130, 28);
    place(browse_collections_, 146, 4, 150, 28);
    place(save_request_, 300, 4, 123, 28);
    place(primary_, 435, 4, 144, 28);
    place(stop_, 583, 4, 134, 28);
    place(request_actions_, 721, 4, 104, 28);
    place(summary_, 838, 9, std::max(0, width - 848), 24);
    const int left = collections_width();
    ::MoveWindow(tabs_, left, scale(38), std::max(0L, rect.right - left), scale(30), TRUE);
    const auto height = std::max(0L, rect.bottom - scale(70));
    layout_collections(std::max(0L, rect.bottom - scale(38)));
    const auto pane_width = std::max(0L, rect.right - left);
    for (const auto &client : clients_)
      if (::IsWindow(client->hwnd())) {
        ::MoveWindow(client->hwnd(), left, scale(70), pane_width, height, TRUE);
        if (client->manager()) layout_collections_manager(client->hwnd(), pane_width, height);
      }
    position_close_buttons();
  }
  void WorkbenchWindow::close_all() noexcept {
    closing_ = true;
    for (const auto &client : clients_)
      if (::IsWindow(client->hwnd())) ::SendMessageW(client->hwnd(), WM_CLOSE, 0, 0);
  }
  LRESULT WorkbenchWindow::OnCreate(UINT, WPARAM, LPARAM, BOOL &) {
    try {
      clients_.reserve(kMaxClients + 1);
      HDC dc = GetDC();
      dpi_ = GetDeviceCaps(dc, LOGPIXELSX);
      ReleaseDC(dc);
      NONCLIENTMETRICSW metrics{sizeof(metrics)};
      if (!::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
        throw std::runtime_error("Could not read Windows UI font");
      font_ = ::CreateFontIndirectW(&metrics.lfMessageFont);
      theme_ = current_theme();
      if (!font_) throw std::runtime_error("Could not create workspace font");
      auto create = [&](Id id, const wchar_t *kind, const wchar_t *label, DWORD style) {
        HWND control = ::CreateWindowExW(0, kind, label, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0,
                                         m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                         _Module.GetModuleInstance(), nullptr);
        if (!control) throw std::runtime_error("Could not create workspace control");
        ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        return control;
      };
      new_request_ = create(choose_new_request, L"BUTTON", L"&New Request...", WS_TABSTOP);
      browse_collections_ =
          create(browse_collections, L"BUTTON", L"Open Collections...", WS_TABSTOP);
      save_request_ = create(save_request, L"BUTTON", L"Save Request", WS_TABSTOP);
      primary_ = create(primary_request, L"BUTTON", L"Run", WS_TABSTOP);
      stop_ = create(disconnect_active, L"BUTTON", L"Stop", WS_TABSTOP);
      request_actions_ = create(request_actions, L"BUTTON", L"Actions...", WS_TABSTOP);
      summary_ = create(workspace_status, L"STATIC", L"", 0);
      tabs_ = create(client_tabs, WC_TABCONTROLW, L"Requests",
                     WS_TABSTOP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | TCS_SINGLELINE |
                         TCS_FOCUSONBUTTONDOWN);
      style_tabs(tabs_, theme_);
      if (!::SetWindowSubclass(tabs_, TabProc, 1, reinterpret_cast<DWORD_PTR>(this)))
        throw std::runtime_error("Could not attach tab layout handler");
      TabCtrl_SetItemSize(tabs_, 0, scale(26));
      HDC tab_dc = ::GetDC(tabs_);
      SIZE space{};
      HGDIOBJ old_font = ::SelectObject(tab_dc, font_);
      const BOOL measured = ::GetTextExtentPoint32W(tab_dc, L" ", 1, &space);
      ::SelectObject(tab_dc, old_font);
      ::ReleaseDC(tabs_, tab_dc);
      if (!measured || space.cx <= 0)
        throw std::runtime_error("Could not measure tab close gutter");
      const int gutter = scale(kTabCloseSize + 2 * kTabCloseInset);
      tab_padding_.assign(static_cast<size_t>((gutter + space.cx - 1) / space.cx), L' ');
      tooltip_ = ::CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                   WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0, m_hWnd,
                                   nullptr, _Module.GetModuleInstance(), nullptr);
      if (!tooltip_) throw std::runtime_error("Could not create tab close tooltips");
      create_menu();
      create_collections();
      update();
      if (!SetTimer(2, 50)) throw std::runtime_error("Could not start workspace timer");
      layout();
      return 0;
    } catch (const std::exception &error) {
      ::MessageBoxA(m_hWnd, error.what(), "Flowie Workbench", MB_OK | MB_ICONERROR);
      return -1;
    }
  }
  LRESULT WorkbenchWindow::OnTimer(UINT, WPARAM, LPARAM, BOOL &) {
    if (faulted_) {
      if (!opening_ && std::all_of(clients_.begin(), clients_.end(), [](const auto &client) {
            return !::IsWindow(client->hwnd());
          })) {
        clients_.clear();
        DestroyWindow();
      }
      return 0; // Observe drain without retrying a failed native tab operation.
    }
    try {
      for (size_t i = clients_.size(); i-- > 0;) {
        if (::IsWindow(clients_[i]->hwnd())) continue;
        if (!TabCtrl_DeleteItem(tabs_, static_cast<int>(i)))
          throw std::runtime_error("Could not remove closed client tab");
        TOOLINFOW tool{sizeof(tool)};
        tool.hwnd = m_hWnd;
        tool.uId = reinterpret_cast<UINT_PTR>(clients_[i]->close_button);
        ::SendMessageW(tooltip_, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        clients_.erase(clients_.begin() + i); // child HWND and SDK callbacks have both drained
        if (i < active_) --active_;
        if (!clients_.empty() && active_ >= clients_.size()) active_ = clients_.size() - 1;
        if (!clients_.empty()) activate(active_);
      }
      update();
      if (closing_ && clients_.empty()) DestroyWindow();
    } catch (const std::exception &error) {
      faulted_ = true;
      close_all();
      ::MessageBoxA(m_hWnd, error.what(), "Flowie Workbench", MB_OK | MB_ICONERROR);
    }
    return 0;
  }
  LRESULT WorkbenchWindow::OnCommand(UINT, WPARAM wparam, LPARAM lparam, BOOL &) {
    const int id = LOWORD(wparam);
    if (id == collection_files && HIWORD(wparam) == LBN_SELCHANGE) {
      update_collection_actions();
      return 0;
    }
    if (HIWORD(wparam) != BN_CLICKED && !(id == sidebar_history && HIWORD(wparam) == LBN_DBLCLK))
      return 0;
    try {
      has_error_ = false;
      if (const auto action = request_action(id)) {
        invoke_request(*action);
        update();
        return 0;
      }
      switch (id) {
      case browse_collections:
        choose_collections_file();
        break;
      case manage_collections:
        open_collections_manager();
        break;
      case reload_collection_file:
      case unload_collection_file:
        change_collection_file(id == unload_collection_file);
        break;
      case sidebar_history:
        open_sidebar_history();
        break;
      case choose_new_request:
        if (can_add()) show_toolbar_menu(new_request_, new_tool_menu_);
        break;
      case primary_request:
        invoke_request(shown_primary_);
        break;
      case request_actions:
        if (request_command(RequestAction::sample).enabled)
          show_toolbar_menu(request_actions_, request_menu_);
        break;
      case add_publisher:
        open_connection(ClientRole::publisher);
        break;
      case add_subscriber:
        open_connection(ClientRole::subscriber);
        break;
      case add_client:
        open_connection(ClientRole::both);
        break;
      case add_http:
      case add_rpc:
      case add_tcp:
      case add_serial:
        if (can_add()) add_tool(static_cast<ToolKind>(id - add_http));
        break;
      case import_connection_file:
        choose_import(true);
        break;
      case save_request:
        save_active_request();
        break;
      case open_saved_request:
        open_selected_request();
        break;
      case toggle_collections:
        collections_visible_ = !collections_visible_;
        layout();
        break;
      case view_workspace:
      case view_publish:
      case view_monitor:
        set_task_layout(id == view_workspace ? TaskLayout::workspace
                        : id == view_publish ? TaskLayout::editor
                                             : TaskLayout::output);
        break;
      case exit_workbench:
        SendMessage(WM_CLOSE);
        return 0;
      case close_client:
        if (!clients_.empty()) close_client_at(active_);
        break;
      case tab_close:
        for (size_t i = 0; i < clients_.size(); ++i)
          if (clients_[i]->close_button == reinterpret_cast<HWND>(lparam)) {
            close_client_at(i);
            break;
          }
        break;
      default:
        if (id >= first_recent && static_cast<size_t>(id - first_recent) < menu_recent_.size())
          show_connection(menu_recent_[id - first_recent].role, menu_recent_, id - first_recent);
        break;
      }
      update();
    } catch (const std::exception &error) {
      has_error_ = true;
      ::SetWindowTextA(summary_, error.what());
    }
    return 0;
  }
  LRESULT WorkbenchWindow::OnNotify(UINT, WPARAM, LPARAM lparam, BOOL &) {
    const auto &notification = *reinterpret_cast<NMHDR *>(lparam);
    if (notification.hwndFrom == collections_tree_ || notification.hwndFrom == sidebar_tabs_)
      return on_collection_notice(notification);
    if (notification.hwndFrom == tooltip_ && notification.code == TTN_GETDISPINFOW) {
      auto &tip = *reinterpret_cast<NMTTDISPINFOW *>(lparam);
      ::GetWindowTextW(reinterpret_cast<HWND>(notification.idFrom), tip.szText, 80);
      tip.lpszText = tip.szText;
      return 0;
    }
    if (notification.idFrom == tab_close && notification.code == BCN_HOTITEMCHANGE) {
      const auto &hot = *reinterpret_cast<NMBCHOTITEM *>(lparam);
      for (const auto &client : clients_)
        if (client->close_button == notification.hwndFrom) {
          client->close_hot = (hot.dwFlags & HICF_ENTERING) != 0;
          ::InvalidateRect(client->close_button, nullptr, TRUE);
          break;
        }
      return 0;
    }
    if (notification.code == NM_CUSTOMDRAW &&
        (notification.idFrom == choose_new_request ||
         notification.hwndFrom == browse_collections_ || notification.hwndFrom == save_request_ ||
         notification.hwndFrom == primary_ || notification.hwndFrom == stop_ ||
         notification.hwndFrom == request_actions_)) {
      const auto glyph = notification.hwndFrom == browse_collections_ ? CommandGlyph::open
                         : notification.hwndFrom == save_request_     ? CommandGlyph::save
                         : notification.idFrom == choose_new_request  ? CommandGlyph::request
                         : notification.hwndFrom == primary_          ? CommandGlyph::run
                         : notification.hwndFrom == stop_             ? CommandGlyph::stop
                                                                      : CommandGlyph::actions;
      return draw_command(*reinterpret_cast<NMCUSTOMDRAW *>(lparam), theme_, glyph, dpi_);
    }

    if (notification.idFrom == client_tabs && notification.code == TCN_SELCHANGE) {
      try {
        has_error_ = false;
        activate(static_cast<size_t>(TabCtrl_GetCurSel(tabs_)));
      } catch (const std::exception &error) {
        has_error_ = true;
        ::SetWindowTextA(summary_, error.what());
      }
    }
    return 0;
  }
  LRESULT WorkbenchWindow::OnErase(UINT, WPARAM wparam, LPARAM, BOOL &) {
    RECT rect{};
    GetClientRect(&rect);
    HDC dc = reinterpret_cast<HDC>(wparam);
    ::FillRect(dc, &rect, color_brush(dc, theme_.ink, theme_.surface));
    rect.bottom = scale(38);
    ::FillRect(dc, &rect, color_brush(dc, theme_.chrome_text, theme_.chrome));
    return 1;
  }
  LRESULT WorkbenchWindow::OnControlColor(UINT, WPARAM wparam, LPARAM lparam, BOOL &) {
    const auto id = ::GetDlgCtrlID(reinterpret_cast<HWND>(lparam));
    if (id == collection_files_title || id == collections_empty)
      return reinterpret_cast<LRESULT>(color_brush(
          reinterpret_cast<HDC>(wparam), id == collection_files_title ? theme_.ink : theme_.muted,
          id == collection_files_title ? theme_.paper : theme_.surface));
    return reinterpret_cast<LRESULT>(
        color_brush(reinterpret_cast<HDC>(wparam),
                    theme_.system ? theme_.chrome_text : theme_.chrome_muted, theme_.chrome));
  }
  LRESULT WorkbenchWindow::OnTheme(UINT message, WPARAM wparam, LPARAM lparam, BOOL &) {
    try {
      theme_ = current_theme();
      refresh_collections_theme();
      for (const auto &client : clients_)
        if (::IsWindow(client->hwnd())) ::SendMessageW(client->hwnd(), message, wparam, lparam);
      ::RedrawWindow(m_hWnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    } catch (const std::exception &error) {
      faulted_ = true;
      close_all();
      ::SetWindowTextA(summary_, error.what());
    }
    return 0;
  }
  LRESULT WorkbenchWindow::OnSize(UINT, WPARAM wparam, LPARAM, BOOL &) {
    if (wparam != SIZE_MINIMIZED && tabs_) layout();
    return 0;
  }
  LRESULT WorkbenchWindow::OnMinMax(UINT, WPARAM, LPARAM lparam, BOOL &) {
    reinterpret_cast<MINMAXINFO *>(lparam)->ptMinTrackSize = {scale(900), scale(480)};
    return 0;
  }
  LRESULT WorkbenchWindow::OnClose(UINT, WPARAM, LPARAM, BOOL &) {
    if (opening_) return 0;
    close_all();
    if (clients_.empty()) DestroyWindow();
    return 0;
  }
  LRESULT WorkbenchWindow::OnDestroy(UINT, WPARAM, LPARAM, BOOL &) {
    KillTimer(2);
    if (tabs_) ::RemoveWindowSubclass(tabs_, TabProc, 1);
    DeleteObject(font_);
    PostQuitMessage(faulted_ ? 1 : 0);
    return 0;
  }
  bool WorkbenchWindow::translate(MSG *message) {
    if (message->message == WM_KEYDOWN && message->wParam == VK_RETURN &&
        message->hwnd == sidebar_history_) {
      SendMessage(WM_COMMAND, MAKEWPARAM(sidebar_history, BN_CLICKED), 0);
      return true;
    }
    if (message->message == WM_KEYDOWN && GetKeyState(VK_CONTROL) < 0 && message->wParam == 'S') {
      SendMessage(WM_COMMAND, MAKEWPARAM(save_request, BN_CLICKED), 0);
      return true;
    }
    if (message->message == WM_KEYDOWN && GetKeyState(VK_CONTROL) < 0 && message->wParam == 'N') {
      SendMessage(WM_COMMAND, MAKEWPARAM(choose_new_request, BN_CLICKED), 0);
      return true;
    }
    if (message->message == WM_KEYDOWN && GetKeyState(VK_CONTROL) < 0 &&
        (message->wParam == VK_RETURN || message->wParam == 'D')) {
      const auto action = message->wParam == VK_RETURN ? publish_active : disconnect_active;
      SendMessage(WM_COMMAND, MAKEWPARAM(action, BN_CLICKED), 0);
      return true;
    }
    if (message->message == WM_KEYDOWN && GetKeyState(VK_CONTROL) < 0 && message->wParam == 'W') {
      SendMessage(WM_COMMAND, MAKEWPARAM(close_client, BN_CLICKED), 0);
      return true;
    }
    if (!clients_.empty() && ::IsWindow(clients_[active_]->hwnd())) {
      if (message->message == WM_KEYDOWN && GetKeyState(VK_CONTROL) < 0 &&
          (message->wParam == VK_PRIOR || message->wParam == VK_NEXT)) {
        try {
          const auto advance = message->wParam == VK_NEXT ? size_t(1) : clients_.size() - 1;
          activate((active_ + advance) % clients_.size());
        } catch (const std::exception &error) {
          ::SetWindowTextA(summary_, error.what());
        }
        return true;
      }
      auto &tab = *clients_[active_];
      if (message->hwnd == tab.hwnd() || ::IsChild(tab.hwnd(), message->hwnd)) {
        if (auto *mqtt = std::get_if<MainWindow>(&tab.window)) return mqtt->translate(message);
        if (auto *tool = std::get_if<ProtocolWindow>(&tab.window)) return tool->translate(message);
      }
    }
    return ::IsDialogMessageW(m_hWnd, message) != FALSE;
  }
} // namespace flowie::app
