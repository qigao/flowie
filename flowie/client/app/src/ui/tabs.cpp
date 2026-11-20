#include "theme.h"
#include <algorithm>
#include <cwchar>
#include <stdexcept>

namespace flowie::app {
  RECT tab_viewport(HWND tabs) {
    RECT viewport{};
    GetClientRect(tabs, &viewport);
    HWND arrows = FindWindowExW(tabs, nullptr, UPDOWN_CLASSW, nullptr);
    if (arrows && (GetWindowLongPtrW(arrows, GWL_STYLE) & WS_VISIBLE)) {
      RECT bounds{};
      GetWindowRect(arrows, &bounds);
      MapWindowPoints(nullptr, tabs, reinterpret_cast<POINT *>(&bounds), 2);
      viewport.right = std::min(viewport.right, bounds.left);
    }
    return viewport;
  }

  namespace {
    void draw_tabs(HWND tabs, HDC dc, const UiTheme &theme) {
      const int saved = SaveDC(dc);
      RECT strip{};
      GetClientRect(tabs, &strip);
      FillRect(dc, &strip, color_brush(dc, theme.ink, theme.surface));
      RECT edge = strip;
      edge.top = edge.bottom - 1;
      FillRect(dc, &edge, color_brush(dc, theme.ink, theme.divider));
      const auto viewport = tab_viewport(tabs);
      IntersectClipRect(dc, viewport.left, viewport.top, viewport.right, viewport.bottom);
      SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(tabs, WM_GETFONT, 0, 0)));
      const int dpi = GetDeviceCaps(dc, LOGPIXELSX);
      const int padding = MulDiv(7, dpi, 96);
      const int selected = TabCtrl_GetCurSel(tabs);
      const int focused =
          GetFocus() == tabs && !(SendMessageW(tabs, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)
              ? TabCtrl_GetCurFocus(tabs)
              : -1;
      const int count = TabCtrl_GetItemCount(tabs);
      for (int index = 0; index < count; ++index) {
        RECT bounds{}, visible{};
        if (!TabCtrl_GetItemRect(tabs, index, &bounds) ||
            !IntersectRect(&visible, &bounds, &viewport))
          continue;
        wchar_t label[128]{};
        TCITEMW item{};
        item.mask = TCIF_TEXT | TCIF_PARAM;
        item.pszText = label;
        item.cchTextMax = 128;
        if (!TabCtrl_GetItem(tabs, index, &item)) continue;
        const bool active = index == selected;
        const auto ink = theme.system && !active ? theme.chrome_text : theme.ink;
        const auto fill = theme.tab_background(active);
        RECT face = bounds;
        face.bottom = edge.top;
        FillRect(dc, &face, color_brush(dc, ink, fill));
        RECT separator = face;
        separator.left = separator.right - 1;
        FillRect(dc, &separator, color_brush(dc, ink, theme.divider));
        if (active) {
          RECT indicator = face;
          indicator.top = indicator.bottom - MulDiv(2, dpi, 96);
          --indicator.right;
          FillRect(dc, &indicator, color_brush(dc, ink, theme.focus));
        }
        RECT text = bounds;
        text.left += padding;
        text.right -= item.lParam ? MulDiv(kTabCloseSize + kTabCloseInset, dpi, 96) : padding;
        auto length = std::wcslen(label);
        while (length && label[length - 1] == L' ')
          --length;
        SetTextColor(dc, ink);
        SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, label, static_cast<int>(length), &text,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        if (index == focused) {
          InflateRect(&text, -1, -3);
          DrawFocusRect(dc, &text);
        }
      }
      RestoreDC(dc, saved);
    }

    // Paint only. The common control still owns selection, scrolling and keyboard input.
    LRESULT CALLBACK tab_paint(HWND tabs, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id,
                               DWORD_PTR context) {
      const auto &theme = *reinterpret_cast<const UiTheme *>(context);
      switch (message) {
      case WM_ERASEBKGND:
        return 1;
      case WM_PAINT: {
        if (wparam) {
          draw_tabs(tabs, reinterpret_cast<HDC>(wparam), theme);
          return 0;
        }
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(tabs, &paint);
        draw_tabs(tabs, dc, theme);
        EndPaint(tabs, &paint);
        return 0;
      }
      case WM_PRINTCLIENT:
        draw_tabs(tabs, reinterpret_cast<HDC>(wparam), theme);
        return 0;
      case WM_SETFOCUS:
      case WM_KILLFOCUS:
      case WM_UPDATEUISTATE:
      case WM_HSCROLL:
      case TCM_SETCURSEL:
      case TCM_SETCURFOCUS: {
        const LRESULT result = DefSubclassProc(tabs, message, wparam, lparam);
        InvalidateRect(tabs, nullptr, FALSE);
        return result;
      }
      case WM_NCDESTROY:
        RemoveWindowSubclass(tabs, tab_paint, id);
        break;
      }
      return DefSubclassProc(tabs, message, wparam, lparam);
    }
  } // namespace

  void style_tabs(HWND tabs, const UiTheme &theme) {
    if (!SetWindowSubclass(tabs, tab_paint, 1, reinterpret_cast<DWORD_PTR>(&theme)))
      throw std::runtime_error("Could not attach tab theme");
  }
} // namespace flowie::app
