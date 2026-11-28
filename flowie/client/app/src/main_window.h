#pragma once
#include "workbench.h"
#include <functional>
#include <map>
#include <windows.h>
// clang-format off
// WTL requires ATL first, and the process module before control wrappers.
#include <atlbase.h>
#include <atlapp.h>
extern CAppModule _Module;
#include <atlwin.h>
#include <atlctrls.h>
#include <atlsplit.h>
// clang-format on
#include "ui/collections.h"
#include "ui/panel.h"
#include "ui/request_actions.h"

namespace flowie::app {
  struct MessageSample;
  enum class TaskLayout { workspace, editor, output };
  class StudioSplitter final : public CSplitterWindowImpl<StudioSplitter> {
  public:
    DECLARE_WND_CLASS_EX(L"FlowieStudioSplitter", CS_DBLCLKS, COLOR_WINDOW)
    StudioSplitter(const UiTheme &theme, bool vertical)
        : CSplitterWindowImpl<StudioSplitter>(vertical), theme_(theme) {}

    void DrawSplitterBar(CDCHandle dc) {
      RECT bounds{};
      if (GetSplitterBarRect(&bounds))
        ::FillRect(dc, &bounds, color_brush(dc, theme_.ink, theme_.divider));
    }

  private:
    const UiTheme &theme_;
  };

  class MainWindow final : public CWindowImpl<MainWindow> {
  public:
    explicit MainWindow(ClientRole role = ClientRole::both, std::wstring identifier = {},
                        std::function<void(const RecentConnection &)> remember = {})
        : role_(role), identifier_(std::move(identifier)), remember_(std::move(remember)) {}
    DECLARE_WND_CLASS_EX(L"FlowieMqttWorkbench", CS_DBLCLKS, COLOR_WINDOW)
    BEGIN_MSG_MAP(MainWindow)
    MESSAGE_HANDLER(WM_CREATE, OnCreate)
    MESSAGE_HANDLER(WM_SIZE, OnSize)
    MESSAGE_HANDLER(kPanelLayout, OnPanelLayout)
    MESSAGE_HANDLER(WM_ERASEBKGND, OnErase)
    MESSAGE_HANDLER(WM_CTLCOLORSTATIC, OnControlColor)
    MESSAGE_HANDLER(WM_CTLCOLOREDIT, OnControlColor)
    MESSAGE_HANDLER(WM_CTLCOLORLISTBOX, OnControlColor)
    MESSAGE_HANDLER(WM_CTLCOLORBTN, OnControlColor)
    MESSAGE_HANDLER(WM_SYSCOLORCHANGE, OnTheme)
    MESSAGE_HANDLER(WM_SETTINGCHANGE, OnTheme)
    MESSAGE_HANDLER(WM_THEMECHANGED, OnTheme)
    MESSAGE_HANDLER(WM_GETMINMAXINFO, OnMinMax)
    MESSAGE_HANDLER(WM_TIMER, OnTimer)
    MESSAGE_HANDLER(WM_COMMAND, OnCommand)
    MESSAGE_HANDLER(WM_NOTIFY, OnNotify)
    MESSAGE_HANDLER(WM_CLOSE, OnClose)
    MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
    END_MSG_MAP()
    bool translate(MSG *message);
    void apply_connection(Connection &connection, bool connect_now);
    void apply_sample(const MessageSample &sample);
    SavedRequest capture_request() const;
    void restore_request(SavedRequest request,
                         const std::optional<CollectionVariables> &variables = {});
    bool can_publish() const { return !closing_ && role_ != ClientRole::subscriber; }
    RequestCommand request_command(RequestAction action) const;
    void invoke_request(RequestAction action);
    void set_task_layout(TaskLayout layout);
    TaskLayout task_layout() const { return task_layout_; }
    State state() const { return session_.state(); }
    std::wstring remote_endpoint() const;
    bool closing() const { return closing_; }
    enum Id {
      host = 100,
      port,
      transport,
      protocol,
      path,
      client_id,
      username,
      password,
      ca,
      cert,
      key,
      connect_button,
      disconnect_button,
      filter,
      subscribe_button,
      unsubscribe_button,
      sub_qos,
      topic,
      pub_qos,
      retain,
      mode,
      payload,
      publish_button,
      format_button,
      validate_button,
      ping_button,
      clear_button,
      records,
      details,
      status,
      counters,
      subscription_title,
      history_title,
      subscriptions,
      request_tabs,
      detail_title,
      saved_topic_picker,
      message_filter,
      editor_splitter = 701,
      results_splitter
    };
    bool action_enabled(Id id) const;

