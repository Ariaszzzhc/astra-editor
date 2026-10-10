#pragma once

#include <astra/service/editor_service.hpp>
#include <astra/script/runtime.hpp>
#include <astra/workspace/workspace.hpp>
#include <astra/language/syntax.hpp>
#include <astra/recovery/store.hpp>

#include <exception>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace astra::service {

// Construct, invoke and destroy on the service loop. Only immutable snapshots
// and worker-owned results cross the worker boundary. The scheduler must invoke
// completion on this owner loop and settle every accepted task before destruction.
class ApplicationService final {
  public:
    using Json = protocol::Json;
    using Scheduler = std::function<std::future<void>(std::function<void()>,
                                                      std::function<void(std::exception_ptr)>)>;

    ApplicationService(const std::filesystem::path& root,
                       Scheduler scheduler,
                       std::string_view defaults_script = {},
                       std::string_view user_script = {},
                       const std::filesystem::path& resource_directory = {},
                       const std::filesystem::path& recovery_directory = {},
                       bool watch_files = false);
    ~ApplicationService();
    ApplicationService(const ApplicationService&) = delete;
    ApplicationService& operator=(const ApplicationService&) = delete;

    [[nodiscard]] std::optional<std::string> dispatch_text(std::string_view body);
    [[nodiscard]] std::optional<Json> dispatch(const Json& request);
    [[nodiscard]] std::vector<std::string> take_notifications();
    // Invoke on the owner loop after it has stopped accepting input and drained
    // workers. Disk work may block only this final, noninteractive shutdown phase.
    void checkpoint_for_shutdown();

  private:
    struct FileBinding {
        std::filesystem::path path;
        io::FileVersion version;
        io::TextEncoding encoding{};
        config::ResolvedEditorConfig config;
    };

    struct BufferSearch {
        DocumentSnapshot snapshot;
        std::optional<FileBinding> binding;
        std::vector<workspace::TextMatch> matches;
    };

    struct Operation {
        std::string id;
        std::uint64_t sequence{};
        std::string kind;
        std::string state{"pending"};
        Json result;
        Json error;
        std::stop_source cancellation;
        std::optional<std::future<void>> future;
        std::shared_ptr<workspace::SearchResult> search;
        std::shared_ptr<std::vector<BufferSearch>> search_buffers;
        // Fixed before scheduling, including schedulers that complete inline.
        bool workspace_mutation{};
        // Owner-loop only: a workspace mutation crossed this asynchronous read.
        bool open_invalidated{};
    };

    [[nodiscard]] Json
    start_operation(std::string kind,
                    std::function<void(const std::shared_ptr<Operation>&)> worker,
                    std::function<Json(const std::shared_ptr<Operation>&)> complete,
                    bool git_mutation = false);
    void poll_operations();
    void prune_operations();
    void finish_operation(const std::shared_ptr<Operation>& operation, std::exception_ptr error);
    [[nodiscard]] Json describe(const Document& document) const;
    [[nodiscard]] Json open_file(const Json& params);
    [[nodiscard]] Json save_file(const Json& params);
    [[nodiscard]] Json tree(const Json& params);
    [[nodiscard]] Json search(const Json& params);
    [[nodiscard]] Json replace(const Json& params);
    [[nodiscard]] Json find_document_text(const Json& params);
    [[nodiscard]] Json replace_document_text(const Json& params);
    [[nodiscard]] Json search_disk(const Json& params);
    [[nodiscard]] Json replace_disk(const Json& params);
    [[nodiscard]] Json operation_get(const Json& params);
    [[nodiscard]] Json operation_cancel(const Json& params);
    [[nodiscard]] Json syntax_view(const Json& params);
    [[nodiscard]] Json select_object(const Json& params);
    void register_git_methods(const std::filesystem::path& resource_directory);
    [[nodiscard]] Json git_request(std::string_view method, const Json& params);
    [[nodiscard]] Json script_request(std::string_view method, const Json& params);
    [[nodiscard]] Json script_command(const Json& params);
    void poll_scripts();
    [[nodiscard]] editing::Options command_defaults(const Document& document) const;
    void register_recovery_methods(const std::filesystem::path& directory);
    void poll_recovery();
    [[nodiscard]] Json recovery_request(std::string_view method, const Json& params);
    [[nodiscard]] std::vector<recovery::RecoveryRecord> capture_recovery() const;
    void finish_recovery_operation(const std::shared_ptr<Operation>& operation,
                                   std::exception_ptr error);
    void register_watch_methods(bool enabled);
    void poll_watch();
    [[nodiscard]] Json reload_file(const Json& params);
    void register_language_methods(const std::filesystem::path& resource_directory);
    void poll_language();
    [[nodiscard]] Json language_request(const Json& params);
    [[nodiscard]] Json language_diagnostics(const Json& params);
    [[nodiscard]] Json language_apply_text_edits(const Json& params);
    [[nodiscard]] Json language_apply_completion(const Json& params);
    [[nodiscard]] Json language_prepare_workspace_edit(const Json& params);
    [[nodiscard]] Json language_apply_workspace_edit(const Json& params);
    void reconcile_saved(BufferId id,
                         const DocumentSnapshot& snapshot,
                         const workspace::SavedFile& saved);

    EditorService editor_;
    std::shared_ptr<workspace::Workspace> workspace_;
    Scheduler scheduler_;
    std::unique_ptr<script::Runtime> script_;
    std::map<std::string, script::Runtime::TaskId> script_tasks_;
    bool polling_scripts_{};
    std::map<std::uint64_t, FileBinding> files_;
    std::map<std::string, std::shared_ptr<Operation>> operations_;
    std::vector<Json> notifications_;
    std::map<std::uint64_t, std::string> saving_;
    std::uint64_t next_operation_{1};
    bool replacing_{};

    struct SyntaxJob {
        Revision revision;
        language::LanguageId language;
        std::string operation;
    };

    std::map<std::uint64_t, language::SyntaxSnapshot> syntax_;
    std::map<std::uint64_t, SyntaxJob> syntax_jobs_;

    struct GitPreview {
        std::filesystem::path path;
        std::optional<io::FileVersion> version;
        std::optional<io::TextEncoding> encoding;
    };

    std::map<std::string, GitPreview> git_previews_;
    std::optional<std::string> git_mutation_;
    std::filesystem::path git_resource_directory_;
    struct LanguageState;
    std::shared_ptr<LanguageState> language_;
    struct RecoveryState;
    std::shared_ptr<RecoveryState> recovery_;
    struct WatchState;
    std::shared_ptr<WatchState> watch_;
};

} // namespace astra::service
