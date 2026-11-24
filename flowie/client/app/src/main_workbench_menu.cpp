#include "main_workbench.h"
#include "ui/connection_dialog.h"
#include "ui/connection_resource.h"
#include "ui/import_files.h"
#include <algorithm>
#include <atluser.h>
#include <stdexcept>
#include <system_error>

namespace flowie::app {
  namespace {
    constexpr UINT kRecentMenuPosition = 4;
    constexpr UINT kRecentCollectionsMenuPosition = 2;
    struct MenuAction {
      WorkbenchWindow::Id id;
      RequestAction action;
      const wchar_t *label;
    };
    constexpr MenuAction session_actions[] = {
        {WorkbenchWindow::connect_active, RequestAction::open, L"Connect / Open"},
        {WorkbenchWindow::disconnect_active, RequestAction::stop, L"Stop"},
        {WorkbenchWindow::refresh_ports_active, RequestAction::refresh_ports, L"Refresh Ports"},
    };
    constexpr MenuAction request_menu_actions[] = {
        {WorkbenchWindow::import_sample_file, RequestAction::sample, L"Load Sample..."},
        {WorkbenchWindow::validate_active, RequestAction::validate_json, L"Validate JSON"},
        {WorkbenchWindow::format_active, RequestAction::format_json, L"Format JSON"},
        {WorkbenchWindow::publish_active, RequestAction::send, L"Send"},
    };
    void append(HMENU menu, UINT flags, UINT_PTR id, const wchar_t *label) {
      if (!AppendMenuW(menu, flags, id, label))
        throw std::runtime_error("Could not build workspace menu");
    }
    void clear_recent_menu(HMENU menu) {
      for (int pos = GetMenuItemCount(menu); pos-- > 0;)
        if (!DeleteMenu(menu, pos, MF_BYPOSITION))
          throw std::runtime_error("Could not refresh workspace history menu.");
    }
    void append_recent(HMENU menu, UINT_PTR id, std::wstring label) {
      // Win32 treats ampersands as menu mnemonics.
      for (size_t amp = 0; (amp = label.find(L'&', amp)) != label.npos; amp += 2)
        label.insert(amp, 1, L'&');
      append(menu, MF_STRING, id, label.c_str());
    }
  } // namespace

  void WorkbenchWindow::create_menu() {
    CMenu bar, file, connection, recent, recent_collections, message, view, new_tool, mqtt;
    if (!bar.CreateMenu() || !file.CreatePopupMenu() || !connection.CreatePopupMenu() ||
        !recent.CreatePopupMenu() || !message.CreatePopupMenu() || !view.CreatePopupMenu() ||
        !new_tool.CreatePopupMenu() || !mqtt.CreatePopupMenu() ||
        !recent_collections.CreatePopupMenu())
      throw std::runtime_error("Could not create workspace menu");
    const auto add_popup = [](HMENU parent, CMenu &child, const wchar_t *label) {
      const auto handle = child.m_hMenu;
      append(parent, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(handle), label);
      child.Detach();
      return handle;
    };
    append(mqtt, MF_STRING, add_client, L"&Client...");
    append(mqtt, MF_STRING, add_publisher, L"&Publisher...");
    append(mqtt, MF_STRING, add_subscriber, L"&Subscriber...");
    append(mqtt, MF_SEPARATOR, 0, nullptr);
    append(mqtt, MF_STRING, import_connection_file, L"&Load Connection Profile...");
    mqtt_menu_ = add_popup(new_tool, mqtt, L"&MQTT");
    append(new_tool, MF_STRING, add_http, L"&HTTP / HTTPS");
    append(new_tool, MF_STRING, add_rpc, L"&JSON-RPC");
    append(new_tool, MF_STRING, add_tcp, L"&TCP");
    append(new_tool, MF_STRING, add_serial, L"&Serial");
    new_tool_menu_ = add_popup(file, new_tool, L"&New Request\tCtrl+N");
    append(file, MF_STRING, browse_collections, L"Open &Collections...");
    append(recent_collections, MF_STRING | MF_GRAYED, 0, L"No recent collections");
    recent_collections_menu_ = add_popup(file, recent_collections, L"Recent Co&llections");
    append(file, MF_STRING, manage_collections, L"&Manage Collections");
    append(file, MF_STRING, run_collection, L"&Run Collection");
    append(file, MF_STRING, save_request, L"&Save Request...\tCtrl+S");
    append(file, MF_SEPARATOR, 0, nullptr);
    append(file, MF_STRING, close_client, L"&Close Tab\tCtrl+W");
    append(file, MF_STRING, exit_workbench, L"E&xit");

    for (const auto &item : session_actions)
      append(connection, MF_STRING, item.id, item.label);
    append(connection, MF_SEPARATOR, 0, nullptr);
    append(recent, MF_STRING | MF_GRAYED, 0, L"No recent connections");
    recent_menu_ = add_popup(connection, recent, L"Recent &MQTT Connections");

    for (const auto &item : request_menu_actions)
      append(message, MF_STRING, item.id, item.label);

    append(view, MF_STRING, view_workspace, L"&Workspace");
    append(view, MF_STRING, view_publish, L"&Editor");
    append(view, MF_STRING, view_monitor, L"&Output");
    append(view, MF_SEPARATOR, 0, nullptr);
    append(view, MF_STRING, toggle_collections, L"&Sidebar");

    file_menu_ = add_popup(bar, file, L"&File");
    session_menu_ = add_popup(bar, connection, L"&Session");
    request_menu_ = add_popup(bar, message, L"&Request");
    view_menu_ = add_popup(bar, view, L"&View");
    if (!SetMenu(bar)) throw std::runtime_error("Could not attach workspace menu");
    bar.Detach(); // The window now owns the menu and its submenus.
  }

