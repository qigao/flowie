#include "loopback_broker.h"
#include "routing_broker.h"
#include "tool_peer.h"
#include <fstream>
#include <functional>
#include <memory>
#define TINYTEST_NO_MAIN
// clang-format off
// Select the SDK's compiler TLS ABI before OpenWTL's MSVC compatibility macros.
#include <tinytest.hpp>
#include "main_window.h"
#include "main_workbench.h"
// clang-format on
#include "ui/connection_resource.h"
#include "ui/import_files.h"

CAppModule _Module;
using flowie::app::MainWindow;
namespace {
  HWND find_control(HWND window, int id) {
    if (HWND direct = ::GetDlgItem(window, id)) return direct;
    struct Query {
      int id;
      HWND result;
    } query{id, nullptr};
    ::EnumChildWindows(
        window,
        [](HWND child, LPARAM context) -> BOOL {
          auto &query = *reinterpret_cast<Query *>(context);
          if (::GetDlgCtrlID(child) != query.id) return TRUE;
          query.result = child;
          return FALSE;
        },
        reinterpret_cast<LPARAM>(&query));
    return query.result;
  }
  bool select_request_page(HWND window, const wchar_t *name) {
    const HWND tabs = find_control(window, MainWindow::request_tabs);
    for (int index = 0; index < TabCtrl_GetItemCount(tabs); ++index) {
      wchar_t label[64]{};
      TCITEMW item{};
      item.mask = TCIF_TEXT;
      item.pszText = label;
      item.cchTextMax = 64;
      if (!TabCtrl_GetItem(tabs, index, &item) || std::wcscmp(label, name) != 0) continue;
      TabCtrl_SetCurSel(tabs, index);
      NMHDR notification{tabs, MainWindow::request_tabs, TCN_SELCHANGE};
      ::SendMessageW(window, WM_NOTIFY, MainWindow::request_tabs,
                     reinterpret_cast<LPARAM>(&notification));
      return true;
    }
    return false;
  }
  bool visible_in_request(HWND control, HWND pane) {
    while (control && control != pane) {
      if (!(GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE)) return false;
      control = GetParent(control);
    }
    return control == pane;
  }
  size_t quit_messages = 0;
  void render_workspace(HWND window, const char *path) {
    struct Surface {
      HWND window;
      HDC source = nullptr, target = nullptr;
      HBITMAP bitmap = nullptr;
      HGDIOBJ previous = nullptr;
      ~Surface() {
        if (previous) SelectObject(target, previous);
        if (bitmap) DeleteObject(bitmap);
        if (target) DeleteDC(target);
        if (source) ReleaseDC(window, source);
      }
    } surface{window};
    RECT area{};
    GetClientRect(window, &area);
    if (area.right <= 0 || area.bottom <= 0 || area.right > 4096 || area.bottom > 4096)
      throw std::runtime_error("Unexpected preview dimensions");
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = area.right;
    info.bmiHeader.biHeight = -area.bottom;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    const DWORD bytes = static_cast<DWORD>(area.right * area.bottom * 4);
    surface.source = GetDC(window);
    surface.target = CreateCompatibleDC(surface.source);
    void *pixels = nullptr;
    surface.bitmap = CreateDIBSection(surface.source, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!surface.source || !surface.target || !surface.bitmap)
      throw std::runtime_error("Could not create preview surface");
    surface.previous = SelectObject(surface.target, surface.bitmap);
    RedrawWindow(window, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE | RDW_ALLCHILDREN);
    if (!PrintWindow(window, surface.target, PW_CLIENTONLY))
      throw std::runtime_error("Could not render workspace preview");
    if (!GdiFlush()) throw std::runtime_error("Could not complete workspace preview drawing");
    BITMAPFILEHEADER file{};
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER);
    file.bfSize = file.bfOffBits + bytes;
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char *>(&file), sizeof(file));
    output.write(reinterpret_cast<const char *>(&info.bmiHeader), sizeof(info.bmiHeader));
    output.write(static_cast<const char *>(pixels), bytes);
    if (!output) throw std::runtime_error("Could not write workspace preview");
  }
  bool pump_until(const std::function<bool()> &ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    do {
      MSG message;
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) ++quit_messages;
        else {
          TranslateMessage(&message);
          DispatchMessageW(&message);
        }
      }
      if (ready()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
  }
  class WindowFixture {
  public:
    static void initialize() {
      struct Runtime {
        Runtime() {
          INITCOMMONCONTROLSEX controls{sizeof(controls),
                                        ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES |
                                            ICC_TAB_CLASSES | ICC_TREEVIEW_CLASSES};
          if (!InitCommonControlsEx(&controls))
            throw std::runtime_error("InitCommonControlsEx failed: " +
                                     std::to_string(GetLastError()));
          const auto result = _Module.Init(nullptr, GetModuleHandleW(nullptr));
          if (FAILED(result))
            throw std::runtime_error("CAppModule::Init failed: " + std::to_string(result));
        }
        ~Runtime() { _Module.Term(); }
      };
      static Runtime runtime; // WTL module lifetime is the process, not one window.
    }
    WindowFixture() {
      initialize();
      window = std::make_unique<MainWindow>();
      RECT bounds{0, 0, 1200, 900};
      if (!window->Create(nullptr, bounds, L"Workbench test",
                          WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN)) {
        throw std::runtime_error("Window creation failed");
      }
    }
    ~WindowFixture() {
      if (IsWindow(*window)) {
        SendMessageW(*window, WM_CLOSE, 0, 0);
        pump_until([&] { return !IsWindow(*window); });
      }
      window.reset();
    }
    HWND control(int id) { return find_control(*window, id); }
    void set(int id, const wchar_t *value) { SetWindowTextW(control(id), value); }
    void click(int id) {
      if (id == MainWindow::subscribe_button || id == MainWindow::unsubscribe_button)
        select_request_page(*window, L"Topics");
      SendMessageW(*window, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED),
                   reinterpret_cast<LPARAM>(control(id)));
    }
    std::wstring text(int id) {
      std::wstring value(GetWindowTextLengthW(control(id)) + 1, L'\0');
      const int size = GetWindowTextW(control(id), value.data(), static_cast<int>(value.size()));
      value.resize(size);
      return value;
    }
    std::unique_ptr<MainWindow> window;
  };
  using flowie::app::ProtocolWindow;
  using flowie::app::RecentConnections;
  using flowie::app::WorkbenchWindow;
  struct TestHistoryKey {
    std::wstring path;
    TestHistoryKey() {
      static unsigned sequence = 0;
      path = L"Software\\FlowieWorkbenchTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++sequence);
    }
    ~TestHistoryKey() { RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()); }
  };

  // Drive the native modal loop on its owning UI thread, without test hooks in production.
  struct DialogAction {
    HWND owner;
    int answer;
    std::function<void(HWND)> prepare;
    int field_id = IDC_BROKER_HOST;
    int error_id = IDC_CONNECTION_ERROR;
    bool seen = false;
    std::exception_ptr error;
    UINT_PTR timer = 0;
    static thread_local DialogAction *pending;

    static void CALLBACK tick(HWND, UINT, UINT_PTR id, DWORD) {
      if (!pending || pending->timer != id) return;
      auto &action = *pending;
      const auto dialog = GetLastActivePopup(action.owner);
      if (!GetDlgItem(dialog, action.field_id) || action.seen) return;
      action.seen = true;
      try {
        if (action.prepare) action.prepare(dialog);
        SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(action.answer, BN_CLICKED), 0);
        // EndDialog completes when the modal loop resumes; the HWND can still exist here.
        if (GetWindowTextLengthW(GetDlgItem(dialog, action.error_id)) > 0)
          SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
      } catch (...) {
        action.error = std::current_exception();
        SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
      }
    }

    void run(int command, HWND button = nullptr) {
      run([=] {
        if (button) SendMessageW(button, BM_CLICK, 0, 0);
        else SendMessageW(owner, WM_COMMAND, MAKEWPARAM(command, BN_CLICKED), 0);
      });
    }

    void run(const std::function<void()> &command) {
      pending = this;
      timer = SetTimer(nullptr, 0, 10, tick);
      if (!timer) {
        pending = nullptr;
        throw std::runtime_error("Could not start dialog test driver");
      }
      {
        struct ResetTimer {
          UINT_PTR timer;
          ~ResetTimer() {
            KillTimer(nullptr, timer);
            DialogAction::pending = nullptr;
          }
        } reset{timer};
        command();
      }
      if (error) std::rethrow_exception(error);
    }
  };
  thread_local DialogAction *DialogAction::pending = nullptr;

  class ImportFiles {
  public:
    ImportFiles() {
      static unsigned sequence = 0;
      dir = std::filesystem::temp_directory_path() /
            (L"flowie-import-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++sequence));
      if (!std::filesystem::create_directory(dir))
        throw std::runtime_error("Could not create test files directory");
    }
    ~ImportFiles() {
      std::error_code error;
      for (const auto &file : files)
        std::filesystem::remove(file, error);
      std::filesystem::remove(dir, error);
    }
    std::filesystem::path reserve(const wchar_t *name) {
      files.push_back(dir / name);
      return files.back();
    }
    std::filesystem::path write(const wchar_t *name, std::string_view bytes) {
      const auto target = reserve(name);
      std::ofstream output(target, std::ios::binary);
      output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      output.close();
      if (!output) throw std::runtime_error("Could not write test import file");
      return target;
    }
    std::filesystem::path dir;
    std::vector<std::filesystem::path> files;
  };

  class WorkspaceFixture {
  public:
    explicit WorkspaceFixture(bool seed = true, const std::wstring &key = {},
                              const std::filesystem::path &library = {})
        : window(key.empty() ? history_key.path : key,
                 library.empty() ? file_workspace.reserve(L"collections.json") : library) {
      WindowFixture::initialize();
      RECT bounds{0, 0, 1200, 960};
      if (!window.Create(nullptr, bounds, L"Multi-client test",
                         WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN))
        throw std::runtime_error("Workspace creation failed");
      if (seed) action(WorkbenchWindow::add_client);
    }
    ~WorkspaceFixture() {
      if (IsWindow(window)) {
        SendMessageW(window, WM_CLOSE, 0, 0);
        pump_until([&] { return !IsWindow(window); });
      }
    }
    int count() { return TabCtrl_GetItemCount(find_control(window, WorkbenchWindow::client_tabs)); }
    HWND close_button(int index) {
      TCITEMW item{};
      item.mask = TCIF_PARAM;
      if (!TabCtrl_GetItem(find_control(window, WorkbenchWindow::client_tabs), index, &item))
        throw std::runtime_error("Could not read tab close control");
      return reinterpret_cast<HWND>(item.lParam);
    }
    HWND active() {
      for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        wchar_t name[128]{};
        GetClassNameW(child, name, 128);
        if ((std::wcscmp(name, L"FlowieMqttWorkbench") == 0 ||
             std::wcscmp(name, L"FlowieProtocolRequest") == 0) &&
            (GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE))
          return child;
      }
      throw std::runtime_error("No active client child window");
    }
    void select(int index) {
      HWND tabs = find_control(window, WorkbenchWindow::client_tabs);
      TabCtrl_SetCurSel(tabs, index);
      NMHDR notification{tabs, WorkbenchWindow::client_tabs, TCN_SELCHANGE};
      SendMessageW(window, WM_NOTIFY, WorkbenchWindow::client_tabs,
                   reinterpret_cast<LPARAM>(&notification));
    }
    void sidebar(int page) {
      const HWND tabs = find_control(window, WorkbenchWindow::sidebar_tabs);
      TabCtrl_SetCurSel(tabs, page);
      NMHDR notification{tabs, WorkbenchWindow::sidebar_tabs, TCN_SELCHANGE};
      SendMessageW(window, WM_NOTIFY, WorkbenchWindow::sidebar_tabs,
                   reinterpret_cast<LPARAM>(&notification));
    }
    void open_request(HWND tree, HTREEITEM item) {
      TreeView_SelectItem(tree, item);
      NMHDR notification{tree, WorkbenchWindow::collections_tree, NM_RETURN};
      SendMessageW(window, WM_NOTIFY, WorkbenchWindow::collections_tree,
                   reinterpret_cast<LPARAM>(&notification));
    }
    void action(int id) {
      if (id == WorkbenchWindow::add_publisher || id == WorkbenchWindow::add_subscriber ||
          id == WorkbenchWindow::add_client) {
        DialogAction open{window, IDC_OPEN_CONNECTION_TAB, {}};
        open.run(id);
      } else SendMessageW(window, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), 0);
    }
    static void set(HWND pane, int id, const wchar_t *value) {
      SetWindowTextW(find_control(pane, id), value);
    }
    static void click(HWND pane, int id) {
      if (id == MainWindow::subscribe_button || id == MainWindow::unsubscribe_button)
        select_request_page(pane, L"Topics");
      SendMessageW(pane, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED),
                   reinterpret_cast<LPARAM>(find_control(pane, id)));
    }
    static std::wstring text(HWND pane, int id) {
      HWND control = find_control(pane, id);
      std::wstring value(GetWindowTextLengthW(control) + 1, L'\0');
      int size = GetWindowTextW(control, value.data(), static_cast<int>(value.size()));
      value.resize(size);
      return value;
    }
    static bool ready(HWND pane) {
      return IsWindowEnabled(find_control(pane, MainWindow::ping_button)) != FALSE;
    }
    static bool counters(HWND pane, int rx, int tx) {
      return text(pane, MainWindow::counters)
                 .find(L"RX " + std::to_wstring(rx) + L"   TX " + std::to_wstring(tx) + L" ") == 0;
    }
    static void connect(HWND pane, int port) {
      set(pane, MainWindow::port, std::to_wstring(port).c_str());
      click(pane, MainWindow::connect_button);
    }
    TestHistoryKey history_key;
    ImportFiles file_workspace;
    WorkbenchWindow window;
  };
} // namespace
suite("Workbench native window") {
  it("validates JSON, connects, subscribes and displays the received payload through real "
     "controls") {
    LoopbackBroker broker;
    WindowFixture view;
    check_false(IsWindowEnabled(view.control(MainWindow::publish_button)));
    view.set(MainWindow::port, std::to_wstring(broker.port).c_str());
    view.set(MainWindow::payload, L"{broken}");
    view.click(MainWindow::validate_button);
    check_false(view.text(MainWindow::status) == L"Valid JSON.");
    view.set(MainWindow::payload, L"{\"message\":\"你好\",\"id\":18446744073709551615}");
    view.click(MainWindow::format_button);
    check_true(view.text(MainWindow::payload).find(L"18446744073709551615") != std::wstring::npos);
    SendMessageW(view.control(MainWindow::mode), CB_SETCURSEL, 1, 0);
    view.click(MainWindow::connect_button);
    check_true(pump_until(
        [&] { return IsWindowEnabled(view.control(MainWindow::publish_button)) != FALSE; }));
    check_false(IsWindowEnabled(view.control(MainWindow::host)));
    const auto formatted = view.text(MainWindow::payload);
    view.set(MainWindow::payload, L"{broken}");
    view.click(MainWindow::publish_button);
    check_true(view.text(MainWindow::counters).find(L"TX 0") != std::wstring::npos);
    view.set(MainWindow::payload, formatted.c_str());
    SendMessageW(view.control(MainWindow::sub_qos), CB_SETCURSEL, 2, 0);
    view.click(MainWindow::subscribe_button);
    check_true(pump_until([&] {
      return SendMessageW(view.control(MainWindow::subscriptions), LB_GETCOUNT, 0, 0) == 1;
    }));
    view.click(MainWindow::publish_button);
    check_true(pump_until([&] {
      return view.text(MainWindow::counters).find(L"RX 1   TX 1") != std::wstring::npos;
    }));
    auto list = view.control(MainWindow::records);
    bool saw_payload = false;
    for (int i = 0; i < ListView_GetItemCount(list); ++i) {
      ListView_SetItemState(list, i, LVIS_SELECTED, LVIS_SELECTED);
      if (view.text(MainWindow::details).find(L"你好") != std::wstring::npos) saw_payload = true;
    }
    check_true(saw_payload);
    const HWND message_filter = view.control(MainWindow::message_filter);
    for (const auto &[choice, event] : {std::pair{1, L"RX"}, std::pair{2, L"TX queued"}}) {
      SendMessageW(message_filter, CB_SETCURSEL, choice, 0);
      SendMessageW(*view.window, WM_COMMAND, MAKEWPARAM(MainWindow::message_filter, CBN_SELCHANGE),
                   reinterpret_cast<LPARAM>(message_filter));
      check_equal(ListView_GetItemCount(list), 1);
      wchar_t kind[32]{};
      ListView_GetItemText(list, 0, 1, kind, 32);
      check_true(std::wcscmp(kind, event) == 0);
      check_true(view.text(MainWindow::details).find(L"你好") != std::wstring::npos);
    }
    SendMessageW(message_filter, CB_SETCURSEL, 0, 0);
    SendMessageW(*view.window, WM_COMMAND, MAKEWPARAM(MainWindow::message_filter, CBN_SELCHANGE),
                 reinterpret_cast<LPARAM>(message_filter));
    view.click(MainWindow::unsubscribe_button);
    check_true(pump_until([&] {
      return SendMessageW(view.control(MainWindow::subscriptions), LB_GETCOUNT, 0, 0) == 0;
    }));
    view.click(MainWindow::disconnect_button);
    check_true(pump_until(
        [&] { return IsWindowEnabled(view.control(MainWindow::connect_button)) != FALSE; }));
    check_equal(broker.failure.load(), 0);
  }
  it("shows a rejected SUBACK without committing an active subscription") {
    LoopbackBroker broker(false, false, true);
    WindowFixture view;
    view.set(MainWindow::port, std::to_wstring(broker.port).c_str());
    view.click(MainWindow::connect_button);
    check_true(pump_until(
        [&] { return IsWindowEnabled(view.control(MainWindow::subscribe_button)) != FALSE; }));
    view.click(MainWindow::subscribe_button);
    check_true(pump_until([&] {
      return view.text(MainWindow::status).find(L"broker rejected") != std::wstring::npos;
    }));
    check_equal(SendMessageW(view.control(MainWindow::subscriptions), LB_GETCOUNT, 0, 0),
                LRESULT(0));
    check_true(IsWindowEnabled(view.control(MainWindow::publish_button)) != FALSE);
  }
  it("keeps transport choices usable and closes while a broker withholds CONNACK") {
    LoopbackBroker broker(false, true);
    WindowFixture view;
    const HWND transport = view.control(MainWindow::transport);
    RECT closed{}, drop{};
    check_true(GetWindowRect(transport, &closed) != FALSE);
    check_true(SendMessageW(transport, CB_GETDROPPEDCONTROLRECT, 0,
                            reinterpret_cast<LPARAM>(&drop)) != FALSE);
    const auto count = SendMessageW(transport, CB_GETCOUNT, 0, 0);
    const auto row_height = SendMessageW(transport, CB_GETITEMHEIGHT, 0, 0);
    check_equal(count, LRESULT(4));
    check_greater(row_height, LRESULT(0));
    const auto list_height = (drop.bottom - drop.top) - (closed.bottom - closed.top);
    check_greater_equal(LRESULT(list_height), count * row_height);
    view.set(MainWindow::port, std::to_wstring(broker.port).c_str());
    view.click(MainWindow::connect_button);
    SendMessageW(*view.window, WM_CLOSE, 0, 0);
    check_true(pump_until([&] { return !IsWindow(*view.window); }));
  }
}

