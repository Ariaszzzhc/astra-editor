#include <astra/service/application_service.hpp>
#include <astra/git/repository.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace astra::service {
namespace {
using protocol::Json;
using protocol::RpcError;
namespace fs = std::filesystem;

std::string path_string(const fs::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
fs::path relative_path(const Json& value) {
    if (!value.is_string()) { throw RpcError(-32602, "Git paths must be UTF-8 strings"); }
    const auto& text = value.get_ref<const std::string&>();
    if (text.empty() || text.size() > 32768 || text.find('\0') != std::string::npos) { throw RpcError(-32602, "Invalid Git path"); }
    fs::path path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
    if (path.is_absolute() || path.has_root_name()) { throw RpcError(-32602, "Git paths must be workspace-relative"); }
    for (const auto& part : path) { if (part == "..") { throw RpcError(-32602, "Parent paths are not allowed"); } }
    return path.lexically_normal();
}
bool same_path(const fs::path& a, const fs::path& b) {
#ifdef _WIN32
    const auto left = a.lexically_normal().native();
    const auto right = b.lexically_normal().native();
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
    return a.lexically_normal() == b.lexically_normal();
#endif
}
void fields(const Json& params, std::initializer_list<std::string_view> allowed) {
    if (!params.is_object()) { throw RpcError(-32602, "Named Git parameters required"); }
    for (auto i = params.begin(); i != params.end(); ++i) {
        if (std::find(allowed.begin(), allowed.end(), i.key()) == allowed.end()) { throw RpcError(-32602, "Unknown Git parameter"); }
    }
}
std::string string_field(const Json& params, const char* key) {
    if (!params.contains(key) || !params[key].is_string()) { throw RpcError(-32602, std::string("Required string: ") + key); }
    return params[key].get<std::string>();
}
bool flag(const Json& params, const char* key, bool fallback = false) {
    if (!params.contains(key)) { return fallback; }
    if (!params[key].is_boolean()) { throw RpcError(-32602, "Git flag must be boolean"); }
    return params[key].get<bool>();
}
std::string_view state_name(git::FileState state) {
    static constexpr std::array names{"unchanged", "modified", "added", "deleted", "renamed", "copied",
                                       "unmerged", "untracked", "ignored", "typeChanged"};
    return names.at(static_cast<std::size_t>(state));
}
Json status_json(const git::RepositoryStatus& status) {
    auto entries = Json::array();
    for (const auto& item : status.entries) {
        Json entry{{"path", path_string(item.path)}, {"staged", state_name(item.staged)},
                    {"unstaged", state_name(item.unstaged)}, {"conflict", item.conflict}, {"submodule", item.submodule}};
        if (item.original_path) { entry["originalPath"] = path_string(*item.original_path); }
        entries.push_back(std::move(entry));
    }
    return {{"branch", status.branch}, {"head", status.head}, {"upstream", status.upstream},
             {"ahead", status.ahead}, {"behind", status.behind}, {"unborn", status.unborn},
             {"detached", status.detached}, {"entries", std::move(entries)}};
}
Json diff_json(const git::DiffResult& diff) {
    auto hunks = Json::array();
    for (const auto& hunk : diff.hunks) {
        hunks.push_back({{"oldStart", hunk.old_start}, {"oldCount", hunk.old_count},
                         {"newStart", hunk.new_start}, {"newCount", hunk.new_count}});
    }
    auto gutter = Json::array();
    for (const auto& mark : diff.gutter) {
        const auto kind = mark.kind == git::GutterKind::added ? "added" : mark.kind == git::GutterKind::modified ? "modified" : "deleted";
        gutter.push_back({{"line", mark.line}, {"count", mark.count}, {"removedLines", mark.removed_lines}, {"kind", kind}});
    }
    return {{"patch", diff.patch}, {"binary", diff.binary}, {"hunks", std::move(hunks)}, {"gutter", std::move(gutter)}};
}
} // namespace

void ApplicationService::register_git_methods(const fs::path& resource_directory) {
    git_resource_directory_ = resource_directory;
    for (const auto name : {"git.status", "git.diff", "git.log", "git.branches", "git.stage", "git.unstage",
                           "git.discard", "git.switchBranch", "git.createBranch", "git.commit"}) {
        editor_.dispatcher().register_method(name, [this, name](const Json& p) { return git_request(name, p); });
    }
}

Json ApplicationService::git_request(std::string_view method, const Json& params) {
    const bool mutation = method == "git.stage" || method == "git.unstage" || method == "git.discard" ||
                          method == "git.switchBranch" || method == "git.createBranch" || method == "git.commit";
    const bool checkout = method == "git.switchBranch" || (method == "git.createBranch" && flag(params, "checkout"));
    if (mutation && (git_mutation_ || replacing_ || !saving_.empty())) { throw RpcError(-32013, "Wait for the current Git or file mutation"); }
    std::vector<fs::path> paths;
    std::vector<git::StagePath> staging;
    std::map<std::uint64_t, DocumentSnapshot> snapshots;
    std::optional<GitPreview> preview;
    std::string name;
    std::size_t limit = 100;
    bool staged = false;
    if (method == "git.diff") {
        fields(params, {"path", "staged"});
        if (!params.contains("path")) { throw RpcError(-32602, "Git diff requires path"); }
        paths.push_back(relative_path(params["path"]));
        staged = flag(params, "staged");
    } else if (method == "git.stage" || method == "git.unstage") {
        fields(params, {"paths"});
        if (!params.contains("paths") || !params["paths"].is_array() || params["paths"].empty() || params["paths"].size() > 256) {
            throw RpcError(-32602, "Git paths must contain between 1 and 256 entries");
        }
        for (const auto& path : params["paths"]) { paths.push_back(relative_path(path)); }
    } else if (method == "git.discard") {
        fields(params, {"previewId"});
        const auto found = git_previews_.find(string_field(params, "previewId"));
        if (found == git_previews_.end()) { throw RpcError(-32002, "Read a fresh Git diff before discarding changes"); }
        preview = found->second;
        paths.push_back(preview->path);
    } else if (method == "git.switchBranch" || method == "git.createBranch") {
        fields(params, {"name", "checkout"});
        name = string_field(params, "name");
    } else if (method == "git.commit") {
        fields(params, {"message"});
        name = string_field(params, "message");
    } else if (method == "git.log") {
        fields(params, {"limit"});
        if (params.contains("limit")) {
            const auto& count = params["limit"];
            if (!count.is_number_integer() || (!count.is_number_unsigned() && count.get<std::int64_t>() <= 0) ||
                count.get<std::uint64_t>() == 0 || count.get<std::uint64_t>() > 1000) { throw RpcError(-32602, "Git log limit must be 1 to 1000"); }
            limit = count.get<std::size_t>();
        }
    } else { fields(params, {}); }
    for (const auto& path : paths) {
        git::StagePath staged_path{path, std::nullopt};
        for (const auto& [id, binding] : files_) {
            const auto doc = editor_.documents_.find(id);
            if (doc != editor_.documents_.end() && same_path(binding.path, path)) {
                if (mutation && doc->second.dirty()) { throw RpcError(-32013, "Save or discard unsaved buffer edits before this Git action"); }
                if (binding.version.raw_content) { staged_path.expected = binding.version; }
                snapshots.emplace(id, doc->second.snapshot());
            }
        }
        staging.push_back(std::move(staged_path));
    }
    std::map<std::uint64_t, fs::path> reload_paths;
    if (checkout) {
        for (const auto& [id, doc] : editor_.documents_) {
            if (doc.dirty()) { throw RpcError(-32013, "Save modified documents before switching branches"); }
            if (const auto file = files_.find(id); file != files_.end()) {
                snapshots.emplace(id, doc.snapshot());
                reload_paths.emplace(id, file->second.path);
            }
        }
    }
    struct WorkResult {
        Json result;
        std::optional<GitPreview> preview;
        std::map<std::uint64_t, workspace::SavedFile> reloaded;
        std::vector<std::uint64_t> missing;
    };
    auto output = std::make_shared<WorkResult>();
    const auto method_name = std::string(method);
    const auto git_settings = script_->settings().value("git", Json::object());
    if (!git_settings.is_object()) { throw RpcError(-32020, "git settings must be an object"); }
    std::optional<std::string> executable;
    if (git_settings.contains("executable")) {
        executable = string_field(git_settings, "executable");
        if (executable->empty() || executable->find('\0') != std::string::npos || executable->size() > 32768) {
            throw RpcError(-32020, "git.executable must be a nonempty executable path");
        }
        static_cast<void>(Rope(*executable));
    }
    git::RepositoryOptions repository_options;
    repository_options.inherit_global_config = git_settings.value("inheritGlobalConfig", true);
    const auto author_name = git_settings.contains("authorName") ? std::optional<std::string>(git_settings.at("authorName").get<std::string>()) : std::nullopt;
    const auto author_email = git_settings.contains("authorEmail") ? std::optional<std::string>(git_settings.at("authorEmail").get<std::string>()) : std::nullopt;
    const auto started = start_operation(method_name, [workspace = workspace_, method_name, paths, staging, preview, name,
                                                       limit, staged, checkout, reload_paths, snapshots, output,
                                                       repository_options, author_name, author_email, executable,
                                                       resources = git_resource_directory_](const auto& operation) {
        auto git_executable = executable.value_or("git");
        if (!executable && !resources.empty()) {
#ifdef _WIN32
            const auto bundled = resources / "tools" / "git" / "cmd" / "git.exe";
#else
            const auto bundled = resources / "tools" / "git" / "bin" / "git";
#endif
            if (fs::is_regular_file(bundled)) { git_executable = path_string(bundled); }
        }
        git::Repository repository(workspace->root(), std::move(git_executable), repository_options);
        git::GitOptions options;
        options.stop = operation->cancellation.get_token();
        options.max_output_bytes = 1024 * 1024;
        options.max_entries = 10000;
        options.author_name = author_name;
        options.author_email = author_email;
        if (method_name == "git.status") { output->result = status_json(repository.status(options)); }
        else if (method_name == "git.diff") {
            GitPreview item{paths.front(), std::nullopt, std::nullopt};
            const auto absolute = workspace->resolve_path(item.path);
            if (fs::exists(absolute) && fs::is_regular_file(absolute)) {
                try {
                    const auto file = workspace->load(item.path);
                    item.version = file.snapshot.version;
                    item.encoding = file.snapshot.encoding;
                } catch (const std::invalid_argument&) { /* Binary diff can be read, but cannot supply a text-discard preview. */ }
            }
            output->result = diff_json(repository.diff(item.path, git::DiffBase::index, staged, options));
            if (item.version) {
                const auto verified = io::read_file(absolute, io::default_max_file_bytes, item.encoding);
                if (*verified.version.raw_content != *item.version->raw_content) { throw io::FileConflict("File changed while reading Git preview"); }
            }
            if (item.version || !fs::exists(absolute)) { output->preview = std::move(item); }
        } else if (method_name == "git.log") {
            auto commits = Json::array();
            for (const auto& entry : repository.log(limit, options)) {
                commits.push_back({{"id", entry.id}, {"parents", entry.parents}, {"author", entry.author},
                                   {"email", entry.email}, {"timestamp", entry.timestamp}, {"subject", entry.subject}});
            }
            output->result = {{"commits", std::move(commits)}};
        } else if (method_name == "git.branches") {
            auto branches = Json::array();
            for (const auto& entry : repository.branches(options)) {
                branches.push_back({{"ref", entry.ref}, {"commit", entry.commit}, {"upstream", entry.upstream},
                                    {"current", entry.current}, {"remote", entry.remote}});
            }
            output->result = {{"branches", std::move(branches)}};
        } else {
            if (method_name == "git.stage") { repository.stage(staging, options); }
            else if (method_name == "git.unstage") { repository.unstage(paths, options); }
            else if (method_name == "git.switchBranch") { repository.switch_branch(name, options); }
            else if (method_name == "git.createBranch") { repository.create_branch(name, checkout, options); }
            else if (method_name == "git.commit") { static_cast<void>(repository.commit(name, options)); }
            else if (method_name == "git.discard") {
                const auto restored = repository.discard(preview->path, preview->version, preview->encoding, options);
                for (const auto& [id, snapshot] : snapshots) {
                    static_cast<void>(snapshot);
                    const auto config = workspace->editor_config(preview->path);
                    output->reloaded.emplace(id, workspace::SavedFile{preview->path, restored.version, restored.text, restored.encoding,
                                                                      config.config, config.issues});
                }
            }
            if (checkout) {
                for (const auto& [id, path] : reload_paths) {
                    if (!fs::exists(workspace->resolve_path(path))) { output->missing.push_back(id); continue; }
                    const auto loaded = workspace->load(path);
                    output->reloaded.emplace(id, workspace::SavedFile{path, loaded.snapshot.version, loaded.snapshot.text,
                                                                      loaded.snapshot.encoding, loaded.config, loaded.issues});
                }
            }
            output->result = status_json(repository.status(options));
        }
    }, [this, output, snapshots, mutation](const auto& operation) {
        if (output->preview) {
            std::size_t retained = output->preview->version ? output->preview->version->raw_content->size() : 0;
            for (const auto& [id, value] : git_previews_) {
                static_cast<void>(id);
                if (value.version) { retained += value.version->raw_content->size(); }
            }
            while (!git_previews_.empty() && (git_previews_.size() >= 16 || retained > 128 * 1024 * 1024)) {
                const auto old = git_previews_.begin();
                if (old->second.version) { retained -= old->second.version->raw_content->size(); }
                git_previews_.erase(old);
            }
            git_previews_.insert_or_assign(operation->id, *output->preview);
            output->result["previewId"] = operation->id;
        }
        for (const auto& [id, saved] : output->reloaded) { reconcile_saved(BufferId{id}, snapshots.at(id), saved); }
        for (const auto id : output->missing) {
            if (const auto document = editor_.documents_.find(id); document != editor_.documents_.end()) {
                document->second.invalidate_saved_state();
                if (const auto file = files_.find(id); file != files_.end()) { file->second.version.raw_content.reset(); }
                editor_.changed(document->second, "deletedOnBranch");
            }
        }
        if (mutation) { notifications_.push_back(protocol::make_notification("git.changed", {{"operationId", operation->id}})); }
        return std::move(output->result);
    }, mutation);
    return started;
}
} // namespace astra::service
