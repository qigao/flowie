#include "panel.h"

namespace flowie::app {
  void UiPanel::extent(int height) {
    RECT rect{};
    GetClientRect(&rect);
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
    info.nMax = height > 0 ? height - 1 : 0;
    info.nPage = static_cast<UINT>(rect.bottom);
    info.nPos = scroll_y_;
    setting_extent_ = true;
    ::SetScrollInfo(m_hWnd, SB_VERT, &info, TRUE);
    setting_extent_ = false;
    info.fMask = SIF_POS;
    ::GetScrollInfo(m_hWnd, SB_VERT, &info);
    scroll_y_ = info.nPos;
  }
  void UiPanel::reset_scroll() { scroll(0); }
  void UiPanel::scroll(int next) {
    SCROLLINFO info{sizeof(info), SIF_POS};
    info.nPos = next;
    ::SetScrollInfo(m_hWnd, SB_VERT, &info, TRUE);
    ::GetScrollInfo(m_hWnd, SB_VERT, &info);
    if (scroll_y_ != info.nPos) {
      scroll_y_ = info.nPos;
      ::SendMessageW(owner, kPanelLayout, 0, 0);
    }
  }
  LRESULT UiPanel::Forward(UINT message, WPARAM wparam, LPARAM lparam, BOOL &) {
    HWND focused = ::GetFocus();
    const bool focus_event =
        message == WM_COMMAND && (HIWORD(wparam) == EN_SETFOCUS || HIWORD(wparam) == CBN_SETFOCUS ||
                                  HIWORD(wparam) == LBN_SETFOCUS || HIWORD(wparam) == BN_SETFOCUS);
    if (focus_event && focused == reinterpret_cast<HWND>(lparam) &&
        ::GetParent(focused) == m_hWnd && (GetWindowLongPtr(GWL_STYLE) & WS_VSCROLL)) {
      RECT control{}, client{};
      ::GetWindowRect(focused, &control);
      GetClientRect(&client);
      POINT top{0, 0}, bottom{0, client.bottom};
      ::ClientToScreen(m_hWnd, &top);
      ::ClientToScreen(m_hWnd, &bottom);
      if (control.top < top.y) scroll(scroll_y_ + control.top - top.y);
      else if (control.bottom > bottom.y) scroll(scroll_y_ + control.bottom - bottom.y);
    }
    return ::SendMessageW(owner, message, wparam,
                          lparam); // Native notification data is borrowed synchronously.
  }
  LRESULT UiPanel::OnErase(UINT, WPARAM wparam, LPARAM, BOOL &) {
    RECT rect{};
    GetClientRect(&rect);
    ::FillRect(reinterpret_cast<HDC>(wparam), &rect,
               color_brush(reinterpret_cast<HDC>(wparam), 0, paper));
    return 1;
  }
  LRESULT UiPanel::OnSize(UINT, WPARAM, LPARAM, BOOL &) {
    if (!setting_extent_ && owner) ::SendMessageW(owner, kPanelLayout, 0, 0);
    return 0;
  }
  LRESULT UiPanel::OnScroll(UINT, WPARAM wparam, LPARAM lparam, BOOL &handled) {
    if (lparam) {
      handled = FALSE;
      return 0;
    }
    SCROLLINFO info{sizeof(info), SIF_ALL};
    ::GetScrollInfo(m_hWnd, SB_VERT, &info);
    int next = info.nPos;
    switch (LOWORD(wparam)) {
    case SB_TOP:
      next = 0;
      break;
    case SB_BOTTOM:
      next = info.nMax;
      break;
    case SB_LINEUP:
      next -= 32;
      break;
    case SB_LINEDOWN:
      next += 32;
      break;
    case SB_PAGEUP:
      next -= static_cast<int>(info.nPage);
      break;
    case SB_PAGEDOWN:
      next += static_cast<int>(info.nPage);
      break;
    case SB_THUMBTRACK:
      next = info.nTrackPos;
      break;
    }
    scroll(next);
    return 0;
  }
  LRESULT UiPanel::OnWheel(UINT, WPARAM wparam, LPARAM, BOOL &) {
    wheel_ += GET_WHEEL_DELTA_WPARAM(wparam);
    const int lines = wheel_ / WHEEL_DELTA;
    wheel_ %= WHEEL_DELTA;
    scroll(scroll_y_ - lines * 48);
    return 0;
  }
} // namespace flowie::app