suite("Workbench protocol tools") {
  it("sends an HTTP request and shows the response headers and body in the added tab") {
    ToolPeer peer(ToolPeer::Mode::http, "hello from HTTP");
    WorkspaceFixture view;
    const HWND mqtt = view.active();
    check_true(IsWindowEnabled(find_control(view.window, WorkbenchWindow::choose_new_request)));
    const HMENU file = GetSubMenu(GetMenu(view.window), 0);
    const HMENU requests = GetSubMenu(file, 0);
    check_equal(GetMenuItemCount(requests), 5);
    const auto http_item = GetMenuItemID(requests, 1);
    check_equal(http_item, static_cast<UINT>(WorkbenchWindow::add_http));
    view.action(http_item);
    const HWND pane = view.active();
    check_not_equal(pane, mqtt);
    const HWND primary = find_control(view.window, WorkbenchWindow::primary_request);
    check_true(view.text(view.window, WorkbenchWindow::primary_request) == L"Send Request");
    check_false(IsWindowEnabled(find_control(view.window, WorkbenchWindow::disconnect_active)));
    view.set(pane, ProtocolWindow::target,
             (L"http://127.0.0.1:" + std::to_wstring(peer.port) + L"/hello?q=one").c_str());
    view.set(pane, ProtocolWindow::headers, L"X-Workbench: test");
    SendMessageW(primary, BM_CLICK, 0, 0);
    const bool completed = pump_until([&] {
      return view.text(pane, ProtocolWindow::detail).find(L"hello from HTTP") != std::wstring::npos;
    });
    check(completed, "HTTP status: %ls; details: %ls; peer received: %d",
          view.text(pane, ProtocolWindow::report).c_str(),
          view.text(pane, ProtocolWindow::detail).c_str(), static_cast<int>(peer.received.load()));
    check_true(view.text(pane, ProtocolWindow::detail).find(L"HTTP 200") != std::wstring::npos);
    check_true(view.text(pane, ProtocolWindow::detail).find(L"X-Fixture: loopback") !=
               std::wstring::npos);
    check_true(peer.request().find("GET /hello?q=one HTTP/1.1") != std::string::npos);
    check_true(peer.request().find("X-Workbench: test") != std::string::npos);
    view.window.MoveWindow(0, 0, 1440, 1000, TRUE);
    view.window.ShowWindow(SW_SHOWNOACTIVATE);
    view.window.UpdateWindow();
    render_workspace(view.window, "workbench-http.bmp");
    SendMessageW(view.close_button(1), BM_CLICK, 0, 0);
    check_true(pump_until([&] { return view.count() == 1; }));
    check_true(view.active() == mqtt);
    check_true(view.text(view.window, WorkbenchWindow::primary_request) == L"Connect");
  }

  it("correlates a JSON-RPC result and closes an unrelated pending HTTP request safely") {
    ToolPeer rpc(ToolPeer::Mode::http,
                 R"({"jsonrpc":"2.0","id":"flowie-workbench","result":{"ready":true}})");
    ToolPeer silent(ToolPeer::Mode::silent_http);
    WorkspaceFixture view(false);
    view.action(WorkbenchWindow::add_rpc);
    const HWND pane = view.active();
    view.set(pane, ProtocolWindow::target,
             (L"http://127.0.0.1:" + std::to_wstring(rpc.port) + L"/rpc").c_str());
    view.set(pane, ProtocolWindow::method, L"service.status");
    view.set(pane, ProtocolWindow::body, L"{\"verbose\":true}");
    view.action(WorkbenchWindow::publish_active);
    const bool completed = pump_until([&] {
      return view.text(pane, ProtocolWindow::detail).find(L"JSON-RPC result") != std::wstring::npos;
    });
    check(completed, "RPC status: %ls; details: %ls; peer received: %d",
          view.text(pane, ProtocolWindow::report).c_str(),
          view.text(pane, ProtocolWindow::detail).c_str(), static_cast<int>(rpc.received.load()));
    check_true(rpc.request().find(R"("method":"service.status")") != std::string::npos);
    check_true(rpc.request().find(R"("id":"flowie-workbench")") != std::string::npos);
    check_true(rpc.request().find(R"("params":{"verbose":true})") != std::string::npos);
    view.action(WorkbenchWindow::add_http);
    const HWND pending = view.active();
    view.set(pending, ProtocolWindow::target,
             (L"http://127.0.0.1:" + std::to_wstring(silent.port) + L"/waiting").c_str());
    view.click(pending, ProtocolWindow::run);
    const HWND stop = find_control(view.window, WorkbenchWindow::disconnect_active);
    check_true(pump_until([&] { return silent.received.load() && IsWindowEnabled(stop); }));
    check_true(view.text(view.window, WorkbenchWindow::disconnect_active) == L"Cancel Request");
    SendMessageW(stop, BM_CLICK, 0, 0);
    check_true(pump_until([&] { return silent.disconnected.load() && !IsWindowEnabled(stop); }));
    check_equal(view.count(), 2);
    SendMessageW(view.close_button(1), BM_CLICK, 0, 0);
    check_true(pump_until([&] { return view.count() == 1 && silent.disconnected.load(); }));
    check_true(view.active() == pane);
  }

  it("starts a TLS handshake and sends no HTTP request to a plaintext peer") {
    ToolPeer plaintext(ToolPeer::Mode::echo);
    WorkspaceFixture view(false);
    view.action(WorkbenchWindow::add_http);
    const HWND pane = view.active();
    const auto url = L"https://127.0.0.1:" + std::to_wstring(plaintext.port) + L"/test";
    view.set(pane, ProtocolWindow::target, url.c_str());
    view.click(pane, ProtocolWindow::run);
    const bool handshaking = pump_until([&] { return plaintext.received.load(); });
    check(handshaking, "HTTPS status: %ls; details: %ls",
          view.text(pane, ProtocolWindow::report).c_str(),
          view.text(pane, ProtocolWindow::detail).c_str());
    const auto wire = plaintext.request();
    check_true(!wire.empty() && static_cast<unsigned char>(wire.front()) == 0x16);
    check_true(wire.find("GET /test HTTP/1.1") == wire.npos);
    check_true(pump_until([&] { return plaintext.disconnected.load(); }));
    check_true(pump_until([&] {
      return view.text(pane, ProtocolWindow::run).find(L"Send request") != std::wstring::npos;
    }));
    check_true(view.text(pane, ProtocolWindow::detail).find(L"HTTP 200") == std::wstring::npos);
  }

  it("sends exact hexadecimal bytes over TCP and reports an unavailable serial port") {
    ToolPeer peer(ToolPeer::Mode::echo);
    WorkspaceFixture view(false);
    view.action(WorkbenchWindow::add_tcp);
    const HWND tcp = view.active();
    view.set(tcp, ProtocolWindow::target,
             (L"tcp://127.0.0.1:" + std::to_wstring(peer.port)).c_str());
    check_true(view.text(view.window, WorkbenchWindow::primary_request) == L"Connect");
    view.action(WorkbenchWindow::connect_active);
    check_true(pump_until([&] {
      return IsWindowEnabled(find_control(tcp, ProtocolWindow::send)) &&
             view.text(view.window, WorkbenchWindow::primary_request) == L"Send Bytes";
    }));
    view.set(tcp, ProtocolWindow::body, L"00 FF 41");
    SendMessageW(find_control(tcp, ProtocolWindow::hex), BM_SETCHECK, BST_CHECKED, 0);
    view.action(WorkbenchWindow::publish_active);
    check_true(pump_until([&] { return peer.request().size() == 3; }));
    check_true(peer.request() == std::string("\0\xff\x41", 3));
    check_true(pump_until([&] {
      return view.text(tcp, ProtocolWindow::detail).find(L"00 FF 41") != std::wstring::npos;
    }));
    view.action(WorkbenchWindow::add_serial);
    const HWND serial = view.active();
    view.set(serial, ProtocolWindow::target, L"COM65535");
    check_true(view.text(view.window, WorkbenchWindow::primary_request) == L"Open Port");
    const HMENU session = GetSubMenu(GetMenu(view.window), 1);
    SendMessageW(view.window, WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(session), 0);
    check_equal(GetMenuState(session, WorkbenchWindow::refresh_ports_active, MF_BYCOMMAND) &
                    MF_GRAYED,
                UINT{0});
    SendMessageW(find_control(view.window, WorkbenchWindow::primary_request), BM_CLICK, 0, 0);
    check_true(pump_until([&] {
      return SendMessageW(find_control(serial, ProtocolWindow::rows), LB_GETCOUNT, 0, 0) > 0;
    }));
    check_false(IsWindowEnabled(find_control(serial, ProtocolWindow::send)));
    check_true(IsWindowEnabled(find_control(tcp, ProtocolWindow::send)));
    view.select(0);
    view.action(WorkbenchWindow::disconnect_active);
    check_true(pump_until([&] { return peer.disconnected.load(); }));
  }

  it("routes payload tools and layout to the active protocol from the global menus") {
    ImportFiles files;
    const auto json = files.write(L"sample.json", R"({"sensor":17,"items":[1,2]})");
    const auto invalid_rpc = files.write(L"invalid.json", R"("scalar")");
    WorkspaceFixture view(false);
    view.action(WorkbenchWindow::add_rpc);
    const auto rpc = view.active();
    view.window.import_sample(json);
    check_true(view.text(rpc, ProtocolWindow::body) == L"{\"sensor\":17,\"items\":[1,2]}");
    check_throws_as(view.window.import_sample(invalid_rpc), std::runtime_error);
    view.action(WorkbenchWindow::format_active);
    const auto formatted = view.text(rpc, ProtocolWindow::body);
    check_true(formatted.find(L"\r\n") != formatted.npos);
    check_true(view.text(rpc, ProtocolWindow::report) == L"JSON formatted");

    view.action(WorkbenchWindow::view_monitor);
    check_false(GetWindowLongPtrW(GetParent(find_control(rpc, ProtocolWindow::body)), GWL_STYLE) &
                WS_VISIBLE);
    view.action(WorkbenchWindow::add_serial);
    const auto serial = view.active();
    view.window.import_sample(json);
    const auto editor = GetParent(find_control(serial, ProtocolWindow::body));
    const auto output = GetParent(find_control(serial, ProtocolWindow::detail));
    view.action(WorkbenchWindow::view_publish);
    check_true(GetWindowLongPtrW(editor, GWL_STYLE) & WS_VISIBLE);
    check_false(GetWindowLongPtrW(output, GWL_STYLE) & WS_VISIBLE);
    view.select(0);
    check_true(view.text(rpc, ProtocolWindow::body) == formatted);
    check_false(GetWindowLongPtrW(GetParent(find_control(rpc, ProtocolWindow::body)), GWL_STYLE) &
                WS_VISIBLE);
    view.action(WorkbenchWindow::view_workspace);
    check_true(GetWindowLongPtrW(GetParent(find_control(rpc, ProtocolWindow::body)), GWL_STYLE) &
               WS_VISIBLE);
  }

  it("reopens a saved TCP request without replacing the MQTT reader or connecting") {
    ImportFiles files;
    const auto path = files.reserve(L"collections.json");
    flowie::app::SavedRequest saved;
    saved.collection = "Test tools";
    saved.name = "Echo";
    saved.tool = flowie::app::ToolDraft{
        flowie::app::ToolKind::tcp, "tcp://127.0.0.1:9000", {}, {}, "00 FF", true};
    check_true(flowie::app::CollectionStore(path).save(saved, false).has_value());
    WorkspaceFixture view(true, {}, path);
    const HWND tree = find_control(view.window, WorkbenchWindow::collections_tree);
    const auto group = TreeView_GetChild(tree, TreeView_GetRoot(tree));
    view.open_request(tree, TreeView_GetChild(tree, group));
    check_equal(view.count(), 2);
    const auto pane = view.active();
    check_true(view.text(pane, ProtocolWindow::target) == L"tcp://127.0.0.1:9000");
    check_true(view.text(pane, ProtocolWindow::body) == L"00 FF");
    check_false(IsWindowEnabled(find_control(pane, ProtocolWindow::send)));
  }
}

