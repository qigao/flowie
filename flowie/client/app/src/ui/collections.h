#pragma once
#include "import_files.h"
#include "workbench_tools.h"

namespace flowie::app {
  constexpr size_t kSavedNameLimit = 64;
  constexpr size_t kSavedTopicLimit = 128;
  constexpr size_t kSavedRequestLimit = 64;
  struct SavedTopic {
    std::string filter;
    int qos;
  };
  struct SavedRequest {
    std::string collection, name;
    ConnectionProfile profile;
    std::vector<SavedTopic> topics;
    std::optional<MessageSample> message;
    std::optional<ToolDraft> tool;
  };
  std::filesystem::path default_collections_file();
  void validate_saved_names(const SavedRequest &request);

  class CollectionStore {
  public:
    explicit CollectionStore(std::filesystem::path file) : file_(std::move(file)) {}
    std::vector<SavedRequest> load(bool require_existing = false) const;
    // Empty result means the named request already exists and replacement was not authorized.
    std::optional<std::vector<SavedRequest>> save(const SavedRequest &request, bool replace,
                                                  bool require_existing = false) const;

  private:
    std::filesystem::path file_;
  };
} // namespace flowie::app
