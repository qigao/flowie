#pragma once
#include "import_files.h"
#include "workbench_tools.h"

namespace flowie::app {
  constexpr size_t kSavedNameLimit = 64;
  constexpr size_t kSavedNotesLimit = 2048;
  constexpr size_t kSavedTopicLimit = 128;
  constexpr size_t kSavedRequestLimit = 64;
  constexpr size_t kRecentCollectionLimit = 12;
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
    std::string notes;
  };
  std::filesystem::path default_collections_file();
  bool same_collection_file(const std::filesystem::path &first,
                            const std::filesystem::path &second);
  std::filesystem::path read_collections_directory(const std::filesystem::path &settings);
  std::vector<std::filesystem::path> read_recent_collections(const std::filesystem::path &settings);
  void remember_collection_file(const std::filesystem::path &settings,
                                const std::filesystem::path &file);
  void validate_saved_names(const SavedRequest &request);

  struct CollectionDocument {
    std::vector<SavedRequest> requests;
    std::optional<CollectionVariables> variables;
    const CollectionVariables *bindings() const { return variables ? &*variables : nullptr; }
  };

  class CollectionStore {
  public:
    explicit CollectionStore(std::filesystem::path file) : file_(std::move(file)) {}
    std::vector<SavedRequest> load(bool require_existing = false) const;
    CollectionDocument load_document(bool require_existing = false) const;
    // Empty result means the named request already exists and replacement was not authorized.
    std::optional<std::vector<SavedRequest>> save(const SavedRequest &request, bool replace,
                                                  bool require_existing = false) const;
    std::optional<CollectionDocument> save_document(const SavedRequest &request, bool replace,
                                                    bool require_existing = false) const;

  private:
    std::filesystem::path file_;
  };
} // namespace flowie::app
