#include "main_workbench.h"
#include "ui/save_request_dialog.h"
#include <algorithm>
#include <map>
#include <stdexcept>

namespace flowie::app {
  namespace {
    constexpr int kSidebarWidth = 242;
    constexpr int kCollectionsPage = 0, kHistoryPage = 1, kManagerPage = 2;

    HWND control(HWND parent, HFONT font, int id, const wchar_t *type, const wchar_t *label,
                 DWORD style) {
      const auto window = ::CreateWindowExW(
          0, type, label, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, parent,
          reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), _Module.GetModuleInstance(), nullptr);
      if (!window) throw std::runtime_error("Could not create a Collections control.");
      ::SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
      return window;
    }

    void list_item(HWND list, const std::wstring &text) {
      if (::SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str())) < 0)
        throw std::runtime_error("Could not display a workspace list.");
    }

    int list_selection(HWND list) {
      return static_cast<int>(::SendMessageW(list, LB_GETCURSEL, 0, 0));
    }
  } // namespace

  int WorkbenchWindow::collections_width() const {
    return collections_visible_ ? scale(kSidebarWidth + 4) : 0;
  }

  void WorkbenchWindow::create_collections() {
    collections_panel_.owner = m_hWnd;
    collections_panel_.paper = theme_.surface;
    RECT bounds{0, 0, scale(kSidebarWidth), 0};
    if (!collections_panel_.Create(m_hWnd, bounds, nullptr,
                                   WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                                   WS_EX_CONTROLPARENT))
      throw std::runtime_error("Could not create the workspace sidebar.");

    sidebar_tabs_ = control(collections_panel_, font_, sidebar_tabs, WC_TABCONTROLW,
                            L"Workspace navigation", WS_TABSTOP | TCS_SINGLELINE);
    style_tabs(sidebar_tabs_, theme_);
    TabCtrl_SetPadding(sidebar_tabs_, scale(12), scale(4));
    for (auto *label : {L"Collections", L"History", L"Manage"}) {
      TCITEMW item{};
      item.mask = TCIF_TEXT;
      item.pszText = const_cast<wchar_t *>(label);
      if (TabCtrl_InsertItem(sidebar_tabs_, TabCtrl_GetItemCount(sidebar_tabs_), &item) < 0)
        throw std::runtime_error("Could not create sidebar navigation.");
    }
    collections_empty_ = control(collections_panel_, font_, collections_empty, L"STATIC",
                                 L"No collections loaded.", SS_LEFT);
    collections_tree_.Attach(control(collections_panel_, font_, collections_tree, WC_TREEVIEWW,
                                     L"Loaded collections",
                                     WS_TABSTOP | WS_BORDER | TVS_HASBUTTONS | TVS_HASLINES |
                                         TVS_LINESATROOT | TVS_SHOWSELALWAYS | TVS_INFOTIP));
    run_collection_ = control(collections_panel_, font_, run_collection, L"BUTTON",
                              L"&Run Collection", WS_TABSTOP);
    sidebar_history_ =
        control(collections_panel_, font_, sidebar_history, L"LISTBOX", L"Recent MQTT connections",
                WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT);
    refresh_collections_theme();
    if (collections_file_.empty()) collections_file_ = default_collections_file();
    collections_settings_ = collections_file_;
    collections_settings_.replace_filename(L"collections.ini");
    try {
      if (std::filesystem::exists(collections_file_))
        load_collection_file(collections_file_);
      else rebuild_collections();
    } catch (const std::exception &error) {
      has_error_ = true;
      ::SetWindowTextA(summary_, error.what());
      rebuild_collections();
    }
  }

  void WorkbenchWindow::layout_collections(int height) {
    if (!collections_panel_.IsWindow()) return;
    if (collections_visible_) {
      collections_panel_.MoveWindow(0, scale(38), scale(kSidebarWidth), height, TRUE);
      ::MoveWindow(sidebar_tabs_, scale(4), 0, scale(kSidebarWidth - 8), scale(31), TRUE);
      ::MoveWindow(collections_empty_, scale(12), scale(43), scale(kSidebarWidth - 24), scale(35),
                   TRUE);
      const int content_height = std::max(0, height - scale(43));
      collections_tree_.MoveWindow(scale(5), scale(35), scale(kSidebarWidth - 10),
                                   std::max(0, content_height - scale(34)), TRUE);
      ::MoveWindow(run_collection_, scale(5), std::max(scale(35), height - scale(34)),
                   scale(kSidebarWidth - 10), scale(28), TRUE);
      ::MoveWindow(sidebar_history_, scale(5), scale(35), scale(kSidebarWidth - 10), content_height,
                   TRUE);
    }
    const auto focus = ::GetFocus();
    collections_panel_.ShowWindow(collections_visible_ ? SW_SHOWNA : SW_HIDE);
    if (!collections_visible_ && ::IsChild(collections_panel_, focus))
      toolbar_.SetFocus();
  }

  void WorkbenchWindow::refresh_collections_theme() {
    collections_panel_.paper = theme_.surface;
    collections_tree_.SetBkColor(theme_.paper);
    collections_tree_.SetTextColor(theme_.ink);
    collections_tree_.SetLineColor(theme_.divider);
    for (const auto &client : clients_)
      if (auto *panel = std::get_if<UiPanel>(&client->window)) panel->paper = theme_.paper;
  }

  std::optional<size_t> WorkbenchWindow::selected_collection_file() const {
    auto node = collections_tree_.GetSelectedItem();
    if (!node) return std::nullopt;
    while (auto parent = collections_tree_.GetParentItem(node))
      node = parent;
    const size_t slot = collections_tree_.GetItemData(node);
    return slot && slot <= loaded_collections_.size() ? std::optional<size_t>(slot - 1)
                                                      : std::nullopt;
  }

  std::optional<WorkbenchWindow::RequestLocation> WorkbenchWindow::selected_request() const {
    return request_at(collections_tree_.GetSelectedItem());
  }

  std::optional<WorkbenchWindow::RequestLocation>
  WorkbenchWindow::request_at(HTREEITEM node) const {
    if (!node) return std::nullopt;
    const auto slot = static_cast<INT_PTR>(collections_tree_.GetItemData(node));
    if (slot >= 0 || static_cast<size_t>(-slot) > sidebar_requests_.size()) return std::nullopt;
    return sidebar_requests_[static_cast<size_t>(-slot) - 1];
  }

  void WorkbenchWindow::rebuild_collections(const std::filesystem::path &path,
                                            const SavedRequest *selection) {
    collections_tree_.DeleteAllItems();
    sidebar_requests_.clear();
    const auto insert = [&](const std::wstring &name, HTREEITEM parent, LPARAM slot) {
      TVINSERTSTRUCTW item{};
      item.hParent = parent;
      item.hInsertAfter = TVI_LAST;
      item.item.mask = TVIF_TEXT | TVIF_PARAM;
      item.item.pszText = const_cast<wchar_t *>(name.c_str());
      item.item.lParam = slot;
      const auto node = collections_tree_.InsertItem(&item);
      if (!node) throw std::runtime_error("Could not display a saved request.");
      return node;
    };
    HTREEITEM selected = nullptr;
    for (size_t file = 0; file < loaded_collections_.size(); ++file) {
      const auto &collection = loaded_collections_[file];
      const auto root = insert(collection.path.filename().wstring(), TVI_ROOT, file + 1);
      const bool selected_file = !path.empty() && same_collection_file(collection.path, path);
      if (selected_file) selected = root;
      std::map<std::string, HTREEITEM> groups;
      for (size_t index = 0; index < collection.document.requests.size(); ++index) {
        const auto &entry = collection.document.requests[index];
        const auto [group, added] = groups.try_emplace(entry.collection, nullptr);
        if (added) group->second = insert(utf8_to_wide(entry.collection), root, 0);
        sidebar_requests_.push_back({file, index});
        const auto node = insert(utf8_to_wide(entry.name), group->second,
                                 -static_cast<LPARAM>(sidebar_requests_.size()));
        if (selected_file && selection && entry.collection == selection->collection &&
            entry.name == selection->name)
          selected = node;
      }
      for (const auto &group : groups) collections_tree_.Expand(group.second);
      collections_tree_.Expand(root);
    }
    if (selected) collections_tree_.SelectItem(selected);
    select_sidebar(sidebar_page_);
  }

  void WorkbenchWindow::select_sidebar(int page) {
    if (page == kManagerPage) {
      TabCtrl_SetCurSel(sidebar_tabs_, sidebar_page_);
      open_collections_manager();
      return;
    }
    if (page == kHistoryPage && page != sidebar_page_) refresh_sidebar_history();
    sidebar_page_ = page;
    TabCtrl_SetCurSel(sidebar_tabs_, page);
    const bool collections = page == kCollectionsPage;
    collections_tree_.ShowWindow(collections && !loaded_collections_.empty() ? SW_SHOWNA : SW_HIDE);
    ::ShowWindow(run_collection_, collections && !loaded_collections_.empty() ? SW_SHOWNA
                                                                             : SW_HIDE);
    ::ShowWindow(sidebar_history_, !collections && !sidebar_recent_.empty() ? SW_SHOWNA : SW_HIDE);
    const bool empty = collections ? loaded_collections_.empty() : sidebar_recent_.empty();
    ::SetWindowTextW(collections_empty_,
                     collections ? L"No collections loaded." : L"No recent MQTT connections.");
    ::ShowWindow(collections_empty_, empty ? SW_SHOWNA : SW_HIDE);
  }

  void WorkbenchWindow::refresh_sidebar_history() {
    auto recent = history_.load();
    ::SendMessageW(sidebar_history_, LB_RESETCONTENT, 0, 0);
    for (const auto &entry : recent)
      list_item(sidebar_history_, connection_label(entry));
    sidebar_recent_ = std::move(recent);
  }

  void WorkbenchWindow::load_collections(const std::filesystem::path &file) {
    load_collection_file(file, true);
  }

  void WorkbenchWindow::load_collection_file(const std::filesystem::path &file,
                                              bool remember) {
    auto path = std::filesystem::canonical(file);
    auto found = std::find_if(loaded_collections_.begin(), loaded_collections_.end(),
                              [&](const auto &entry) { return same_collection_file(entry.path, path); });
    const auto index = static_cast<size_t>(found - loaded_collections_.begin());
    if (found == loaded_collections_.end() && index == kLoadedFilesLimit)
      throw std::runtime_error("The workspace can load up to 16 collection files.");
    auto document = CollectionStore(path).load_document(true);
    if (found == loaded_collections_.end())
      loaded_collections_.push_back({std::move(path), std::move(document)});
    else found->document = std::move(document);

    rebuild_collections(loaded_collections_[index].path);
    select_sidebar(kCollectionsPage);
    refresh_collections_manager(static_cast<int>(index));
    if (remember) remember_collection_file(collections_settings_, loaded_collections_[index].path);
  }

  void WorkbenchWindow::choose_collections_file() {
    if (opening_ || closing_) return;
    const auto chosen = [&] {
      struct Pick {
        bool &opening;
        ~Pick() { opening = false; }
      } guard{opening_};
      opening_ = true;
      return choose_collection_file(m_hWnd, read_collections_directory(collections_settings_));
    }();
    if (chosen && !faulted_ && !closing_) load_collections(*chosen);
  }

  HWND WorkbenchWindow::manager_window() const {
    for (const auto &client : clients_)
      if (client->manager() && ::IsWindow(client->hwnd())) return client->hwnd();
    return nullptr;
  }

  void WorkbenchWindow::open_collections_manager() {
    if (opening_ || closing_) return;
    for (size_t i = 0; i < clients_.size(); ++i) {
      if (clients_[i]->manager()) {
        if (::IsWindow(clients_[i]->hwnd())) {
          activate(i);
          ::SetFocus(::GetDlgItem(clients_[i]->hwnd(), loaded_collections_.empty()
                                                           ? browse_collections
                                                           : collection_files));
        }
        return;
      }
    }
    auto tab = std::make_unique<Client>();
    auto &panel = std::get<UiPanel>(tab->window);
    panel.owner = m_hWnd;
    panel.paper = theme_.paper;
    const auto window = add_tab(std::move(tab)).hwnd();
    try {
      control(window, font_, collection_files_title, L"STATIC", L"Loaded collection files", 0);
      control(window, font_, browse_collections, L"BUTTON", L"&Open file...", WS_TABSTOP);
      control(window, font_, reload_collection_file, L"BUTTON", L"&Reload", WS_TABSTOP);
      control(window, font_, unload_collection_file, L"BUTTON", L"&Unload", WS_TABSTOP);
      auto list = control(window, font_, collection_files, L"LISTBOX", L"Loaded collection files",
                          WS_TABSTOP | WS_BORDER | WS_VSCROLL | WS_HSCROLL | LBS_NOTIFY |
                              LBS_NOINTEGRALHEIGHT);
      const auto selected = selected_collection_file();
      refresh_collections_manager(selected ? static_cast<int>(*selected) : -1);
      layout();
      ::SetFocus(loaded_collections_.empty() ? ::GetDlgItem(window, browse_collections) : list);
    } catch (...) {
      ::SendMessageW(window, WM_CLOSE, 0, 0);
      throw;
    }
  }

  void WorkbenchWindow::layout_collections_manager(HWND window, int width, int height) {
    const auto place = [&](Id id, int x, int y, int w, int h) {
      if (auto child = ::GetDlgItem(window, id)) ::MoveWindow(child, x, y, w, h, TRUE);
    };
    const int content = std::max(0, width - scale(32));
    place(collection_files_title, scale(16), scale(14), content, scale(22));
    place(browse_collections, scale(16), scale(42), scale(115), scale(28));
    place(reload_collection_file, scale(139), scale(42), scale(85), scale(28));
    place(unload_collection_file, scale(232), scale(42), scale(85), scale(28));
    place(collection_files, scale(16), scale(82), content, std::max(0, height - scale(98)));
  }

  void WorkbenchWindow::refresh_collections_manager(int selected) {
    const auto window = manager_window();
    if (!window) return;
    const auto list = ::GetDlgItem(window, collection_files);
    if (!list) return;
    if (selected < 0) selected = list_selection(list);
    ::SendMessageW(list, LB_RESETCONTENT, 0, 0);
    CClientDC dc(list);
    const auto font = dc.SelectFont(font_);
    LONG width = 0;
    for (const auto &entry : loaded_collections_) {
      const auto count = entry.document.requests.size();
      auto text = entry.path.wstring() + L"   (" + std::to_wstring(count) +
                  (count == 1 ? L" request)" : L" requests)");
      list_item(list, text);
      SIZE size{};
      dc.GetTextExtent(text.c_str(), static_cast<int>(text.size()), &size);
      width = std::max(width, size.cx);
    }
    dc.SelectFont(font);
    ::SendMessageW(list, LB_SETHORIZONTALEXTENT, width + scale(12), 0);
    if (selected < 0) selected = 0;
    ::SendMessageW(list, LB_SETCURSEL, selected, 0);
    update_collection_actions();
  }

  void WorkbenchWindow::change_collection_file(bool unload) {
    if (opening_ || closing_) return;
    const auto window = manager_window();
    if (!window) return;
    const int index = list_selection(::GetDlgItem(window, collection_files));
    if (index < 0 || static_cast<size_t>(index) >= loaded_collections_.size()) return;
    if (unload) {
      loaded_collections_.erase(loaded_collections_.begin() + index);
      rebuild_collections();
      refresh_collections_manager(
          std::min(index, static_cast<int>(loaded_collections_.size()) - 1));
    } else {
      load_collection_file(loaded_collections_[index].path);
    }
  }

  void WorkbenchWindow::update_collection_actions() {
    ::EnableWindow(run_collection_, can_run_collection());
    if (const auto window = manager_window()) {
      ::EnableWindow(::GetDlgItem(window, browse_collections), !opening_ && !closing_);
      const bool selected =
          !opening_ && !closing_ && list_selection(::GetDlgItem(window, collection_files)) >= 0;
      ::EnableWindow(::GetDlgItem(window, reload_collection_file), selected);
      ::EnableWindow(::GetDlgItem(window, unload_collection_file), selected);
    }
  }

  bool WorkbenchWindow::can_save_request() const {
    const auto *tab = active_tab();
    if (!tab || opening_) return false;
    const auto *tool = std::get_if<ProtocolWindow>(&tab->window);
    return !tool || !tool->runner();
  }

  bool WorkbenchWindow::can_run_collection() const {
    return !faulted_ && can_add() && sidebar_page_ == kCollectionsPage &&
           selected_collection_file().has_value();
  }

  void WorkbenchWindow::run_selected_collection() {
    if (!can_run_collection()) return;
    const auto &file = loaded_collections_[*selected_collection_file()];
    const auto node = collections_tree_.GetSelectedItem();
    auto scope = request_at(node);
    // A group node refers to the collection of its first member.
    if (!scope && collections_tree_.GetParentItem(node))
      scope = request_at(collections_tree_.GetChildItem(node));
    const auto &document = file.document;
    const std::string *name = scope ? &document.requests[scope->request].collection : nullptr;
    std::vector<CollectionStep> steps;
    for (const auto &request : document.requests) {
      if (name && request.collection != *name) continue;
      if (!request.tool || !request_tool(request.tool->kind))
        throw std::runtime_error("Run Collection supports HTTP and JSON-RPC. Check " +
                                 request.name + ".");
      try {
        auto draft = expand_collection_tool(*request.tool, document.bindings());
        validate_http_request(draft);
        steps.push_back({request.name, std::move(draft)});
      } catch (const std::exception &error) {
        throw std::runtime_error(request.name + ": " + error.what());
      }
    }
    if (steps.empty()) throw std::runtime_error("The selected collection is empty.");
    const auto title = name ? utf8_to_wide(*name) : file.path.filename().wstring();
    auto &runner = add_tool(ToolKind::http, L"Run " + title, true);
    try {
      runner.start_collection(std::move(steps));
    } catch (...) {
      ::SendMessageW(runner, WM_CLOSE, 0, 0);
      throw;
    }
  }

  void WorkbenchWindow::save_active_request() {
    if (!can_save_request()) return;
    auto *tab = active_tab();
    const auto origin = tab->hwnd();
    auto request = std::get_if<ProtocolWindow>(&tab->window)
                       ? std::get<ProtocolWindow>(tab->window).capture_request()
                       : std::get<MainWindow>(tab->window).capture_request();
    request.collection = tab->collection;
    request.name = tab->saved_name;
    request.notes = tab->notes;
    auto file = std::find_if(loaded_collections_.begin(), loaded_collections_.end(),
                             [&](const auto &entry) { return same_collection_file(entry.path, tab->source); });
    if (file == loaded_collections_.end()) {
      if (const auto selected = selected_collection_file())
        file = loaded_collections_.begin() + *selected;
      else if (!loaded_collections_.empty()) file = std::prev(loaded_collections_.end());
    }
    const bool loaded = file != loaded_collections_.end();
    const auto path = loaded ? file->path : collections_file_;
    const std::vector<SavedRequest> empty;
    struct Reset {
      bool &flag;
      ~Reset() { flag = false; }
    } reset{opening_};
    opening_ = true;
    if (!name_saved_request(m_hWnd, request, loaded ? file->document.requests : empty, path)) return;
    if (faulted_ || closing_) return;
    const CollectionStore store(path);
    auto committed = store.save_document(request, false, loaded);
    if (!committed) {
      const auto name = utf8_to_wide(request.collection + " / " + request.name);
      if (::MessageBoxW(m_hWnd,
                        (L"Replace " + name + L" in " + path.filename().wstring() + L"?").c_str(),
                        L"Save Request", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;
      committed = store.save_document(request, true, loaded);
    }
    const auto resolved = std::filesystem::canonical(path);
    if (file == loaded_collections_.end())
      loaded_collections_.push_back({resolved, std::move(*committed)});
    else file->document = std::move(*committed);
    for (auto &client : clients_) {
      if (client->hwnd() != origin) continue;
      client->collection = request.collection;
      client->saved_name = request.name;
      client->notes = request.notes;
      client->source = resolved;
      client->label = utf8_to_wide(request.name);
      break;
    }
    rebuild_collections(resolved, &request);
    select_sidebar(kCollectionsPage);
    refresh_collections_manager();
  }

  void WorkbenchWindow::open_selected_request() {
    const auto selected = selected_request();
    if (!can_add() || !selected) return;
    const auto &file = loaded_collections_[selected->file];
    auto request = file.document.requests[selected->request];
    const auto name = utf8_to_wide(request.name);
    const auto window = request.tool ? add_tool(request.tool->kind, name).m_hWnd
                                     : add(request.profile.role, name).m_hWnd;
    try {
      auto &client = *clients_[active_];
      client.collection = request.collection;
      client.saved_name = request.name;
      client.notes = request.notes;
      client.source = file.path;
      if (request.tool)
        std::get<ProtocolWindow>(client.window).restore_request(*request.tool, file.document.variables);
      else
        std::get<MainWindow>(client.window).restore_request(std::move(request), file.document.variables);
    } catch (...) {
      ::SendMessageW(window, WM_CLOSE, 0, 0);
      throw;
    }
  }

  void WorkbenchWindow::open_sidebar_history() {
    const int index = list_selection(sidebar_history_);
    if (!can_add() || index < 0 || static_cast<size_t>(index) >= sidebar_recent_.size()) return;
    show_connection(sidebar_recent_[index].role, sidebar_recent_, index);
  }

  LRESULT WorkbenchWindow::on_collection_notice(const NMHDR &notification) {
    try {
      if (notification.hwndFrom == sidebar_tabs_ && notification.code == TCN_SELCHANGE) {
        select_sidebar(TabCtrl_GetCurSel(sidebar_tabs_));
      } else if (notification.hwndFrom == collections_tree_) {
        if (notification.code == TVN_SELCHANGEDW) update_collection_actions();
        if (notification.code == TVN_GETINFOTIPW) {
          auto &tip = reinterpret_cast<NMTVGETINFOTIPW &>(const_cast<NMHDR &>(notification));
          if (tip.lParam > 0 && static_cast<size_t>(tip.lParam) <= loaded_collections_.size())
            ::lstrcpynW(tip.pszText, loaded_collections_[tip.lParam - 1].path.c_str(),
                        tip.cchTextMax);
          else if (tip.lParam < 0 && static_cast<size_t>(-tip.lParam) <= sidebar_requests_.size()) {
            const auto location = sidebar_requests_[static_cast<size_t>(-tip.lParam) - 1];
            const auto &notes =
                loaded_collections_[location.file].document.requests[location.request].notes;
            if (!notes.empty())
              ::lstrcpynW(tip.pszText, utf8_to_wide(notes).c_str(), tip.cchTextMax);
          }
          return 0;
        }
        bool open = notification.code == NM_RETURN;
        if (notification.code == NM_DBLCLK) {
          TVHITTESTINFO hit{};
          if (::GetCursorPos(&hit.pt)) {
            collections_tree_.ScreenToClient(&hit.pt);
            open = collections_tree_.HitTest(&hit) == collections_tree_.GetSelectedItem() &&
                   (hit.flags & TVHT_ONITEM);
          }
        }
        if (open && selected_request()) {
          SendMessage(WM_COMMAND, MAKEWPARAM(open_saved_request, BN_CLICKED));
          return TRUE;
        }
      }
    } catch (const std::exception &error) {
      has_error_ = true;
      ::SetWindowTextA(summary_, error.what());
    }
    return 0;
  }
} // namespace flowie::app