suite("Workbench connection menu and history") {
  it("opens a modal from each MQTT role in New Request and creates no tab on cancel") {
    WorkspaceFixture view(false);
    check_equal(view.count(), 0);
    check_true(GetMenu(view.window) != nullptr);
    const auto file = GetSubMenu(GetMenu(view.window), 0);
    const auto mqtt = GetSubMenu(GetSubMenu(file, 0), 0);
    for (const auto id : {WorkbenchWindow::add_client, WorkbenchWindow::add_publisher,
                          WorkbenchWindow::add_subscriber}) {
      bool owner_disabled = false;
      DialogAction cancel{view.window, IDCANCEL,
                          [&](HWND) { owner_disabled = !IsWindowEnabled(view.window); }};
      check_true(GetMenuState(mqtt, id, MF_BYCOMMAND) != UINT(-1));
      cancel.run(id);
      check_true(cancel.seen);
      check_true(owner_disabled);
      check_true(IsWindowEnabled(view.window));
      check_equal(view.count(), 0);
    }
    check_true(RecentConnections(view.history_key.path).load().empty());
  }

  it("recalls a successful broker from a new workspace without restoring credentials") {
    TestHistoryKey history;
    RoutingBroker broker;
    const auto port = std::to_wstring(broker.port);
    {
      WorkspaceFixture view(false, history.path);
      DialogAction open{view.window, IDOK, [&](HWND dialog) {
                          SetDlgItemTextW(dialog, IDC_BROKER_PORT, port.c_str());
                          SetDlgItemTextW(dialog, IDC_BROKER_USER, L"test-username");
                          SetDlgItemTextW(dialog, IDC_BROKER_PASSWORD, L"test-password");
                        }};
      open.run(WorkbenchWindow::add_subscriber);
      check_true(open.seen);
      check_equal(view.count(), 1);
      check_true(pump_until([&] { return view.ready(view.active()); }));
      const auto entries = RecentConnections(history.path).load();
      check_equal(entries.size(), size_t{1});
      check_equal(entries[0].port, broker.port);
      check_true(entries[0].role == flowie::app::ClientRole::subscriber);
    }
    {
      WorkspaceFixture view(false, history.path);
      const HMENU connection = GetSubMenu(GetMenu(view.window), 1);
      const HMENU recent = GetSubMenu(connection, 4);
      SendMessageW(view.window, WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(recent), 4);
      check_equal(GetMenuItemID(recent, 0), static_cast<UINT>(WorkbenchWindow::first_recent));
      std::wstring recalled_port;
      bool blank_credentials = false;
      DialogAction open{view.window, IDC_OPEN_CONNECTION_TAB, [&](HWND dialog) {
                          recalled_port = view.text(dialog, IDC_BROKER_PORT);
                          blank_credentials = view.text(dialog, IDC_BROKER_USER).empty() &&
                                              view.text(dialog, IDC_BROKER_PASSWORD).empty();
                        }};
      open.run(WorkbenchWindow::first_recent);
      check_true(open.seen);
      check_true(recalled_port == port);
      check_true(blank_credentials);
      check_equal(view.count(), 1);
      check_true(view.text(view.active(), MainWindow::port) == port);
      view.connect(view.active(), broker.port);
      check_true(pump_until([&] { return view.ready(view.active()); }));
      check_false(IsWindowEnabled(find_control(view.active(), MainWindow::publish_button)));
      check_equal(RecentConnections(history.path).load().size(), size_t{1});
    }
  }

  it("leaves a rejected broker out of persisted connection history") {
    LoopbackBroker broker(true);
    WorkspaceFixture view(false);
    DialogAction open{view.window, IDOK, [&](HWND dialog) {
                        SetDlgItemTextW(dialog, IDC_BROKER_PORT,
                                        std::to_wstring(broker.port).c_str());
                      }};
    open.run(WorkbenchWindow::add_client);
    check_true(open.seen);
    check_equal(view.count(), 1);
    check_true(pump_until([&] {
      return view.text(view.active(), MainWindow::status).find(L"broker rejected") !=
                 std::wstring::npos ||
             view.text(view.active(), MainWindow::status).find(L"Closed") == 0;
    }));
    check_true(RecentConnections(view.history_key.path).load().empty());
  }

  it("keeps the most recent distinct endpoints within the saved limit") {
    TestHistoryKey history;
    const auto store = RecentConnections(history.path);
    flowie::app::RecentConnection entry{flowie::app::ClientRole::both, L"localhost", 1883,
                                        FLOWIE_MQTT_CLIENT_TRANSPORT_TCP, FLOWIE_MQTT_VERSION_5};
    for (size_t i = 0; i <= flowie::app::kRecentConnectionLimit; ++i) {
      entry.port = 1883 + static_cast<int>(i);
      store.remember(entry);
    }
    entry.port = 1884;
    entry.host = L"LOCALHOST";
    store.remember(entry);
    const auto result = store.load();
    check_equal(result.size(), flowie::app::kRecentConnectionLimit);
    check_equal(result.front().port, 1884);
    check_equal(result.back().port, 1885);
  }
}

