#include "import_files.h"
#include "main_window.h"
#include <atldlgs.h>
#include <memory>
#include <shlobj.h>
#include <stdexcept>

namespace flowie::app {
  namespace {
    std::optional<std::filesystem::path> choose_file(HWND owner, const COMDLG_FILTERSPEC &type,
                                                     const wchar_t *title,
                                                     const std::filesystem::path &directory = {}) {
      const HRESULT init =
          CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
      if (FAILED(init)) throw std::runtime_error("Could not initialize the Windows file picker.");
      struct Apartment {
        ~Apartment() { CoUninitialize(); }
      } apartment;
      CShellFileOpenDialog picker;
      if (picker.IsNull()) throw std::runtime_error("Could not create the Windows file picker.");
      auto *dialog = picker.GetPtr();
      if (FAILED(dialog->SetFileTypes(1, &type)) || FAILED(dialog->SetTitle(title)))
        throw std::runtime_error("Could not configure the Windows file picker.");
      if (!directory.empty() && std::filesystem::is_directory(directory)) {
        PIDLIST_ABSOLUTE id = nullptr;
        if (FAILED(SHParseDisplayName(directory.c_str(), nullptr, &id, 0, nullptr)))
          throw std::runtime_error("Could not resolve the last Collections folder.");
        const std::unique_ptr<ITEMIDLIST, decltype(&CoTaskMemFree)> parsed(id, CoTaskMemFree);
        CComPtr<IShellItem> folder;
        if (FAILED(SHCreateShellItem(nullptr, nullptr, parsed.get(), &folder)) ||
            FAILED(dialog->SetFolder(folder)))
          throw std::runtime_error("Could not open the last Collections folder.");
      }
      const auto shown = dialog->Show(owner);
      if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return std::nullopt;
      if (FAILED(shown)) throw std::runtime_error("Could not show the Windows file picker.");
      // Long Windows paths can exceed the older common file dialog's MAX_PATH buffer.
      std::wstring path(32768, L'\0');
      if (FAILED(picker.GetFilePath(path.data(), static_cast<int>(path.size()))))
        throw std::runtime_error("Could not read the selected file path.");
      path.resize(std::char_traits<wchar_t>::length(path.c_str()));
      return std::filesystem::path(std::move(path));
    }
  } // namespace
  std::optional<std::filesystem::path> choose_import_file(HWND owner, bool connection) {
    return connection ? choose_file(owner, {L"Connection profiles (*.json)", L"*.json"},
                                    L"Load MQTT Connection")
                      : choose_file(owner, {L"Message samples (*.txt; *.json)", L"*.txt;*.json"},
                                    L"Load Request Sample");
  }
  std::optional<std::filesystem::path> choose_collection_file(
      HWND owner, const std::filesystem::path &directory) {
    return choose_file(owner, {L"Collection files (*.json)", L"*.json"}, L"Open Collections",
                       directory);
  }
} // namespace flowie::app
