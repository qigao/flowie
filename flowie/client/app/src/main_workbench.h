#pragma once
#include <filesystem>
#include <memory>
#include <variant>
#include <vector>

#include "main_protocol_window.h"

namespace flowie::app {
  constexpr size_t kMaxClients = 8;
  // Owns stable child windows; each child retains its own Session and native controls.
  class WorkbenchWindow final : public CWindowImpl<WorkbenchWindow> {
  public:
    DECLARE_WND_CLASS_EX(L"FlowieMqttWorkspace", CS_DBLCLKS, COLOR_WINDOW)
    BEGIN_MSG_MAP(WorkbenchWindow)
    MESSAGE_HANDLER(WM_CREATE, OnCreate)
    MESSAGE_HANDLER(WM_SIZE, OnSize)
    MESSAGE_HANDLER(WM_GETMINMAXINFO, OnMinMax)
    MESSAGE_HANDLER(WM_COMMAND, OnCommand)
    MESSAGE_HANDLER(WM_INITMENUPOPUP, OnMenuPopup)
    MESSAGE_HANDLER(WM_NOTIFY, OnNotify)
    MESSAGE_HANDLER(WM_DRAWITEM, OnDrawItem)
    MESSAGE_HANDLER(WM_ERASEBKGND, OnErase)
    MESSAGE_HANDLER(WM_CTLCOLORSTATIC, OnControlColor)
    MESSAGE_HANDLER(WM_SYSCOLORCHANGE, OnTheme)
    MESSAGE_HANDLER(WM_SETTINGCHANGE, OnTheme)
    MESSAGE_HANDLER(WM_THEMECHANGED, OnTheme)
    MESSAGE_HANDLER(WM_TIMER, OnTimer)
    MESSAGE_HANDLER(WM_CLOSE, OnClose)
    MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
    END_MSG_MAP()
    enum Id {
      add_publisher = 500,
      add_subscriber,
      add_client,
      close_client,
      client_tabs,
      workspace_status,
      tab_close,
      exit_workbench,
      import_connection_file,
      import_sample_file,
      disconnect_active,
      publish_active,
      validate_active,
      format_active,
      view_workspace,
      view_publish,
      view_monitor,
      save_request,
      open_saved_request,
      toggle_collections,
      collections_tree,
      sidebar_tabs,
      collections_empty,
      add_http,
      add_rpc,
      add_tcp,
      add_serial,
      choose_new_request,
      browse_collections,
      manage_collections,
      collection_files,
      reload_collection_file,
      unload_collection_file,
      sidebar_history,
      collection_files_title,
      connect_active,
      refresh_ports_active,
      primary_request,
      request_actions,
      run_collection,
      remote_status,
      command_toolbar,
      first_recent = 600,
      first_recent_collection = 700
    };
    explicit WorkbenchWindow(std::wstring history_key = kRecentConnectionKey,
                             std::filesystem::path library = {})
        : history_(std::move(history_key)), collections_file_(std::move(library)) {}
    bool translate(MSG *message);
    void import_connection(const std::filesystem::path &file);
    void import_sample(const std::filesystem::path &file);
    void load_collections(const std::filesystem::path &file);

