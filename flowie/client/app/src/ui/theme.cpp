#include "theme.h"
#include <string>
#include <system_error>

namespace flowie::app {
  namespace {
    // Studio palette: paper is content, surface is navigation and section chrome.
    constexpr UiTheme studio = [] {
      UiTheme colors{};
      colors.paper = RGB(255, 255, 255);
      colors.surface = RGB(241, 241, 241);
      colors.divider = RGB(217, 220, 224);
      colors.ink = RGB(35, 45, 61);
      colors.muted = RGB(92, 106, 124);
      colors.disabled = RGB(112, 119, 128);
      colors.blue = RGB(34, 92, 174);
      colors.green = RGB(32, 117, 66);
      colors.red = RGB(174, 49, 59);
      colors.control_border = RGB(184, 191, 200);
      colors.hover = RGB(221, 234, 251);
      colors.pressed = RGB(192, 215, 246);
      colors.focus = RGB(34, 92, 174);
      colors.chrome = RGB(76, 76, 76);
      colors.chrome_text = RGB(250, 250, 250);
      colors.chrome_muted = RGB(190, 190, 190);
      colors.chrome_hover = RGB(92, 92, 92);
      colors.chrome_pressed = RGB(56, 56, 56);
      colors.chrome_focus = RGB(153, 218, 250);
      colors.chrome_green = RGB(155, 220, 151);
      colors.chrome_amber = RGB(255, 211, 139);
      return colors;
    }();

    void outline(HDC dc, RECT bounds, COLORREF color, int width) {
      const auto brush = color_brush(dc, color, color);
      for (int i = 0; i < width; ++i) {
        FrameRect(dc, &bounds, brush);
        InflateRect(&bounds, -1, -1);
      }
    }

