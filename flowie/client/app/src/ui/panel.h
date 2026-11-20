#pragma once
#include <windows.h>
// clang-format off
#include <atlbase.h>
#include <atlapp.h>
extern CAppModule _Module;
#include <atlwin.h>
// clang-format on
#include "theme.h"

namespace flowie::app {
  constexpr UINT kPanelLayout = WM_APP + 40;
  class UiPanel final : public CWindowImpl<UiPanel> {
  public:
    DECLARE_WND_CLASS_EX(L"FlowieStudioPanel", 0, COLOR_WINDOW)
    BEGIN_MSG_MAP(UiPanel)
    MESSAGE_HANDLER(WM_ERASEBKGND, OnErase)
    MESSAGE_HANDLER(WM_SIZE, OnSize)
    MESSAGE_HANDLER(WM_COMMAND, Forward)
    MESSAGE_HANDLER(WM_NOTIFY, Forward)
    MESSAGE_HANDLER(WM_CTLCOLORSTATIC, Forward)
    MESSAGE_HANDLER(WM_CTLCOLOREDIT, Forward)
    MESSAGE_HANDLER(WM_CTLCOLORLISTBOX, Forward)
    MESSAGE_HANDLER(WM_CTLCOLORBTN, Forward)
    MESSAGE_HANDLER(WM_VSCROLL, OnScroll)
    MESSAGE_HANDLER(WM_MOUSEWHEEL, OnWheel)
    END_MSG_MAP()
    HWND owner = nullptr;
    COLORREF paper{};
    void extent(int height);
    int scroll_y() const { return scroll_y_; }
    void reset_scroll();

  private:
    void scroll(int next);
    LRESULT Forward(UINT message, WPARAM wparam, LPARAM lparam, BOOL &);
    LRESULT OnErase(UINT, WPARAM, LPARAM, BOOL &);
    LRESULT OnSize(UINT, WPARAM, LPARAM, BOOL &);
    LRESULT OnScroll(UINT, WPARAM, LPARAM, BOOL &);
    LRESULT OnWheel(UINT, WPARAM, LPARAM, BOOL &);
    int scroll_y_ = 0, wheel_ = 0;
    bool setting_extent_ = false;
  };
} // namespace flowie::app
