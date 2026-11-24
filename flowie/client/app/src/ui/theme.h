#pragma once
// clang-format off
// Common Controls requires the Win32 type declarations first.
#include <windows.h>
#include <commctrl.h>
// clang-format on

namespace flowie::app {
  struct UiTheme {
    bool system = false;
    COLORREF paper, surface, divider, ink, muted, disabled, blue, green, red;
    COLORREF control_border, hover, pressed, focus;
    COLORREF chrome_text;

    COLORREF tab_background(bool selected) const { return selected ? paper : surface; }
  };
  UiTheme current_theme();
  HBRUSH color_brush(HDC dc, COLORREF foreground, COLORREF background);
  constexpr int kTabCloseSize = 24;
  constexpr int kTabCloseInset = 3;
  RECT tab_viewport(HWND tabs);
  void style_tabs(HWND tabs, const UiTheme &theme);
  LRESULT draw_form_button(const NMCUSTOMDRAW &draw, const UiTheme &theme, int dpi,
                           bool primary = false);
  LRESULT draw_tab_close(const DRAWITEMSTRUCT &draw, const UiTheme &theme, bool selected, bool hot,
                         int dpi);
} // namespace flowie::app