  WorkbenchWindow::Client *WorkbenchWindow::active_tab() const {
    if (closing_ || clients_.empty()) return nullptr;
    auto *tab = clients_[active_].get();
    return !tab->manager() && ::IsWindow(tab->hwnd()) && !tab->closing() ? tab : nullptr;
  }
  RequestCommand WorkbenchWindow::request_command(RequestAction action) const {
    const auto *tab = active_tab();
    if (!tab || opening_) return {};
    if (const auto *mqtt = std::get_if<MainWindow>(&tab->window))
      return mqtt->request_command(action);
    return std::get<ProtocolWindow>(tab->window).request_command(action);
  }

  RequestAction WorkbenchWindow::primary_action() const {
    const auto open = request_command(RequestAction::open);
    const auto send = request_command(RequestAction::send);
    return open.enabled || (!send.label && open.label) ? RequestAction::open : RequestAction::send;
  }

  std::optional<RequestAction> WorkbenchWindow::request_action(int id) {
    for (const auto &item : session_actions)
      if (item.id == id) return item.action;
    for (const auto &item : request_menu_actions)
      if (item.id == id) return item.action;
    return std::nullopt;
  }

  void WorkbenchWindow::invoke_request(RequestAction action) {
    if (!request_command(action).enabled) return;
    if (action == RequestAction::sample) {
      choose_import(false);
    } else if (auto *tab = active_tab()) {
      if (auto *mqtt = std::get_if<MainWindow>(&tab->window)) mqtt->invoke_request(action);
      else std::get<ProtocolWindow>(tab->window).invoke_request(action);
    }
  }

  std::optional<TaskLayout> WorkbenchWindow::task_layout() const {
    const auto *tab = active_tab();
    if (!tab || opening_) return std::nullopt;
    if (const auto *mqtt = std::get_if<MainWindow>(&tab->window)) return mqtt->task_layout();
    const auto &tool = std::get<ProtocolWindow>(tab->window);
    return tool.runner() ? std::nullopt : std::optional{tool.task_layout()};
  }

  void WorkbenchWindow::set_task_layout(TaskLayout layout) {
    if (!task_layout()) return;
    auto *tab = active_tab();
    if (auto *mqtt = std::get_if<MainWindow>(&tab->window)) mqtt->set_task_layout(layout);
    else std::get<ProtocolWindow>(tab->window).set_task_layout(layout);
  }