  private:
    enum class RequestPage { message, topics, authorization, settings };
    HWND make(Id id, const wchar_t *kind, const wchar_t *text, DWORD style = 0);
    void field(Id id, const wchar_t *label, const wchar_t *value, bool secret = false);
    void choice(Id id, const std::initializer_list<const wchar_t *> &choices);
    void place(int id, int x, int y, int width, int height);
    void layout();
    void layout_panels();
    void create_request_tabs();
    void select_request_page(RequestPage page);
    bool show_event(Kind kind, int filter_index) const;
    void create_panels();
    void refresh_theme();
    HWND control_parent(int id) const;
    void enable();
    void refresh();
    void show_detail();
    void report(const std::wstring &message);
    void checked(int result, const wchar_t *operation);
    Connection connection_fields() const;
    void select_saved_topic();
    void connect(const Connection &connection);
    void command(int id);
    std::wstring text(int id) const;
    std::string utf8(int id) const;
    std::string request_text(int id, size_t limit) const;
    int selection(int id) const;
    int scale(int value) const;
    LRESULT OnCreate(UINT, WPARAM, LPARAM, BOOL &);
    LRESULT OnSize(UINT, WPARAM, LPARAM, BOOL &);
    LRESULT OnPanelLayout(UINT, WPARAM, LPARAM, BOOL &);
    LRESULT OnErase(UINT, WPARAM, LPARAM, BOOL &);
    LRESULT OnControlColor(UINT, WPARAM, LPARAM, BOOL &);
    LRESULT OnTheme(UINT, WPARAM, LPARAM, BOOL &);
    LRESULT OnMinMax(UINT, WPARAM, LPARAM, BOOL &);
    LRESULT OnTimer(UINT, WPARAM, LPARAM, BOOL &);
    LRESULT OnCommand(UINT, WPARAM, LPARAM, BOOL &);
    LRESULT OnNotify(UINT, WPARAM, LPARAM, BOOL &);
    LRESULT OnClose(UINT, WPARAM, LPARAM, BOOL &);
    LRESULT OnDestroy(UINT, WPARAM, LPARAM, BOOL &);
    std::map<int, HWND> controls_;
    HFONT font_ = nullptr, heading_font_ = nullptr, mono_font_ = nullptr;
    int dpi_ = 96;
    bool panels_ready_ = false, laying_out_ = false;
    RequestPage request_page_ = RequestPage::message;
    std::vector<RequestPage> request_pages_;
    TaskLayout task_layout_ = TaskLayout::workspace;
    UiTheme theme_{};
    StudioSplitter editor_{theme_, false}, results_{theme_, false};
    UiPanel connection_bar_, request_, composer_, topics_, auth_, settings_, history_panel_,
        detail_panel_;
    bool closing_ = false;
    ClientRole role_;
    std::wstring identifier_;
    std::function<void(const RecentConnection &)> remember_;
    State shown_state_ = State::idle;
    Session session_;
    std::optional<CollectionVariables> collection_variables_;
    std::optional<RecentConnection> pending_connection_;
    History history_;
    std::vector<size_t> visible_records_;
    size_t received_ = 0, transmitted_ = 0;
    std::map<std::string, int> subscriptions_;
    std::vector<SavedTopic> saved_topics_;
    std::string pending_filter_;
    int pending_qos_ = 0;
  };
} // namespace flowie::app