    void draw_button_label(const NMCUSTOMDRAW &draw, RECT text, COLORREF ink, UINT align) {
      wchar_t label[96]{};
      const auto window = draw.hdr.hwndFrom;
      const int length = GetWindowTextW(window, label, 96);
      const bool hide = (SendMessageW(window, WM_QUERYUISTATE, 0, 0) & UISF_HIDEACCEL) != 0;
      SetTextColor(draw.hdc, ink);
      SetBkMode(draw.hdc, TRANSPARENT);
      SelectObject(draw.hdc, reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0)));
      DrawTextW(draw.hdc, label, length, &text,
                align | DT_SINGLELINE | DT_VCENTER | (hide ? DT_HIDEPREFIX : 0));
    }
  } // namespace

  UiTheme current_theme() {
    HIGHCONTRASTW contrast{sizeof(contrast)};
    if (!::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0))
      throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                              "Read Windows contrast settings");
    if (!(contrast.dwFlags & HCF_HIGHCONTRASTON)) return studio;
    const auto paper = GetSysColor(COLOR_WINDOW), ink = GetSysColor(COLOR_WINDOWTEXT);
    const auto face = GetSysColor(COLOR_BTNFACE), face_ink = GetSysColor(COLOR_BTNTEXT);
    const auto highlight = GetSysColor(COLOR_HIGHLIGHT), disabled = GetSysColor(COLOR_GRAYTEXT);
    UiTheme colors{};
    colors.system = true;
    colors.paper = paper;
    colors.surface = face;
    colors.divider = face_ink;
    colors.ink = ink;
    colors.muted = ink;
    colors.disabled = disabled;
    colors.blue = highlight;
    colors.green = ink;
    colors.red = ink;
    colors.control_border = face_ink;
    colors.hover = highlight;
    colors.pressed = highlight;
    colors.focus = highlight;
    colors.chrome = face;
    colors.chrome_text = face_ink;
    colors.chrome_muted = disabled;
    return colors;
  }
  HBRUSH color_brush(HDC dc, COLORREF foreground, COLORREF background) {
    SetTextColor(dc, foreground);
    SetBkColor(dc, background);
    SetDCBrushColor(dc, background);
    return static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
  }
  LRESULT draw_command(const NMCUSTOMDRAW &draw, const UiTheme &theme, CommandGlyph glyph,
                       int dpi) {
    if (theme.system) return CDRF_DODEFAULT;
    if (draw.dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
    const int saved = SaveDC(draw.hdc);
    const bool disabled = (draw.uItemState & CDIS_DISABLED) != 0;
    const bool pressed = (draw.uItemState & CDIS_SELECTED) != 0;
    const bool hot = (draw.uItemState & CDIS_HOT) != 0;
    const auto background = pressed ? theme.chrome_pressed
                            : hot   ? theme.chrome_hover
                                    : theme.chrome;
    const auto accent = disabled                      ? theme.chrome_muted
                        : glyph == CommandGlyph::run  ? theme.chrome_green
                        : glyph == CommandGlyph::stop ? theme.chrome_amber
                                                      : theme.chrome_text;
    FillRect(draw.hdc, &draw.rc, color_brush(draw.hdc, theme.chrome_text, background));
    auto s = [=](int value) { return MulDiv(value, dpi, 96); };
    const int x = draw.rc.left + s(10),
              y = draw.rc.top + (draw.rc.bottom - draw.rc.top - s(18)) / 2;
    HPEN pen = CreatePen(PS_SOLID, s(2), accent);
    HGDIOBJ old_pen = SelectObject(draw.hdc, pen),
            old_brush = SelectObject(draw.hdc, GetStockObject(NULL_BRUSH));
    auto line = [&](int x1, int y1, int x2, int y2) {
      MoveToEx(draw.hdc, x + s(x1), y + s(y1), nullptr);
      LineTo(draw.hdc, x + s(x2), y + s(y2));
    };
    if (glyph == CommandGlyph::run) {
      line(5, 3, 15, 9);
      line(15, 9, 5, 15);
      line(5, 15, 5, 3);
    } else if (glyph == CommandGlyph::stop) {
      Rectangle(draw.hdc, x + s(4), y + s(4), x + s(15), y + s(15));
    } else if (glyph == CommandGlyph::request) {
      line(3, 9, 16, 9);
      line(9, 3, 9, 16);
    } else if (glyph == CommandGlyph::open) {
      line(2, 15, 2, 4);
      line(2, 4, 7, 4);
      line(7, 4, 9, 7);
      line(9, 7, 17, 7);
      line(17, 7, 14, 15);
      line(14, 15, 2, 15);
    } else if (glyph == CommandGlyph::save) {
      Rectangle(draw.hdc, x + s(2), y + s(2), x + s(16), y + s(16));
      line(5, 3, 5, 8);
      line(5, 8, 12, 8);
      line(5, 13, 12, 13);
    } else {
      line(4, 7, 9, 12);
      line(9, 12, 14, 7);
    }
    SelectObject(draw.hdc, old_pen);
    SelectObject(draw.hdc, old_brush);
    DeleteObject(pen);
    RECT text = draw.rc;
    text.left += s(32);
    text.right -= s(6);
    draw_button_label(draw, text, disabled ? theme.chrome_muted : theme.chrome_text, DT_LEFT);
    if (draw.uItemState & CDIS_FOCUS) {
      RECT focus = draw.rc;
      InflateRect(&focus, -s(2), -s(2));
      outline(draw.hdc, focus, theme.chrome_focus, s(2));
    }
    RestoreDC(draw.hdc, saved);
    return CDRF_SKIPDEFAULT;
  }
  LRESULT draw_form_button(const NMCUSTOMDRAW &draw, const UiTheme &theme, int dpi, bool primary) {
    if (theme.system || draw.dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
    const bool disabled = (draw.uItemState & CDIS_DISABLED) != 0;
    const bool solid = primary && !disabled && !(draw.uItemState & (CDIS_SELECTED | CDIS_HOT));
    const auto fill = disabled                          ? theme.surface
                      : draw.uItemState & CDIS_SELECTED ? theme.pressed
                      : draw.uItemState & CDIS_HOT      ? theme.hover
                      : solid                           ? theme.blue
                                                        : theme.paper;
    const auto ink = disabled ? theme.disabled : solid ? theme.paper : theme.ink;
    const int saved = SaveDC(draw.hdc);
    FillRect(draw.hdc, &draw.rc, color_brush(draw.hdc, ink, fill));
    const bool focused = (draw.uItemState & CDIS_FOCUS) != 0;
    outline(draw.hdc, draw.rc,
            focused    ? (solid ? theme.paper : theme.focus)
            : disabled ? theme.divider
                       : theme.control_border,
            focused ? MulDiv(2, dpi, 96) : 1);
    draw_button_label(draw, draw.rc, ink, DT_CENTER);
    RestoreDC(draw.hdc, saved);
    return CDRF_SKIPDEFAULT;
  }
  LRESULT draw_tab_close(const DRAWITEMSTRUCT &draw, const UiTheme &theme, bool selected, bool hot,
                         int dpi) {
    const int saved = SaveDC(draw.hDC);
    const bool disabled = (draw.itemState & ODS_DISABLED) != 0;
    const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
    auto background = theme.tab_background(selected);
    if (!theme.system && !disabled && (pressed || hot))
      background = pressed ? theme.pressed : theme.hover;
    const auto ink = disabled                    ? theme.disabled
                     : theme.system && !selected ? theme.chrome_text
                                                 : theme.ink;
    FillRect(draw.hDC, &draw.rcItem, color_brush(draw.hDC, ink, background));
    const int radius = MulDiv(4, dpi, 96), width = MulDiv(2, dpi, 96);
    const int x = (draw.rcItem.left + draw.rcItem.right) / 2,
              y = (draw.rcItem.top + draw.rcItem.bottom) / 2;
    HPEN pen = CreatePen(PS_SOLID, width, ink);
    HGDIOBJ old = SelectObject(draw.hDC, pen);
    MoveToEx(draw.hDC, x - radius, y - radius, nullptr);
    LineTo(draw.hDC, x + radius + 1, y + radius + 1);
    MoveToEx(draw.hDC, x + radius, y - radius, nullptr);
    LineTo(draw.hDC, x - radius - 1, y + radius + 1);
    SelectObject(draw.hDC, old);
    DeleteObject(pen);
    if (draw.itemState & ODS_FOCUS) {
      RECT focus = draw.rcItem;
      InflateRect(&focus, -1, -1);
      outline(draw.hDC, focus, theme.focus, width);
    }
    RestoreDC(draw.hDC, saved);
    return TRUE;
  }
} // namespace flowie::app