  void WorkbenchWindow::create_toolbar() {
    constexpr DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS |
                            TBSTYLE_FLAT | TBSTYLE_LIST | TBSTYLE_TOOLTIPS |
                            CCS_NOPARENTALIGN | CCS_NORESIZE | CCS_NODIVIDER;
    if (!toolbar_.Create(m_hWnd, nullptr, L"Workbench commands", style, 0, command_toolbar))
      throw std::runtime_error("Could not create the Workbench toolbar.");
    toolbar_.SetButtonStructSize();
    toolbar_.SetFont(font_);
    toolbar_.SetExtendedStyle(TBSTYLE_EX_MIXEDBUTTONS | TBSTYLE_EX_DRAWDDARROWS);
    toolbar_.SetMaxTextRows(1);
    toolbar_.SetIndent(scale(6));
    toolbar_.SetPadding(scale(10), scale(8));
    const int images = toolbar_.LoadStdImages(IDB_STD_SMALL_COLOR);
    if (images < 0) throw std::runtime_error("Could not load standard toolbar images.");
    const auto button = [](Id id, const wchar_t *caption, int image, BYTE style = BTNS_BUTTON) {
      TBBUTTON item{};
      item.iBitmap = image;
      item.idCommand = id;
      item.fsState = TBSTATE_ENABLED;
      item.fsStyle = static_cast<BYTE>(style | BTNS_AUTOSIZE | BTNS_SHOWTEXT);
      item.iString = reinterpret_cast<INT_PTR>(caption);
      return item;
    };
    TBBUTTON separator{};
    separator.iBitmap = scale(8);
    separator.fsStyle = BTNS_SEP;
    const TBBUTTON commands[] = {
        button(choose_new_request, L"&New Request", images + STD_FILENEW, BTNS_WHOLEDROPDOWN),
        button(browse_collections, L"Open Collections…", images + STD_FILEOPEN),
        button(save_request, L"Save Request", images + STD_FILESAVE),
        separator,
        button(primary_request, L"Run", I_IMAGENONE),
        button(disconnect_active, L"Stop", I_IMAGENONE),
        button(request_actions, L"Actions", images + STD_PROPERTIES, BTNS_WHOLEDROPDOWN),
    };
    if (!toolbar_.AddButtons(static_cast<int>(std::size(commands)), commands))
      throw std::runtime_error("Could not add the Workbench toolbar commands.");
  }

  void WorkbenchWindow::layout_toolbar(int width) {
    toolbar_.AutoSize();
    SIZE commands{};
    if (!toolbar_.GetMaxSize(&commands))
      throw std::runtime_error("Could not measure the Workbench toolbar.");
    const int toolbar_width = std::min(width, static_cast<int>(commands.cx) + scale(6));
    const int toolbar_height = scale(38);
    const int y = std::max(0, (toolbar_height - static_cast<int>(commands.cy)) / 2);
    ::MoveWindow(toolbar_, 0, y, toolbar_width, toolbar_height - y, TRUE);
    const int summary_x = toolbar_width + scale(12);
    ::MoveWindow(summary_, summary_x, scale(10), std::max(0, width - summary_x - scale(8)),
                 scale(21), TRUE);
  }

