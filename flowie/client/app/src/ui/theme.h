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
    COLORREF chrome, chrome_text, chrome_muted, chrome_hover, chrome_pressed, chrome_focus;
    COLORREF chrome_green, chrome_amber;

    COLORREF tab_background(bool selected) const { return selected ? paper : surface; }
  };
  UiTheme current_theme();
  HBRUSH color_brush(HDC dc, COLORREF foreground, COLORREF background);
  constexpr int kTabCloseSize = 24;
  constexpr int kTabCloseInset = 3;
  RECT tab_viewport(HWND tabs);
  void style_tabs(HWND tabs, const UiTheme &theme);
  enum class CommandGlyph { request, open, save, run, stop, actions };
  LRESULT draw_command(const NMCUSTOMDRAW &draw, const UiTheme &theme, CommandGlyph glyph, int dpi);
  LRESULT draw_form_button(const NMCUSTOMDRAW &draw, const UiTheme &theme, int dpi,
                           bool primary = false);
  LRESULT draw_tab_close(const DRAWITEMSTRUCT &draw, const UiTheme &theme, bool selected, bool hot,
                         int dpi);
} // namespace flowie::app
