#pragma once
#include "collections.h"

namespace flowie::app {
  bool name_saved_request(HWND owner, SavedRequest &request,
                          const std::vector<SavedRequest> &library,
                          const std::filesystem::path &file);
} // namespace flowie::app