  void WorkbenchWindow::show_toolbar_menu(Id command, HMENU menu) {
    const auto *tab = active_tab();
    const HWND target = tab ? tab->hwnd() : nullptr;
    RECT bounds{};
    if (!toolbar_.GetRect(command, &bounds))
      throw std::runtime_error("Could not locate the toolbar menu.");
    ::MapWindowPoints(toolbar_, nullptr, reinterpret_cast<POINT *>(&bounds), 2);
    const auto action = ::TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_RETURNCMD, bounds.left,
                                         bounds.bottom, 0, m_hWnd, nullptr);
    if (!action) return;
    if (menu == request_menu_) {
      const auto *current = active_tab();
      if (!current || current->hwnd() != target) return;
    }
    SendMessage(WM_COMMAND, MAKEWPARAM(action, BN_CLICKED));
  }

  void WorkbenchWindow::update_request_toolbar() {
    const auto enable = [&](Id id, bool active) {
      if ((toolbar_.IsButtonEnabled(id) != FALSE) != active &&
          !toolbar_.EnableButton(id, active))
        throw std::runtime_error("Could not update a toolbar command.");
    };
    bool labels_changed = false;
    const auto update = [&](Id id, RequestCommand command, const wchar_t *fallback) {
      const auto label = command.label ? command.label : fallback;
      wchar_t current[64]{};
      TBBUTTONINFOW info{sizeof(info), TBIF_TEXT};
      info.pszText = current;
      info.cchText = static_cast<int>(std::size(current));
      if (toolbar_.GetButtonInfo(id, &info) < 0)
        throw std::runtime_error("Could not read a toolbar command.");
      if (std::wstring_view(current) != label) {
        info.pszText = const_cast<wchar_t *>(label);
        if (!toolbar_.SetButtonInfo(id, &info))
          throw std::runtime_error("Could not rename a toolbar command.");
        labels_changed = true;
      }
      enable(id, command.enabled);
    };
    shown_primary_ = primary_action();
    update(primary_request, request_command(shown_primary_), L"Run");
    update(disconnect_active, request_command(RequestAction::stop), L"Stop");
    enable(choose_new_request, can_add());
    enable(browse_collections, !opening_ && !closing_);
    enable(save_request, can_save_request());
    enable(request_actions, request_command(RequestAction::sample).enabled);
    if (labels_changed) {
      RECT client{};
      GetClientRect(&client);
      layout_toolbar(client.right);
    }
  }

  LRESULT WorkbenchWindow::OnMenuPopup(UINT, WPARAM wparam, LPARAM, BOOL &handled) {
    const auto popup = reinterpret_cast<HMENU>(wparam);
    const auto enable = [popup](int id, bool enabled) {
      EnableMenuItem(popup, id, MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED));
    };
    if (popup == file_menu_) {
      enable(browse_collections, !opening_ && !closing_);
      EnableMenuItem(popup, kRecentCollectionsMenuPosition,
                     MF_BYPOSITION | (!opening_ && !closing_ && !faulted_ ? MF_ENABLED : MF_GRAYED));
      enable(manage_collections, !opening_ && !closing_);
      enable(run_collection, can_run_collection());
      enable(save_request, can_save_request());
      enable(close_client, !clients_.empty() && can_close(*clients_[active_]));
      return 0;
    }
    if (popup == new_tool_menu_) {
      for (const int id : {add_http, add_rpc, add_tcp, add_serial})
        enable(id, can_add());
      EnableMenuItem(popup, 0, MF_BYPOSITION | (can_add() ? MF_ENABLED : MF_GRAYED));
      return 0;
    }
    if (popup == mqtt_menu_) {
      for (const int id : {add_client, add_publisher, add_subscriber, import_connection_file})
        enable(id, can_add());
      return 0;
    }
    if (popup == session_menu_ || popup == request_menu_) {
      const auto refresh = [&](const auto &items) {
        for (const auto &item : items) {
          const auto command = request_command(item.action);
          std::wstring label = command.label ? command.label : item.label;
          if (item.action == RequestAction::send) label += L"\tCtrl+Enter";
          if (item.action == RequestAction::stop) label += L"\tCtrl+D";
          if (!::ModifyMenuW(popup, item.id, MF_BYCOMMAND | MF_STRING, item.id, label.c_str()))
            throw std::runtime_error("Could not update the request menu.");
          enable(item.id, command.enabled);
        }
      };
      try {
        if (popup == session_menu_) {
          refresh(session_actions);
          EnableMenuItem(popup, kRecentMenuPosition,
                         MF_BYPOSITION | (can_add() ? MF_ENABLED : MF_GRAYED));
        } else refresh(request_menu_actions);
      } catch (const std::exception &error) {
        has_error_ = true;
        ::SetWindowTextA(summary_, error.what());
      }
      return 0;
    }
    if (popup == view_menu_) {
      CheckMenuItem(popup, toggle_collections,
                    MF_BYCOMMAND | (collections_visible_ ? MF_CHECKED : MF_UNCHECKED));
      const auto mode = task_layout();
      enable(view_workspace, mode.has_value());
      enable(view_publish, mode.has_value());
      enable(view_monitor, mode.has_value());
      if (mode) {
        const int selected = *mode == TaskLayout::workspace ? view_workspace
                             : *mode == TaskLayout::editor  ? view_publish
                                                            : view_monitor;
        CheckMenuRadioItem(popup, view_workspace, view_monitor, selected, MF_BYCOMMAND);
      } else {
        for (int id = view_workspace; id <= view_monitor; ++id)
          CheckMenuItem(popup, id, MF_BYCOMMAND | MF_UNCHECKED);
      }
      return 0;
    }
    if (popup != recent_menu_ && popup != recent_collections_menu_) {
      handled = FALSE;
      return 0;
    }
    try {
      if (popup == recent_collections_menu_) {
        menu_recent_collections_.clear();
        clear_recent_menu(popup);
        auto recent = read_recent_collections(collections_settings_);
        if (recent.empty()) append(popup, MF_STRING | MF_GRAYED, 0, L"No recent collections");
        for (size_t i = 0; i < recent.size(); ++i)
          append_recent(popup, first_recent_collection + i, recent[i].wstring());
        menu_recent_collections_ = std::move(recent);
      } else {
        menu_recent_.clear();
        clear_recent_menu(popup);
        auto recent = history_.load();
        if (recent.empty()) append(popup, MF_STRING | MF_GRAYED, 0, L"No recent connections");
        for (size_t i = 0; i < recent.size(); ++i)
          append_recent(popup, first_recent + i, connection_label(recent[i]));
        menu_recent_ = std::move(recent);
      }
    } catch (const std::exception &error) {
      has_error_ = true;
      ::SetWindowTextA(summary_, error.what());
    }
    return 0;
  }

  void WorkbenchWindow::open_connection(ClientRole role, const Connection *initial) {
    show_connection(role, history_.load(), -1, initial);
  }

  void WorkbenchWindow::show_connection(ClientRole role, std::vector<RecentConnection> recent,
                                        int index, const Connection *initial) {
    if (!can_add()) return;
    ConnectionDialog dialog(role, recent, index, theme_, initial);
    opening_ = true;
    const auto action = dialog.show(m_hWnd, _Module.GetModuleInstance());
    const auto error = GetLastError();
    opening_ = false;
    if (action == -1)
      throw std::system_error(static_cast<int>(error), std::system_category(),
                              "Open connection dialog");
    if (!faulted_ && !closing_ && (action == IDOK || action == IDC_OPEN_CONNECTION_TAB))
      add(role).apply_connection(dialog.connection(), action == IDOK);
  }

  void WorkbenchWindow::import_connection(const std::filesystem::path &file) {
    if (!can_add()) return;
    const auto profile = load_connection_file(file);
    open_connection(profile.role, &profile.connection);
  }

  void WorkbenchWindow::import_sample(const std::filesystem::path &file) {
    if (!request_command(RequestAction::sample).enabled)
      throw std::runtime_error("Select a request tab with a payload editor.");
    auto *tab = active_tab();
    if (auto *mqtt = std::get_if<MainWindow>(&tab->window))
      mqtt->apply_sample(load_sample_file(file));
    else std::get<ProtocolWindow>(tab->window).apply_sample(load_payload_sample(file, kToolBytes));
  }

  void WorkbenchWindow::choose_import(bool connection) {
    if (opening_ || faulted_ || closing_) return;
    const auto *tab = active_tab();
    if (connection ? !can_add() : !request_command(RequestAction::sample).enabled) return;
    const HWND target = tab ? tab->hwnd() : nullptr;
    const auto chosen = [&] {
      struct OpenPicker {
        bool &opening;
        ~OpenPicker() { opening = false; }
      } guard{opening_};
      opening_ = true;
      return choose_import_file(m_hWnd, connection);
    }();
    if (!chosen || faulted_ || closing_) return;
    if (connection) import_connection(*chosen);
    else {
      auto *current = active_tab();
      if (!current || current->hwnd() != target)
        throw std::runtime_error("The original tab closed while the file picker was open.");
      import_sample(*chosen);
    }
  }

  void WorkbenchWindow::remember_connection(const RecentConnection &entry) {
    try {
      history_.remember(entry);
      if (sidebar_page_ == 1) {
        refresh_sidebar_history();
        select_sidebar(sidebar_page_);
      }
    } catch (const std::exception &error) {
      has_error_ = true;
      ::SetWindowTextA(summary_, error.what());
    }
  }
} // namespace flowie::app