suite("Workbench saved Collections") {
  it("saves a named request from the UI and restores an offline tab without broker credentials") {
    RoutingBroker broker;
    ImportFiles files;
    const auto library = files.reserve(L"collections.json");
    const std::wstring draft = L"{\"message\":\"sensor on\"}\r\n";
    {
      WorkspaceFixture view(true, {}, library);
      const HWND pane = view.active();
      view.set(pane, MainWindow::username, L"collection-test-account");
      view.set(pane, MainWindow::password, L"collection-test-secret");
      view.connect(pane, broker.port);
      check_true(pump_until([&] { return view.ready(pane); }));
      view.set(pane, MainWindow::filter, L"workbench/#");
      SendMessageW(find_control(pane, MainWindow::sub_qos), CB_SETCURSEL, 1, 0);
      view.click(pane, MainWindow::subscribe_button);
      check_true(pump_until([&] { return view.ready(pane); }));
      view.set(pane, MainWindow::filter, L"workbench/status/#");
      SendMessageW(find_control(pane, MainWindow::sub_qos), CB_SETCURSEL, 2, 0);
      view.set(pane, MainWindow::topic, L"workbench/status/sensor");
      view.set(pane, MainWindow::payload, draft.c_str());
      SendMessageW(find_control(pane, MainWindow::mode), CB_SETCURSEL, 1, 0);
      SendMessageW(find_control(pane, MainWindow::pub_qos), CB_SETCURSEL, 1, 0);
      SendMessageW(find_control(pane, MainWindow::retain), BM_SETCHECK, BST_CHECKED, 0);
      DialogAction save{view.window, IDOK, [](HWND dialog) {
                          SetDlgItemTextW(dialog, IDC_SAVE_COLLECTION, L"Integration");
                          SetDlgItemTextW(dialog, IDC_SAVE_REQUEST_NAME, L"Sensor status");
                        }};
      save.field_id = IDC_SAVE_REQUEST_NAME;
      save.error_id = IDC_SAVE_REQUEST_ERROR;
      save.run(WorkbenchWindow::save_request,
               find_control(view.window, WorkbenchWindow::save_request));
      check_true(save.seen);
      const auto requests = flowie::app::CollectionStore(library).load();
      check_equal(requests.size(), size_t{1});
      check_true(requests[0].collection == "Integration");
      check_true(requests[0].name == "Sensor status");
      check_equal(requests[0].profile.connection.port, broker.port);
      check_equal(requests[0].topics.size(), size_t{2});
      check_true(requests[0].message.has_value());
      check_true(requests[0].message->payload == "{\"message\":\"sensor on\"}\r\n");
      std::ifstream input(library, std::ios::binary);
      const std::string saved((std::istreambuf_iterator<char>(input)), {});
      check_true(saved.find("collection-test-account") == saved.npos);
      check_true(saved.find("collection-test-secret") == saved.npos);
      check_true(view.counters(pane, 0, 0));
      check_true(view.ready(pane));
    }
    {
      WorkspaceFixture view(false, {}, library);
      const HWND tree = find_control(view.window, WorkbenchWindow::collections_tree);
      const auto group = TreeView_GetChild(tree, TreeView_GetRoot(tree));
      check_true(group != nullptr);
      const auto item = TreeView_GetChild(tree, group);
      check_true(item != nullptr);
      view.open_request(tree, item);
      check_equal(view.count(), 1);
      const HWND pane = view.active();
      check_false(view.ready(pane));
      check_true(view.text(pane, MainWindow::username).empty());
      check_true(view.text(pane, MainWindow::password).empty());
      check_true(view.text(pane, MainWindow::payload) == draft);
      check_true(view.text(pane, MainWindow::topic) == L"workbench/status/sensor");
      check_true(view.counters(pane, 0, 0));
      const HWND topics = find_control(pane, MainWindow::saved_topic_picker);
      check_equal(SendMessageW(topics, CB_GETCOUNT, 0, 0), LRESULT{2});
      view.connect(pane, broker.port);
      check_true(pump_until([&] { return view.ready(pane); }));
      const HWND active_topics = find_control(pane, MainWindow::subscriptions);
      check_equal(SendMessageW(active_topics, LB_GETCOUNT, 0, 0), LRESULT{0});
      select_request_page(pane, L"Topics");
      SendMessageW(topics, CB_SETCURSEL, 1, 0);
      SendMessageW(pane, WM_COMMAND, MAKEWPARAM(MainWindow::saved_topic_picker, CBN_SELCHANGE),
                   reinterpret_cast<LPARAM>(topics));
      check_true(view.text(pane, MainWindow::filter) == L"workbench/status/#");
      view.click(pane, MainWindow::subscribe_button);
      check_true(pump_until(
          [&] { return view.ready(pane) && SendMessageW(active_topics, LB_GETCOUNT, 0, 0) == 1; }));
      view.click(pane, MainWindow::publish_button);
      check_true(pump_until([&] { return view.counters(pane, 1, 1) && view.ready(pane); }));
      view.window.MoveWindow(0, 0, 1440, 1000, TRUE);
      view.window.ShowWindow(SW_SHOWNOACTIVATE);
      view.window.UpdateWindow();
      render_workspace(view.window, "workbench-collections.bmp");
    }
  }

  it("loads distinct files and manages them without changing open requests or deleting data") {
    ImportFiles files;
    const auto first = files.reserve(L"lab.json");
    const auto second = files.reserve(L"staging.json");
    flowie::app::SavedRequest request;
    request.collection = "Examples";
    request.name = "Echo";
    request.tool = flowie::app::ToolDraft{flowie::app::ToolKind::tcp, "tcp://127.0.0.1:9001"};
    check_true(flowie::app::CollectionStore(first).save(request, false).has_value());
    request.tool->endpoint = "tcp://127.0.0.1:9002";
    check_true(flowie::app::CollectionStore(second).save(request, false).has_value());

    WorkspaceFixture view(false);
    const HWND toolbar = find_control(view.window, WorkbenchWindow::browse_collections);
    check_true(GetParent(toolbar) == view.window);
    const HWND tree = find_control(view.window, WorkbenchWindow::collections_tree);
    check_true(GetParent(find_control(view.window, WorkbenchWindow::save_request)) == view.window);
    check_true(find_control(view.window, WorkbenchWindow::open_saved_request) == nullptr);
    const HMENU file = GetSubMenu(GetMenu(view.window), 0);
    check_true(GetMenuState(file, WorkbenchWindow::browse_collections, MF_BYCOMMAND) != UINT(-1));
    check_true(TreeView_GetRoot(tree) == nullptr);
    view.window.load_collections(first);
    view.window.load_collections(second);
    const auto first_root = TreeView_GetRoot(tree);
    const auto second_root = TreeView_GetNextSibling(tree, first_root);
    check_true(second_root != nullptr);
    const auto group = TreeView_GetChild(tree, first_root);
    view.open_request(tree, TreeView_GetChild(tree, group));
    check_equal(view.count(), 1);
    const HWND pane = view.active();
    check_true(view.text(pane, ProtocolWindow::target) == L"tcp://127.0.0.1:9001");
    view.open_request(tree, TreeView_GetChild(tree, TreeView_GetChild(tree, second_root)));
    check_equal(view.count(), 2);
    check_true(view.text(view.active(), ProtocolWindow::target) == L"tcp://127.0.0.1:9002");

    view.sidebar(2);
    check_equal(view.count(), 3);
    const HWND list = find_control(view.window, WorkbenchWindow::collection_files);
    check_equal(SendMessageW(list, LB_GETCOUNT, 0, 0), LRESULT{2});
    view.action(WorkbenchWindow::manage_collections);
    check_equal(view.count(), 3);
    view.window.MoveWindow(0, 0, 1280, 780, TRUE);
    view.window.ShowWindow(SW_SHOWNOACTIVATE);
    view.window.UpdateWindow();
    render_workspace(view.window, "workbench-collections-manager.bmp");

    SendMessageW(list, LB_SETCURSEL, 1, 0);
    view.action(WorkbenchWindow::unload_collection_file);
    check_equal(SendMessageW(list, LB_GETCOUNT, 0, 0), LRESULT{1});
    check_true(std::filesystem::exists(second));
    check_true(IsWindow(pane));
    request.tool->endpoint = "tcp://127.0.0.1:9003";
    check_true(flowie::app::CollectionStore(first).save(request, true).has_value());
    view.action(WorkbenchWindow::reload_collection_file);
    check_equal(SendMessageW(list, LB_GETCOUNT, 0, 0), LRESULT{1});
    check_true(view.text(pane, ProtocolWindow::target) == L"tcp://127.0.0.1:9001");
    files.write(L"lab.json", "invalid collection");
    check_throws_as(view.window.load_collections(first), std::runtime_error);
    check_equal(SendMessageW(list, LB_GETCOUNT, 0, 0), LRESULT{1});
    const auto loaded = TreeView_GetChild(tree, TreeView_GetRoot(tree));
    view.open_request(tree, TreeView_GetChild(tree, loaded));
    check_true(view.text(view.active(), ProtocolWindow::target) == L"tcp://127.0.0.1:9003");
    SendMessageW(view.close_button(2), BM_CLICK, 0, 0);
    check_true(pump_until([&] { return view.count() == 3; }));
    check_true(IsWindow(pane));
  }

  it("opens a previous MQTT endpoint from the sidebar History without connecting") {
    TestHistoryKey key;
    RecentConnections(key.path).remember({flowie::app::ClientRole::subscriber, L"localhost", 1887,
                                          FLOWIE_MQTT_CLIENT_TRANSPORT_TCP, FLOWIE_MQTT_VERSION_5});
    WorkspaceFixture view(false, key.path);
    view.sidebar(1);
    const HWND list = find_control(view.window, WorkbenchWindow::sidebar_history);
    check_true((GetWindowLongPtrW(list, GWL_STYLE) & WS_VISIBLE) != 0);
    check_equal(SendMessageW(list, LB_GETCOUNT, 0, 0), LRESULT{1});
    SendMessageW(list, LB_SETCURSEL, 0, 0);
    DialogAction open{view.window, IDC_OPEN_CONNECTION_TAB, {}};
    open.run(WorkbenchWindow::sidebar_history);
    check_true(open.seen);
    check_equal(view.count(), 1);
    check_true(view.text(view.active(), MainWindow::port) == L"1887");
    check_false(view.ready(view.active()));
  }

  it("keeps distinct collections and only replaces a request with explicit consent") {
    ImportFiles files;
    const auto library = files.reserve(L"collections.json");
    const flowie::app::CollectionStore store(library);
    flowie::app::SavedRequest request;
    request.collection = "Local";
    request.name = "Ping";
    request.profile.connection.port = 1883;
    check_true(store.save(request, false).has_value());
    request.profile.connection.port = 1884;
    check_false(store.save(request, false).has_value());
    const auto before = store.load();
    check_equal(before[0].profile.connection.port, 1883);
    check_true(store.save(request, true).has_value());
    request.collection = "Staging";
    request.profile.connection.port = 1885;
    check_true(store.save(request, false).has_value());
    request.profile.connection.port = 0;
    check_throws_as(store.save(request, true), std::runtime_error);
    const auto after = flowie::app::CollectionStore(library).load();
    check_equal(after.size(), size_t{2});
    check_equal(after[0].profile.connection.port, 1884);
    check_equal(after[1].profile.connection.port, 1885);
  }
}

