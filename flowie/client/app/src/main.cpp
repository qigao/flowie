#include "main_workbench.h"
#include <algorithm>

CAppModule _Module;

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES |
                                                      ICC_TAB_CLASSES | ICC_TREEVIEW_CLASSES |
                                                      ICC_BAR_CLASSES};
  if (!InitCommonControlsEx(&controls)) return 1;
  RECT work{};
  if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) return 1;
  HDC screen = ::GetDC(nullptr);
  if (!screen) return 1;
  const int dpi = GetDeviceCaps(screen, LOGPIXELSX);
  ::ReleaseDC(nullptr, screen);
  if (FAILED(_Module.Init(nullptr, instance))) return 2;
  int result = 1;
  {
    flowie::app::WorkbenchWindow window;
    RECT bounds{
        work.left + 16, work.top + 16,
        work.left + 16 +
            (std::min)(MulDiv(1200, dpi, 96), static_cast<int>(work.right - work.left - 32)),
        work.top + 16 +
            (std::min)(MulDiv(960, dpi, 96), static_cast<int>(work.bottom - work.top - 32))};
    if (window.Create(nullptr, bounds, L"Flowie Workbench",
                      WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN) != nullptr) {
      window.ShowWindow(show);
      window.UpdateWindow();
      MSG message{};
      BOOL status;
      while ((status = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        if (!window.translate(&message)) {
          TranslateMessage(&message);
          DispatchMessageW(&message);
        }
      }
      result = status == -1 ? 3 : static_cast<int>(message.wParam);
    }
  }
  _Module.Term();
  return result;
}
