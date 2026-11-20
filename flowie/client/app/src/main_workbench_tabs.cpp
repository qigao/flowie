#include "main_workbench.h"

namespace flowie::app {
  bool WorkbenchWindow::can_close(const Client &client) const {
    return ::IsWindow(client.hwnd()) &&
           ((!closing_ && !client.closing()) || client.state() == State::close_failed);
  }
  void WorkbenchWindow::close_client_at(size_t index) {
    if (index >= clients_.size() || !can_close(*clients_[index])) return;
    ::SendMessageW(clients_[index]->hwnd(), WM_CLOSE, 0, 0);
    ::EnableWindow(clients_[index]->close_button, can_close(*clients_[index]));
  }
  std::wstring WorkbenchWindow::padded_tab_label(const std::wstring &text) const {
    return text + tab_padding_;
  }
  void WorkbenchWindow::position_close_buttons() {
    if (!tabs_) return;
    const RECT viewport = tab_viewport(tabs_);
    const int size = scale(kTabCloseSize), inset = scale(kTabCloseInset);
    for (size_t i = 0; i < clients_.size(); ++i) {
      HWND button = clients_[i]->close_button;
      RECT tab{};
      if (!button || !TabCtrl_GetItemRect(tabs_, static_cast<int>(i), &tab)) continue;
      RECT target{tab.right - inset - size, tab.top + (tab.bottom - tab.top - size) / 2,
                  tab.right - inset, tab.top + (tab.bottom - tab.top - size) / 2 + size};
      const bool visible = target.left >= viewport.left && target.right <= viewport.right &&
                           target.top >= viewport.top && target.bottom <= viewport.bottom;
      if (!visible) {
        ::ShowWindow(button, SW_HIDE);
        continue;
      }
      ::MapWindowPoints(tabs_, m_hWnd, reinterpret_cast<POINT *>(&target), 2);
      RECT current{};
      ::GetWindowRect(button, &current);
      ::MapWindowPoints(nullptr, m_hWnd, reinterpret_cast<POINT *>(&current), 2);
      if (!::EqualRect(&current, &target))
        ::SetWindowPos(button, HWND_TOP, target.left, target.top, target.right - target.left,
                       target.bottom - target.top, SWP_NOACTIVATE);
      if (!(::GetWindowLongPtrW(button, GWL_STYLE) & WS_VISIBLE)) ::ShowWindow(button, SW_SHOWNA);
    }
  }
  LRESULT CALLBACK WorkbenchWindow::TabProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                            UINT_PTR id, DWORD_PTR context) {
    auto &owner = *reinterpret_cast<WorkbenchWindow *>(context);
    const LRESULT result = ::DefSubclassProc(window, message, wparam, lparam);
    switch (message) {
    case WM_PAINT:
    case WM_SIZE:
    case WM_HSCROLL:
    case WM_WINDOWPOSCHANGED:
    case WM_NOTIFY:
    case WM_MOUSEWHEEL:
    case TCM_SETCURSEL:
    case TCM_SETITEMW:
    case TCM_INSERTITEMW:
    case TCM_DELETEITEM:
      owner.position_close_buttons();
      break;
    case WM_NCDESTROY:
      ::RemoveWindowSubclass(window, TabProc, id);
      break;
    }
    return result;
  }
  LRESULT WorkbenchWindow::OnDrawItem(UINT, WPARAM wparam, LPARAM lparam, BOOL &handled) {
    if (wparam != tab_close) {
      handled = FALSE;
      return 0;
    }
    const auto &draw = *reinterpret_cast<DRAWITEMSTRUCT *>(lparam);
    for (size_t i = 0; i < clients_.size(); ++i)
      if (clients_[i]->close_button == draw.hwndItem)
        return draw_tab_close(draw, theme_, i == active_, clients_[i]->close_hot, dpi_);
    return TRUE;
  }
} // namespace flowie::app