suite("Workbench file import and task views") {
  it("loads a connection profile into the confirmation dialog and waits for the chosen action") {
    ImportFiles files;
    const auto profile = files.write(
        L"broker.json",
        R"({"host":"localhost","port":8884,"transport":"wss","protocol":"3.1.1","role":"publisher","path":"/test","ca_file":"roots.pem"})");
    WorkspaceFixture view(false);
    const HMENU menu = GetMenu(view.window);
    check_equal(GetMenuItemCount(menu), 4);
    const HMENU mqtt = GetSubMenu(GetSubMenu(GetSubMenu(menu, 0), 0), 0);
    check_true(GetMenuState(mqtt, WorkbenchWindow::import_connection_file, MF_BYCOMMAND) !=
               UINT(-1));
    check_equal(GetMenuItemID(GetSubMenu(menu, 2), 0),
                static_cast<UINT>(WorkbenchWindow::import_sample_file));
    std::wstring ca_file;
    bool fields_match = false;
    DialogAction open{
        view.window, IDC_OPEN_CONNECTION_TAB, [&](HWND dialog) {
          ca_file = view.text(dialog, IDC_BROKER_CA);
          fields_match =
              view.text(dialog, IDC_BROKER_HOST) == L"localhost" &&
              view.text(dialog, IDC_BROKER_PORT) == L"8884" &&
              view.text(dialog, IDC_BROKER_PATH) == L"/test" &&
              view.text(dialog, IDC_BROKER_PASSWORD).empty() &&
              SendDlgItemMessageW(dialog, IDC_BROKER_TRANSPORT, CB_GETCURSEL, 0, 0) == 3 &&
              SendDlgItemMessageW(dialog, IDC_BROKER_PROTOCOL, CB_GETCURSEL, 0, 0) == 1;
        }};
    open.run([&] { view.window.import_connection(profile); });
    check_true(open.seen);
    check_true(fields_match);
    check_true(std::filesystem::path(ca_file) == files.dir / L"roots.pem");
    check_equal(view.count(), 1);
    check_true(view.text(view.active(), MainWindow::path) == L"/test");
    check_false(view.ready(view.active()));
    check_true(RecentConnections(view.history_key.path).load().empty());
    const auto invalid = files.write(L"invalid.json", R"({"host":"localhost","port":65536})");
    check_throws_as(view.window.import_connection(invalid), std::runtime_error);
    check_equal(view.count(), 1);
  }

  it("loads text, raw JSON and full sample drafts without sending until Publish") {
    ImportFiles files;
    const auto text = files.write(L"payload.txt", "first line\r\nsecond line\n");
    const auto json = files.write(L"payload.json", "{ \"message\": \"hello\" }\n");
    const auto invalid = files.write(L"invalid.json", "{bad json");
    const auto sample = files.write(
        L"sensor.mqtt-sample.json",
        R"({"topic":"workbench/sensor","qos":1,"retain":true,"format":"json","payload":"{\"level\":3}"})");
    RoutingBroker broker;
    WorkspaceFixture view;
    const HWND pane = view.active();
    view.connect(pane, broker.port);
    check_true(pump_until([&] { return view.ready(pane); }));
    view.click(pane, MainWindow::subscribe_button);
    check_true(pump_until([&] { return view.ready(pane); }));
    const auto selection = [&](int id) {
      return SendMessageW(find_control(pane, id), CB_GETCURSEL, 0, 0);
    };
    const auto original_topic = view.text(pane, MainWindow::topic);
    view.window.import_sample(text);
    check_true(view.text(pane, MainWindow::payload) == L"first line\r\nsecond line\n");
    check_equal(selection(MainWindow::mode), LRESULT{0});
    view.window.import_sample(json);
    check_true(view.text(pane, MainWindow::payload) == L"{ \"message\": \"hello\" }\n");
    check_true(view.text(pane, MainWindow::topic) == original_topic);
    check_equal(selection(MainWindow::mode), LRESULT{1});
    check_throws_as(view.window.import_sample(invalid), std::runtime_error);
    check_true(view.text(pane, MainWindow::payload) == L"{ \"message\": \"hello\" }\n");
    view.action(WorkbenchWindow::view_monitor);
    view.window.import_sample(sample);
    check_true(view.text(pane, MainWindow::topic) == L"workbench/sensor");
    check_true(view.text(pane, MainWindow::payload) == L"{\"level\":3}");
    check_equal(selection(MainWindow::pub_qos), LRESULT{1});
    check_equal(SendMessageW(find_control(pane, MainWindow::retain), BM_GETCHECK, 0, 0),
                LRESULT{BST_CHECKED});
    check_true(view.counters(pane, 0, 0));
    view.action(WorkbenchWindow::publish_active);
    check_true(pump_until([&] { return view.counters(pane, 1, 1) && view.ready(pane); }));
  }

  it("uses independent WTL pane modes while preserving the connected tab and its draft") {
    RoutingBroker broker;
    WorkspaceFixture view;
    const HWND first = view.active();
    view.connect(first, broker.port);
    check_true(pump_until([&] { return view.ready(first); }));
    view.set(first, MainWindow::payload, L"Publish draft");
    const HWND composer = GetParent(find_control(first, MainWindow::payload));
    const HWND results = find_control(first, MainWindow::results_splitter);
    view.action(WorkbenchWindow::view_publish);
    check_true(visible_in_request(composer, first));
    check_false(visible_in_request(results, first));
    view.action(WorkbenchWindow::add_subscriber);
    const HWND second = view.active();
    const HWND second_composer = GetParent(find_control(second, MainWindow::payload));
    view.action(WorkbenchWindow::view_monitor);
    check_false(visible_in_request(second_composer, second));
    view.action(WorkbenchWindow::view_publish);
    check_false(visible_in_request(second_composer, second));
    check_true(visible_in_request(find_control(second, MainWindow::filter), second));
    check_false(visible_in_request(find_control(second, MainWindow::results_splitter), second));
    view.select(0);
    check_false(visible_in_request(results, first));
    view.action(WorkbenchWindow::view_monitor);
    check_false(visible_in_request(composer, first));
    check_true(visible_in_request(results, first));
    view.action(WorkbenchWindow::view_workspace);
    check_true(visible_in_request(composer, first));
    check_true(visible_in_request(results, first));
    check_true(view.ready(first));
    check_true(view.text(first, MainWindow::payload) == L"Publish draft");
    check_equal(view.count(), 2);
  }
}

