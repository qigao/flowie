#include "save_request_dialog.h"
#include "connection_resource.h"
#include "main_window.h"
#include <set>
#include <stdexcept>

namespace flowie::app {
  namespace {
    std::string read_name(HWND control) {
      wchar_t value[kSavedNameLimit + 1]{};
      const int size = GetWindowTextW(control, value, static_cast<int>(std::size(value)));
      const auto first = std::wstring_view(value, size).find_first_not_of(L' ');
      if (first == std::wstring_view::npos) return {};
      const auto last = std::wstring_view(value, size).find_last_not_of(L' ');
      const int count = static_cast<int>(last - first + 1);
      const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value + first, count,
                                            nullptr, 0, nullptr, nullptr);
      if (!bytes) throw std::runtime_error("The name contains invalid Unicode.");
      std::string utf8(static_cast<size_t>(bytes), '\0');
      if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value + first, count, utf8.data(),
                              bytes, nullptr, nullptr) != bytes)
        throw std::runtime_error("Could not encode the name.");
      return utf8;
    }

    class SaveRequestDialog : public CDialogImpl<SaveRequestDialog> {
    public:
      enum { IDD = IDD_SAVE_REQUEST };
      SaveRequestDialog(SavedRequest &request, const std::vector<SavedRequest> &library,
                        const std::filesystem::path &file)
          : request_(request), library_(library), file_(file) {}
      BEGIN_MSG_MAP(SaveRequestDialog)
      MESSAGE_HANDLER(WM_INITDIALOG, initialize)
      COMMAND_ID_HANDLER(IDOK, accept)
      COMMAND_ID_HANDLER(IDCANCEL, cancel)
      END_MSG_MAP()

      LRESULT initialize(UINT, WPARAM, LPARAM, BOOL &) {
        try {
          SetWindowText((L"Save Request to " + file_.filename().wstring()).c_str());
          CComboBox collections(GetDlgItem(IDC_SAVE_COLLECTION));
          collections.LimitText(static_cast<int>(kSavedNameLimit));
          std::set<std::string> names;
          for (const auto &entry : library_)
            if (names.insert(entry.collection).second &&
                collections.AddString(utf8_to_wide(entry.collection).c_str()) < 0)
              throw std::runtime_error("Could not list existing collections.");
          collections.SetWindowText(request_.collection.empty()
                                        ? (request_.tool ? L"Requests" : L"MQTT requests")
                                        : utf8_to_wide(request_.collection).c_str());
          if (request_.tool && request_tool(request_.tool->kind))
            SetDlgItemText(IDC_SAVE_REQUEST_HINT,
                           L"Headers and URL query parameters are excluded.");
          else if (request_.tool) SetDlgItemText(IDC_SAVE_REQUEST_HINT, L"");
          SendDlgItemMessage(IDC_SAVE_REQUEST_NAME, EM_SETLIMITTEXT, kSavedNameLimit);
          SetDlgItemText(IDC_SAVE_REQUEST_NAME, utf8_to_wide(request_.name).c_str());
          ::SetFocus(GetDlgItem(IDC_SAVE_REQUEST_NAME));
          SendDlgItemMessage(IDC_SAVE_REQUEST_NAME, EM_SETSEL, 0, -1);
          return FALSE;
        } catch (const std::exception &) {
          EndDialog(-1);
          return TRUE;
        }
      }

      LRESULT accept(WORD, WORD, HWND, BOOL &) {
        try {
          request_.collection = read_name(GetDlgItem(IDC_SAVE_COLLECTION));
          request_.name = read_name(GetDlgItem(IDC_SAVE_REQUEST_NAME));
          validate_saved_names(request_);
          EndDialog(IDOK);
        } catch (const std::exception &error) {
          ::SetDlgItemTextA(m_hWnd, IDC_SAVE_REQUEST_ERROR, error.what());
          ::SetFocus(GetDlgItem(request_.collection.empty() ? IDC_SAVE_COLLECTION
                                                            : IDC_SAVE_REQUEST_NAME));
        }
        return 0;
      }

      LRESULT cancel(WORD, WORD, HWND, BOOL &) {
        EndDialog(IDCANCEL);
        return 0;
      }

    private:
      SavedRequest &request_;
      const std::vector<SavedRequest> &library_;
      const std::filesystem::path &file_;
    };
  } // namespace

  bool name_saved_request(HWND owner, SavedRequest &request,
                          const std::vector<SavedRequest> &library,
                          const std::filesystem::path &file) {
    SaveRequestDialog dialog(request, library, file);
    const auto result = dialog.DoModal(owner);
    if (result == -1) throw std::runtime_error("Could not open the Save Request dialog.");
    return result == IDOK;
  }
} // namespace flowie::app
