#pragma once
#include "main_window.h"

namespace flowie::app {
  struct CollectionStep {
    std::string name;
    ToolDraft request;
  };

  class ProtocolWindow final : public CWindowImpl<ProtocolWindow> {
  public:
    explicit ProtocolWindow(ToolKind kind, bool runner = false) : kind_(kind), runner_(runner) {}
    DECLARE_WND_CLASS_EX(L"FlowieProtocolRequest", CS_DBLCLKS, COLOR_WINDOW)
    BEGIN_MSG_MAP(ProtocolWindow)
    MESSAGE_HANDLER(WM_CREATE, on_create)
    MESSAGE_HANDLER(WM_SIZE, on_layout)
    MESSAGE_HANDLER(kPanelLayout, on_layout)
    MESSAGE_HANDLER(WM_COMMAND, on_command)
    MESSAGE_HANDLER(WM_NOTIFY, on_notify)
    MESSAGE_HANDLER(WM_TIMER, on_timer)
    MESSAGE_HANDLER(WM_CLOSE, on_close)
    MESSAGE_HANDLER(WM_DESTROY, on_destroy)
    MESSAGE_HANDLER(WM_ERASEBKGND, on_erase)
    MESSAGE_HANDLER(WM_CTLCOLORSTATIC, on_color)
    MESSAGE_HANDLER(WM_CTLCOLOREDIT, on_color)
    MESSAGE_HANDLER(WM_CTLCOLORLISTBOX, on_color)
    MESSAGE_HANDLER(WM_CTLCOLORBTN, on_color)
    MESSAGE_HANDLER(WM_SYSCOLORCHANGE, on_theme)
    MESSAGE_HANDLER(WM_SETTINGCHANGE, on_theme)
    MESSAGE_HANDLER(WM_THEMECHANGED, on_theme)
    END_MSG_MAP()
    enum Id {
      target = 900,
      method,
      headers,
      body,
      hex,
      run,
      send,
      ports,
      baud,
      bits,
      parity,
      stop_bits,
      flow,
      rows,
      detail,
      report,
      message_title,
      target_title,
      method_title,
      header_title,
      body_title,
      json_body,
      use_cookies,
      clear_cookies,
      request_tabs,
      query_text,
      query_title,
      response_tabs,
      response_summary,
      response_body,
      response_headers
    };
    State state() const { return session_.state(); }
    ToolKind kind() const { return kind_; }
    bool closing() const { return closing_; }
    bool translate(MSG *message);
    RequestCommand request_command(RequestAction action) const;
    void invoke_request(RequestAction action);
    void apply_sample(std::string_view payload);
    void set_task_layout(TaskLayout layout);
    TaskLayout task_layout() const { return task_layout_; }
    SavedRequest capture_request() const;
    void restore_request(const ToolDraft &draft,
                         const std::optional<CollectionVariables> &variables = {});
    bool runner() const { return runner_; }
    void start_collection(std::vector<CollectionStep> steps);
    std::wstring remote_endpoint() const;
    const std::wstring &connection_status() const { return connection_status_; }

  private:
    enum class RequestPage { query, headers, body, session };
    enum class ResponsePage { body, headers, activity };

    HWND make(Id id, HWND parent, const wchar_t *kind, const wchar_t *value, DWORD style = 0,
              bool focusable = true);
    HWND get(Id id) const { return controls_[id - target]; }
    void choose(Id id, std::initializer_list<const wchar_t *> items);
    void create_tabs(Id id, HWND parent, const wchar_t *name,
                     std::initializer_list<const wchar_t *> labels);
    void select_request_page(RequestPage page);
    void select_response_page(ResponsePage page);
    void sync_query(Id edited);
    void layout_web_request(SIZE size);
    void layout_web_response(SIZE size);
    void refresh_ports();
    ToolDraft draft() const;
    void update();
    void show_detail();
    void start_collection_step();
    void advance_collection();
    void place(Id id, int x, int y, int width, int height);
    int scale(int value) const { return MulDiv(value, dpi_, 96); }
    LRESULT on_create(UINT, WPARAM, LPARAM, BOOL &);
    LRESULT on_layout(UINT, WPARAM, LPARAM, BOOL &);
    LRESULT on_command(UINT, WPARAM, LPARAM, BOOL &);
    LRESULT on_notify(UINT, WPARAM, LPARAM, BOOL &);
    LRESULT on_timer(UINT, WPARAM, LPARAM, BOOL &);
    LRESULT on_close(UINT, WPARAM, LPARAM, BOOL &);
    LRESULT on_destroy(UINT, WPARAM, LPARAM, BOOL &);
    LRESULT on_erase(UINT, WPARAM, LPARAM, BOOL &);
    LRESULT on_color(UINT, WPARAM, LPARAM, BOOL &);
    LRESULT on_theme(UINT, WPARAM, LPARAM, BOOL &);

    ToolKind kind_;
    const bool runner_;
    UiTheme theme_{};
    StudioSplitter split_{theme_, false};
    UiPanel request_, response_;
    std::array<HWND, response_headers - target + 1> controls_{};
    ToolSession session_;
    std::optional<CollectionVariables> collection_variables_;
    History history_;
    std::vector<CollectionStep> collection_steps_;
    size_t collection_index_ = 0;
    bool step_finished_ = false, step_failed_ = false, collection_canceled_ = false;
    std::wstring collection_remote_, connection_status_;
    HFONT font_ = nullptr, mono_ = nullptr;
    int dpi_ = 96;
    bool ready_ = false, layout_ = false, closing_ = false;
    bool syncing_query_ = false;
    RequestPage request_page_ = RequestPage::query;
    TaskLayout task_layout_ = TaskLayout::workspace;
  };
} // namespace flowie::app
