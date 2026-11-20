#pragma once
#include "recent_connections.h"
#include "theme.h"

namespace flowie::app {
  class ConnectionDialog {
  public:
    ConnectionDialog(ClientRole role, const std::vector<RecentConnection> &recent, int selected,
                     const UiTheme &theme, const Connection *initial = nullptr)
        : role_(role), recent_(recent), selected_(selected), theme_(theme), initial_(initial) {}
    ~ConnectionDialog();
    INT_PTR show(HWND owner, HINSTANCE instance);
    Connection &connection() { return connection_; }

  private:
    static INT_PTR CALLBACK dispatch(HWND, UINT, WPARAM, LPARAM);
    INT_PTR message(HWND, UINT, WPARAM, LPARAM);
    void populate(HWND);
    void select_recent(HWND);
    void enable_transport_fields(HWND);
    void submit(HWND, int action);

    ClientRole role_;
    const std::vector<RecentConnection> &recent_;
    int selected_;
    const UiTheme &theme_;
    const Connection *initial_;
    Connection connection_;
  };
} // namespace flowie::app