suite("Workbench multiple publishers and subscribers") {
  it("closes an inactive tab by its own button and preserves the selected client's connection") {
    RoutingBroker broker;
    WorkspaceFixture view;
    HWND first = view.active();
    view.connect(first, broker.port);
    check_true(pump_until([&] { return view.ready(first); }));
    view.click(first, MainWindow::subscribe_button);
    check_true(pump_until([&] { return view.ready(first); }));
    view.action(WorkbenchWindow::add_publisher);
    HWND second = view.active();
    view.connect(second, broker.port);
    check_true(pump_until([&] { return view.ready(second); }));
    view.action(WorkbenchWindow::add_subscriber);
    HWND third = view.active();
    HWND close_second = view.close_button(1), close_third = view.close_button(2);
    check_true(view.text(view.window, WorkbenchWindow::tab_close).find(L"Close ") == 0);
    view.select(0);
    const auto quits = quit_messages;
    SendMessageW(close_second, BM_CLICK, 0, 0);
    check_false(IsWindowEnabled(close_second));
    check_true(view.active() == first);
    check_true(pump_until([&] { return view.count() == 2; }));
    check_false(IsWindow(second));
    check_false(IsWindow(close_second));
    check_true(view.active() == first);
    check_true(view.close_button(1) == close_third);
    view.click(first, MainWindow::publish_button);
    check_true(pump_until([&] { return view.counters(first, 1, 1) && view.ready(first); }));
    SendMessageW(close_third, BM_CLICK, 0, 0);
    check_true(pump_until([&] { return view.count() == 1; }));
    check_false(IsWindow(third));
    SendMessageW(view.close_button(0), BM_CLICK, 0, 0);
    check_true(pump_until([&] { return view.count() == 0; }));
    check_true(IsWindow(view.window));
    check_equal(quit_messages, quits);
    view.action(WorkbenchWindow::add_client);
    check_equal(view.count(), 1);
    check_equal(broker.failure.load(), 0);
  }
  it("keeps close buttons within visible tab headers when tabs overflow and the window resizes") {
    WorkspaceFixture view;
    const HDC dc = GetDC(view.window);
    check_not_null(dc);
    const int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(view.window, dc);
    view.window.MoveWindow(0, 0, MulDiv(900, dpi, 96), MulDiv(760, dpi, 96), TRUE);
    for (size_t i = 1; i < flowie::app::kMaxClients; ++i)
      view.action(WorkbenchWindow::add_subscriber);
    auto check_visible_buttons = [&] {
      HWND tabs = find_control(view.window, WorkbenchWindow::client_tabs);
      RECT viewport{};
      GetClientRect(tabs, &viewport);
      HWND arrows = FindWindowExW(tabs, nullptr, UPDOWN_CLASSW, nullptr);
      if (arrows && (GetWindowLongPtrW(arrows, GWL_STYLE) & WS_VISIBLE)) {
        RECT bounds{};
        GetWindowRect(arrows, &bounds);
        MapWindowPoints(nullptr, tabs, reinterpret_cast<POINT *>(&bounds), 2);
        viewport.right = (std::min)(viewport.right, bounds.left);
      }
      int visible = 0;
      for (int index = 0; index < view.count(); ++index) {
        HWND button = view.close_button(index);
        if (!(GetWindowLongPtrW(button, GWL_STYLE) & WS_VISIBLE)) continue;
        ++visible;
        RECT close{}, tab{}, intersection{};
        GetWindowRect(button, &close);
        MapWindowPoints(nullptr, tabs, reinterpret_cast<POINT *>(&close), 2);
        TabCtrl_GetItemRect(tabs, index, &tab);
        IntersectRect(&intersection, &close, &viewport);
        check_true(EqualRect(&intersection, &close) != FALSE);
        IntersectRect(&intersection, &close, &tab);
        check_true(EqualRect(&intersection, &close) != FALSE);
      }
      check_greater(visible, 0);
    };
    check_visible_buttons();
    check_true((GetWindowLongPtrW(view.close_button(7), GWL_STYLE) & WS_VISIBLE) != 0);
    view.select(0);
    check_visible_buttons();
    check_true((GetWindowLongPtrW(view.close_button(0), GWL_STYLE) & WS_VISIBLE) != 0);
    view.window.MoveWindow(0, 0, 1200, 760, TRUE);
    check_visible_buttons();
  }
  it("disables repeated tab closes during CONNECT drain and supports Ctrl+W on the remaining "
     "client") {
    RoutingBroker broker;
    LoopbackBroker silent(false, true);
    WorkspaceFixture view;
    HWND first = view.active();
    view.connect(first, broker.port);
    check_true(pump_until([&] { return view.ready(first); }));
    view.action(WorkbenchWindow::add_subscriber);
    HWND waiting = view.active();
    view.connect(waiting, silent.port);
    HWND close = view.close_button(1);
    SendMessageW(close, BM_CLICK, 0, 0);
    check_false(IsWindowEnabled(close));
    check_true(IsWindow(waiting));
    SendMessageW(close, BM_CLICK, 0, 0);
    check_true(pump_until([&] { return view.count() == 1; }));
    check_true(view.active() == first);
    check_true(view.ready(first));
    bool handled = false;
    {
      struct ControlKey {
        BYTE saved[256]{};
        ControlKey() {
          if (!GetKeyboardState(saved))
            throw std::runtime_error("Could not read test keyboard state");
          BYTE state[256];
          std::copy(saved, saved + 256, state);
          state[VK_CONTROL] = 0x80;
          state[VK_LCONTROL] = 0x80;
          if (!SetKeyboardState(state)) throw std::runtime_error("Could not set test Control key");
        }
        ~ControlKey() { SetKeyboardState(saved); }
      } control;
      MSG message{};
      message.hwnd = view.window;
      message.message = WM_KEYDOWN;
      message.wParam = 'W';
      handled = view.window.translate(&message);
    }
    check_true(handled);
    check_true(pump_until([&] { return view.count() == 0; }));
    check_true(IsWindow(view.window));
  }

  it("renders the Studio workspace while keeping the connected client and message detail intact") {
    RoutingBroker broker;
    WorkspaceFixture view;
    view.window.MoveWindow(0, 0, 1440, 1000, TRUE);
    HWND pane = view.active();
    view.connect(pane, broker.port);
    check_true(pump_until([&] { return view.ready(pane); }));
    view.click(pane, MainWindow::subscribe_button);
    check_true(pump_until([&] { return view.ready(pane); }));
    view.set(pane, MainWindow::topic, L"workbench/sensor-17");
    view.set(pane, MainWindow::payload,
             L"{\r\n  \"device\": \"sensor-17\",\r\n  \"temperature\": 23.6,\r\n  \"online\": "
             L"true\r\n}");
    SendMessageW(find_control(pane, MainWindow::mode), CB_SETCURSEL, 1, 0);
    SendMessageW(find_control(pane, MainWindow::pub_qos), CB_SETCURSEL, 1, 0);
    view.click(pane, MainWindow::publish_button);
    check_true(pump_until([&] { return view.counters(pane, 1, 1) && view.ready(pane); }));
    view.action(WorkbenchWindow::add_publisher);
    view.action(WorkbenchWindow::add_subscriber);
    view.select(0);
    HWND list = find_control(pane, MainWindow::records);
    for (int row = 0; row < ListView_GetItemCount(list); ++row) {
      wchar_t event[32]{};
      ListView_GetItemText(list, row, 1, event, 32);
      if (std::wcscmp(event, L"RX") == 0)
        ListView_SetItemState(list, row, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
    }
    select_request_page(pane, L"Message");
    view.window.ShowWindow(SW_SHOWNOACTIVATE);
    view.window.UpdateWindow();
    SendMessageW(view.window, WM_THEMECHANGED, 0, 0);
    render_workspace(view.window, "workbench-studio.bmp");
    select_request_page(pane, L"Topics");
    render_workspace(view.window, "workbench-subscriptions.bmp");
    HWND command = find_control(view.window, WorkbenchWindow::primary_request);
    HWND previous_focus = SetFocus(command);
    SendMessageW(view.window, WM_CHANGEUISTATE,
                 MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS | UISF_HIDEACCEL), 0);
    SendMessageW(command, BM_SETSTATE, TRUE, 0);
    render_workspace(view.window, "workbench-command-focus.bmp");
    SendMessageW(command, BM_SETSTATE, FALSE, 0);
    if (IsWindow(previous_focus)) SetFocus(previous_focus);
    check_true(view.counters(pane, 1, 1));
    check_true(view.text(pane, MainWindow::details).find(L"sensor-17") != std::wstring::npos);
    select_request_page(pane, L"Message");
    view.action(WorkbenchWindow::view_publish);
    render_workspace(view.window, "workbench-publish-layout.bmp");
    view.action(WorkbenchWindow::view_monitor);
    render_workspace(view.window, "workbench-monitor-layout.bmp");
  }
  it("routes two publishers to two background subscribers and keeps other clients alive after "
     "closing one") {
    RoutingBroker broker;
    WorkspaceFixture view;
    view.action(WorkbenchWindow::close_client);
    check_true(pump_until([&] { return view.count() == 0; }));
    std::array<HWND, 4> panes{};
    std::set<std::wstring> identifiers;
    for (size_t i = 0; i < panes.size(); ++i) {
      view.action(i < 2 ? WorkbenchWindow::add_subscriber : WorkbenchWindow::add_publisher);
      panes[i] = view.active();
      identifiers.insert(view.text(panes[i], MainWindow::client_id));
      view.connect(panes[i], broker.port);
    }
    check_equal(identifiers.size(), size_t(4));
    check_true(pump_until(
        [&] { return std::all_of(panes.begin(), panes.end(), WorkspaceFixture::ready); }));
    for (size_t i = 0; i < 2; ++i) {
      view.select(static_cast<int>(i));
      check_false(IsWindowEnabled(find_control(panes[i], MainWindow::publish_button)));
      view.set(panes[i], MainWindow::filter, L"multi/+");
      view.click(panes[i], MainWindow::subscribe_button);
    }
    check_true(pump_until([&] {
      return SendMessageW(find_control(panes[0], MainWindow::subscriptions), LB_GETCOUNT, 0, 0) ==
                 1 &&
             SendMessageW(find_control(panes[1], MainWindow::subscriptions), LB_GETCOUNT, 0, 0) ==
                 1;
    }));
    for (size_t i = 2; i < 4; ++i) {
      view.select(static_cast<int>(i));
      check_false(IsWindowEnabled(find_control(panes[i], MainWindow::subscribe_button)));
      view.set(panes[i], MainWindow::topic, i == 2 ? L"multi/json" : L"multi/txt");
      view.set(panes[i], MainWindow::payload,
               i == 2 ? L"{\"source\":1,\"text\":\"你好\"}" : L"publisher two");
      SendMessageW(find_control(panes[i], MainWindow::mode), CB_SETCURSEL, i == 2 ? 1 : 0, 0);
      SendMessageW(find_control(panes[i], MainWindow::pub_qos), CB_SETCURSEL, i == 2 ? 1 : 0, 0);
      view.click(panes[i], MainWindow::publish_button);
    }
    check_true(pump_until([&] {
      return view.counters(panes[0], 2, 0) && view.counters(panes[1], 2, 0) &&
             view.ready(panes[2]) && view.ready(panes[3]);
    }));
    SendMessageW(view.window, WM_SYSCOLORCHANGE, 0, 0);
    check_true(view.counters(panes[0], 2, 0));
    check_true(view.counters(panes[1], 2, 0));
    check_true(view.counters(panes[2], 0, 1));
    check_true(view.counters(panes[3], 0, 1));
    view.select(2);
    check_true(view.active() == panes[2]);
    check_true(view.text(view.active(), MainWindow::payload).find(L"你好") != std::wstring::npos);
    view.select(0);
    view.click(panes[0], MainWindow::publish_button); // dispatch must enforce the role too
    check_true(view.counters(panes[0], 2, 0));
    view.select(2);
    view.click(panes[2], MainWindow::subscribe_button);
    check_equal(SendMessageW(find_control(panes[2], MainWindow::subscriptions), LB_GETCOUNT, 0, 0),
                LRESULT(0));
    const auto quit_before_close = quit_messages;
    view.action(WorkbenchWindow::close_client);
    check_true(pump_until([&] { return view.count() == 3; }));
    check_false(IsWindow(panes[2]));
    check_equal(quit_messages, quit_before_close);
    view.select(2);
    check_true(view.active() == panes[3]);
    check_true(view.text(panes[3], MainWindow::payload) == L"publisher two");
    view.click(panes[3], MainWindow::publish_button);
    check_true(
        pump_until([&] { return view.counters(panes[0], 3, 0) && view.counters(panes[1], 3, 0); }));
    view.select(0);
    view.action(WorkbenchWindow::close_client);
    check_true(pump_until([&] { return view.count() == 2 && view.ready(panes[3]); }));
    view.select(1);
    view.click(panes[3], MainWindow::publish_button);
    check_true(pump_until([&] { return view.counters(panes[1], 4, 0); }));
    check_equal(broker.failure.load(), 0);
  }
  it("isolates identical topics on different brokers and matches filters within a broker") {
    RoutingBroker first, second;
    WorkspaceFixture view;
    HWND first_client = view.active();
    view.connect(first_client, first.port);
    view.action(WorkbenchWindow::add_client);
    HWND second_client = view.active();
    SendMessageW(find_control(second_client, MainWindow::protocol), CB_SETCURSEL, 1, 0);
    view.connect(second_client, second.port);
    check_true(pump_until([&] { return view.ready(first_client) && view.ready(second_client); }));
    for (HWND pane : {first_client, second_client}) {
      view.set(pane, MainWindow::filter, L"broker/selected");
      view.click(pane, MainWindow::subscribe_button);
    }
    check_true(pump_until([&] { return view.ready(first_client) && view.ready(second_client); }));
    view.set(first_client, MainWindow::topic, L"broker/selected");
    view.set(first_client, MainWindow::payload, L"only first broker");
    view.click(first_client, MainWindow::publish_button);
    view.set(second_client, MainWindow::topic, L"broker/selected");
    view.set(second_client, MainWindow::payload, L"only second broker");
    view.click(second_client, MainWindow::publish_button);
    check_true(pump_until([&] {
      return view.counters(first_client, 1, 1) && view.counters(second_client, 1, 1) &&
             view.ready(first_client) && view.ready(second_client);
    }));
    view.set(first_client, MainWindow::topic, L"broker/unmatched");
    view.click(first_client, MainWindow::publish_button);
    check_true(
        pump_until([&] { return view.ready(first_client) && view.counters(first_client, 1, 2); }));
    view.click(second_client, MainWindow::publish_button);
    check_true(pump_until([&] { return view.counters(second_client, 2, 2); }));
    check_true(view.counters(first_client, 1, 2));
    view.select(0);
    check_true(view.active() == first_client);
    check_true(view.text(first_client, MainWindow::port) == std::to_wstring(first.port));
    check_true(view.text(second_client, MainWindow::port) == std::to_wstring(second.port));
    check_equal(first.failure.load(), 0);
    check_equal(second.failure.load(), 0);
  }
  it("bounds the workspace to eight clients, removes the final tab and creates fresh client IDs") {
    WorkspaceFixture view;
    std::set<std::wstring> identifiers{view.text(view.active(), MainWindow::client_id)};
    for (size_t i = 1; i < flowie::app::kMaxClients; ++i) {
      view.action(i % 2 ? WorkbenchWindow::add_publisher : WorkbenchWindow::add_subscriber);
      identifiers.insert(view.text(view.active(), MainWindow::client_id));
    }
    check_equal(view.count(), 8);
    check_equal(identifiers.size(), size_t(8));
    check_false(IsWindowEnabled(find_control(view.window, WorkbenchWindow::add_client)));
    view.action(WorkbenchWindow::add_client);
    check_equal(view.count(), 8);
    for (int count = 8; count > 0; --count) {
      view.action(WorkbenchWindow::close_client);
      check_true(pump_until([&] { return view.count() == count - 1; }));
    }
    check_true(IsWindow(view.window));
    view.action(WorkbenchWindow::add_subscriber);
    check_equal(view.count(), 1);
    check_false(identifiers.count(view.text(view.active(), MainWindow::client_id)) != 0);
  }
  it("keeps connection actions visible when a short request page scrolls") {
    WorkspaceFixture view;
    view.window.MoveWindow(0, 0, 1200, 550, TRUE);
    HWND pane = view.active();
    select_request_page(pane, L"Settings");
    const HWND transport = find_control(pane, MainWindow::transport);
    SendMessageW(transport, CB_SETCURSEL, 3, 0);
    SendMessageW(pane, WM_COMMAND, MAKEWPARAM(MainWindow::transport, CBN_SELCHANGE),
                 reinterpret_cast<LPARAM>(transport));
    const HWND key = find_control(pane, MainWindow::key);
    const HWND body = GetParent(key);
    SCROLLINFO scroll{sizeof(scroll), SIF_ALL};
    check_true(GetScrollInfo(body, SB_VERT, &scroll) != FALSE);
    check_greater(scroll.nMax, static_cast<int>(scroll.nPage));
    SendMessageW(body, WM_VSCROLL, SB_BOTTOM, 0);
    RECT key_bounds{}, area{}, footer{}, connect{};
    GetWindowRect(key, &key_bounds);
    GetClientRect(body, &area);
    POINT body_bottom{0, area.bottom};
    ClientToScreen(body, &body_bottom);
    check_less_equal(key_bounds.bottom, body_bottom.y);
    GetWindowRect(find_control(pane, MainWindow::status), &footer);
    GetClientRect(pane, &area);
    POINT bottom{0, area.bottom};
    ClientToScreen(pane, &bottom);
    check_less_equal(footer.bottom, bottom.y);
    const HWND host = find_control(pane, MainWindow::host);
    const HWND action = find_control(pane, MainWindow::connect_button);
    GetWindowRect(action, &connect);
    check_true(visible_in_request(host, pane));
    check_true(visible_in_request(action, pane));
    check_less(connect.bottom, key_bounds.top);
    select_request_page(pane, L"Topics");
    check_true(visible_in_request(find_control(pane, MainWindow::subscriptions), pane));
    check_false(visible_in_request(key, pane));
    check_true(visible_in_request(host, pane));
    select_request_page(pane, L"Authorization");
    check_true(visible_in_request(find_control(pane, MainWindow::password), pane));
    check_true(visible_in_request(host, pane));
  }
  it("drains all child contexts when one connection is established and another waits for CONNACK") {
    LoopbackBroker normal, silent(false, true);
    WorkspaceFixture view;
    HWND first = view.active();
    view.connect(first, normal.port);
    check_true(pump_until([&] { return view.ready(first); }));
    view.action(WorkbenchWindow::add_subscriber);
    HWND waiting = view.active();
    view.connect(waiting, silent.port);
    SendMessageW(view.window, WM_CLOSE, 0, 0);
    check_true(pump_until([&] { return !IsWindow(view.window); }));
    check_false(IsWindow(first));
    check_false(IsWindow(waiting));
  }
}
