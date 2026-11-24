#include "save_request_dialog.h"
#include "connection_resource.h"
#include "main_window.h"
#include <set>
#include <stdexcept>

namespace flowie::app {
  namespace {
    std::string read_field(HWND control, bool trim) {
      std::wstring value(static_cast<size_t>(GetWindowTextLengthW(control)) + 1, L'\0');
      const int size = GetWindowTextW(control, value.data(), static_cast<int>(value.size()));
      std::wstring_view text(value.data(), size);
      if (trim) {
        const auto first = text.find_first_not_of(L' ');
        if (first == text.npos) return {};
        text = text.substr(first, text.find_last_not_of(L' ') - first + 1);
      }
      if (text.empty()) return {};
      const int count = static_cast<int>(text.size());
      const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), count,
                                            nullptr, 0, nullptr, nullptr);
      if (!bytes) throw std::runtime_error("The text contains invalid Unicode.");
      std::string utf8(static_cast<size_t>(bytes), '\0');
      if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), count, utf8.data(),
                              bytes, nullptr, nullptr) != bytes)
        throw std::runtime_error("Could not encode the text.");
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
                           L"Credential headers and URL query parameters are excluded.");
          else if (request_.tool) SetDlgItemText(IDC_SAVE_REQUEST_HINT, L"");
          SendDlgItemMessage(IDC_SAVE_REQUEST_NAME, EM_SETLIMITTEXT, kSavedNameLimit);
          SetDlgItemText(IDC_SAVE_REQUEST_NAME, utf8_to_wide(request_.name).c_str());
          SendDlgItemMessage(IDC_SAVE_REQUEST_NOTES, EM_SETLIMITTEXT, kSavedNotesLimit);
          SetDlgItemText(IDC_SAVE_REQUEST_NOTES, utf8_to_wide(request_.notes).c_str());
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
          request_.collection = read_field(GetDlgItem(IDC_SAVE_COLLECTION), true);
          request_.name = read_field(GetDlgItem(IDC_SAVE_REQUEST_NAME), true);
          request_.notes = read_field(GetDlgItem(IDC_SAVE_REQUEST_NOTES), false);
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