  private:
    struct Client {
      std::wstring label, shown_label;
      std::string collection, saved_name, notes;
      std::filesystem::path source;
      std::variant<MainWindow, ProtocolWindow, UiPanel> window;
      HWND close_button = nullptr;
      bool close_hot = false;
      Client(WorkbenchWindow &owner, ClientRole role, std::wstring identifier, std::wstring name)
          : label(std::move(name)),
            window(std::in_place_type<MainWindow>, role, std::move(identifier),
                   [&owner](const RecentConnection &entry) { owner.remember_connection(entry); }) {}
      Client(ToolKind kind, std::wstring name, bool runner)
          : label(std::move(name)), window(std::in_place_type<ProtocolWindow>, kind, runner) {}
      Client() : label(L"Manage Collections"), window(std::in_place_type<UiPanel>) {}
      bool manager() const { return std::holds_alternative<UiPanel>(window); }
      HWND hwnd() const {
        return std::visit([](const auto &pane) { return pane.m_hWnd; }, window);
      }
      State state() const {
        if (const auto *mqtt = std::get_if<MainWindow>(&window)) return mqtt->state();
        if (const auto *tool = std::get_if<ProtocolWindow>(&window)) return tool->state();
        return State::idle;
      }
      bool closing() const {
        if (const auto *mqtt = std::get_if<MainWindow>(&window)) return mqtt->closing();
        if (const auto *tool = std::get_if<ProtocolWindow>(&window)) return tool->closing();
        return false;
      }
      ~Client() {
        if (::IsWindow(close_button)) ::DestroyWindow(close_button);
      }
    };
    MainWindow &add(ClientRole role, std::wstring name = {});
    ProtocolWindow &add_tool(ToolKind kind, std::wstring name = {}, bool runner = false);
    Client &add_tab(std::unique_ptr<Client> client);
    void open_connection(ClientRole role, const Connection *initial = nullptr);
    void show_connection(ClientRole role, std::vector<RecentConnection> recent, int index,
                         const Connection *initial = nullptr);
    Client *active_tab() const;
    RequestCommand request_command(RequestAction action) const;
    RequestAction primary_action() const;
    static std::optional<RequestAction> request_action(int id);
    void invoke_request(RequestAction action);
    std::optional<TaskLayout> task_layout() const;
    void set_task_layout(TaskLayout layout);
    void create_toolbar();
    void layout_toolbar(int width);
    void show_toolbar_menu(Id command, HMENU menu);
    void update_request_toolbar();
    void choose_import(bool connection);
    void create_menu();
    void create_collections();
    void choose_collections_file();
    void load_collection_file(const std::filesystem::path &file, bool remember = false);
    void open_collections_manager();
    void refresh_collections_manager(int selected = -1);
    void change_collection_file(bool unload);
    HWND manager_window() const;
    void layout_collections_manager(HWND window, int width, int height);
    void refresh_sidebar_history();
    void open_sidebar_history();
    void select_sidebar(int page);
    int collections_width() const;
    void layout_collections(int height);
    void refresh_collections_theme();
    void rebuild_collections(const std::filesystem::path &file = {},
                             const SavedRequest *selection = nullptr);
    void update_collection_actions();
    void run_selected_collection();
    bool can_run_collection() const;
    struct RequestLocation {
      size_t file, request;
    };
    std::optional<RequestLocation> selected_request() const;
    std::optional<RequestLocation> request_at(HTREEITEM node) const;
    std::optional<size_t> selected_collection_file() const;
    void save_active_request();
    bool can_save_request() const;
    void open_selected_request();
    LRESULT on_collection_notice(const NMHDR &notification);
    void remember_connection(const RecentConnection &entry);
    size_t request_count() const;
    bool can_add() const { return !opening_ && !closing_ && request_count() < kMaxClients; }
    void activate(size_t index);
    void update();
    void update_status_bar();
    void layout();
    void close_all() noexcept;
    void close_client_at(size_t index);
    void position_close_buttons();
    bool can_close(const Client &client) const;
    std::wstring padded_tab_label(const std::wstring &text) const;
    static LRESULT CALLBACK TabProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    int scale(int value) const { return MulDiv(value, dpi_, 96); }
    LRESULT OnCreate(UINT, WPARAM, LPARAM, BOOL &);
    LRESULT OnSize(UINT, WPARAM, LPARAM, BOOL &);
    LRESULT OnMinMax(UINT, WPARAM, LPARAM, BOOL &);
    LRESULT OnCommand(UINT, WPARAM, LPARAM, BOOL &);
    LRESULT OnMenuPopup(UINT, WPARAM, LPARAM, BOOL &);
    LRESULT OnNotify(UINT, WPARAM, LPARAM, BOOL &);
    LRESULT OnDrawItem(UINT, WPARAM, LPARAM, BOOL &);
    LRESULT OnErase(UINT, WPARAM, LPARAM, BOOL &);
    LRESULT OnControlColor(UINT, WPARAM, LPARAM, BOOL &);
    LRESULT OnTheme(UINT, WPARAM, LPARAM, BOOL &);
    LRESULT OnTimer(UINT, WPARAM, LPARAM, BOOL &);
    LRESULT OnClose(UINT, WPARAM, LPARAM, BOOL &);
    LRESULT OnDestroy(UINT, WPARAM, LPARAM, BOOL &);
    std::vector<std::unique_ptr<Client>> clients_;
    RecentConnections history_;
    std::filesystem::path collections_file_, collections_settings_;
    struct LoadedCollection {
      std::filesystem::path path;
      std::vector<SavedRequest> requests;
    };
    static constexpr size_t kLoadedFilesLimit = 16;
    std::vector<LoadedCollection> loaded_collections_;
    std::vector<RequestLocation> sidebar_requests_;
    std::vector<RecentConnection> sidebar_recent_;
    std::vector<RecentConnection> menu_recent_;
    std::vector<std::filesystem::path> menu_recent_collections_;
    HMENU file_menu_ = nullptr, session_menu_ = nullptr, recent_menu_ = nullptr;
    HMENU recent_collections_menu_ = nullptr;
    HMENU new_tool_menu_ = nullptr;
    HMENU mqtt_menu_ = nullptr;
    HMENU request_menu_ = nullptr, view_menu_ = nullptr;
    size_t active_ = 0;
    uint64_t next_id_ = 1;
    HWND tabs_ = nullptr, summary_ = nullptr;
    CToolBarCtrl toolbar_;
    HWND status_bar_ = nullptr;
    std::wstring shown_remote_, shown_connection_;
    HWND tooltip_ = nullptr;
    RequestAction shown_primary_ = RequestAction::send;
    HFONT font_ = nullptr;
    int dpi_ = 96;
    bool closing_ = false, faulted_ = false, opening_ = false;
    bool has_error_ = false;
    bool collections_visible_ = true;
    UiTheme theme_{};
    UiPanel collections_panel_;
    CTreeViewCtrl collections_tree_;
    HWND sidebar_tabs_ = nullptr, collections_empty_ = nullptr, sidebar_history_ = nullptr;
    HWND run_collection_ = nullptr;
    int sidebar_page_ = 0;
    std::wstring tab_padding_;
  };
} // namespace flowie::app
