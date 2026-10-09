#pragma once

#include <astra/service/editor_service.hpp>
#include <astra/script/runtime.hpp>
#include <astra/workspace/workspace.hpp>

#include <exception>
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
    using Scheduler = std::function<std::future<void>(
        std::function<void()>, std::function<void(std::exception_ptr)>)>;

    ApplicationService(const std::filesystem::path& root, Scheduler scheduler,
                       std::string_view defaults_script = {}, std::string_view user_script = {});
    ~ApplicationService();
    ApplicationService(const ApplicationService&) = delete;
    ApplicationService& operator=(const ApplicationService&) = delete;

    [[nodiscard]] std::optional<std::string> dispatch_text(std::string_view body);
    [[nodiscard]] std::optional<Json> dispatch(const Json& request);
    [[nodiscard]] std::vector<std::string> take_notifications();

private:
    struct FileBinding {
        std::filesystem::path path;
        io::FileVersion version;
        io::TextEncoding encoding{};
        config::ResolvedEditorConfig config;
    };
    struct Operation {
        std::string id;
        std::string kind;
        std::string state{"pending"};
        Json result;
        Json error;
        std::stop_source cancellation;
        std::optional<std::future<void>> future;
        std::shared_ptr<workspace::SearchResult> search;
    };

    [[nodiscard]] Json start_operation(std::string kind,
        std::function<void(const std::shared_ptr<Operation>&)> worker,
        std::function<Json(const std::shared_ptr<Operation>&)> complete);
    void poll_operations();
    void finish_operation(const std::shared_ptr<Operation>& operation, std::exception_ptr error);
    [[nodiscard]] Json describe(const Document& document) const;
    [[nodiscard]] Json open_file(const Json& params);
    [[nodiscard]] Json save_file(const Json& params);
    [[nodiscard]] Json tree(const Json& params);
    [[nodiscard]] Json search(const Json& params);
    [[nodiscard]] Json replace(const Json& params);
    [[nodiscard]] Json operation_get(const Json& params);
    [[nodiscard]] Json operation_cancel(const Json& params);
    [[nodiscard]] Json script_request(std::string_view method, const Json& params);
    void reconcile_saved(BufferId id, const DocumentSnapshot& snapshot, const workspace::SavedFile& saved);

    EditorService editor_;
    std::shared_ptr<workspace::Workspace> workspace_;
    Scheduler scheduler_;
    std::unique_ptr<script::Runtime> script_;
    std::map<std::uint64_t, FileBinding> files_;
    std::map<std::string, std::shared_ptr<Operation>> operations_;
    std::vector<Json> notifications_;
    std::map<std::uint64_t, std::string> saving_;
    std::uint64_t next_operation_{1};
    bool replacing_{};
};

} // namespace astra::service
