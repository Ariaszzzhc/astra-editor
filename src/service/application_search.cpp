#include <astra/service/application_service.hpp>

#include <algorithm>
#include <limits>
#include <set>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace astra::service {
namespace {
using protocol::Json;
using protocol::RpcError;
namespace fs = std::filesystem;

void fields(const Json& value, std::initializer_list<std::string_view> allowed) {
    if (!value.is_object()) {
        throw RpcError(-32602, "Named search parameters required");
    }
    for (auto i = value.begin(); i != value.end(); ++i) {
        if (std::find(allowed.begin(), allowed.end(), i.key()) == allowed.end()) {
            throw RpcError(-32602, "Unknown search parameter");
        }
    }
}

std::string string_field(const Json& value, const char* key) {
    if (!value.contains(key) || !value[key].is_string()) {
        throw RpcError(-32602, std::string("Required string: ") + key);
    }
    return value[key].get<std::string>();
}

bool flag(const Json& value, const char* key, bool fallback = false) {
    if (!value.contains(key)) {
        return fallback;
    }
    if (!value[key].is_boolean()) {
        throw RpcError(-32602, std::string("Invalid flag: ") + key);
    }
    return value[key].get<bool>();
}

std::string path_text(const fs::path& path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

bool same_path(const fs::path& left, const fs::path& right) {
#ifdef _WIN32
    const auto a = left.lexically_normal().native();
    const auto b = right.lexically_normal().native();
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
    return left.lexically_normal() == right.lexically_normal();
#endif
}

Json issues_json(const std::vector<workspace::WorkspaceIssue>& issues) {
    auto result = Json::array();
    for (const auto& issue : issues) {
        Json value{{"path", path_text(issue.relative_path)}, {"message", issue.message}};
        if (issue.recovery_path) {
            value["recoveryPath"] = path_text(*issue.recovery_path);
        }
        result.push_back(std::move(value));
    }
    return result;
}
} // namespace

Json ApplicationService::search(const Json& params) {
    fields(
        params,
        {"query", "wholeWord", "includeIgnored", "maxMatches", "caseSensitive", "regex", "scope"});
    const auto scope = params.contains("scope") ? string_field(params, "scope") : "workspace";
    if (scope != "workspace" && scope != "disk") {
        throw RpcError(-32602, "Search scope must be workspace or disk");
    }
    auto disk_params = params;
    disk_params.erase("scope");
    if (scope == "disk") {
        return search_disk(disk_params);
    }
    workspace::SearchOptions options;
    options.query = string_field(params, "query");
    options.whole_word = flag(params, "wholeWord");
    options.case_sensitive = flag(params, "caseSensitive", true);
    options.regex = flag(params, "regex");
    options.tree.include_ignored = flag(params, "includeIgnored");
    if (params.contains("maxMatches")) {
        const auto& count = params.at("maxMatches");
        if (!count.is_number_integer() ||
            (!count.is_number_unsigned() && count.get<std::int64_t>() < 0) ||
            count.get<std::uint64_t>() > 100000) {
            throw RpcError(-32602, "maxMatches must be 0 to 100000");
        }
        options.max_matches = count.get<std::size_t>();
    }
    const auto buffers = std::make_shared<std::vector<BufferSearch>>();
    for (const auto& [id, document] : editor_.documents_) {
        const auto file = files_.find(id);
        if (file != files_.end() && !document.dirty()) {
            continue;
        }
        if (buffers->size() >= 4096) {
            throw RpcError(-32004, "At most 4096 unsaved buffers can be searched at once");
        }
        buffers->push_back({document.snapshot(),
                            file == files_.end() ? std::nullopt : std::optional(file->second),
                            {}});
        if (file != files_.end()) {
            options.excluded_paths.push_back(file->second.path);
        }
    }
    if (buffers->empty()) {
        return search_disk(disk_params);
    }
    auto wire_matches = std::make_shared<Json>(Json::array());
    auto matched_files = std::make_shared<std::set<std::string>>();
    return start_operation(
        "search",
        [workspace = workspace_, options, buffers, wire_matches, matched_files](
            const auto& operation) {
            workspace::SearchPattern pattern(options);
            auto disk_options = options;
            std::size_t bytes = 0;
            std::size_t metadata_bytes = 0;
            bool truncated = false;
            bool cancelled = false;
            const auto stop = operation->cancellation.get_token();
            std::vector<workspace::WorkspaceIssue> issues;
            for (auto& buffer : *buffers) {
                if (stop.stop_requested()) {
                    cancelled = true;
                    break;
                }
                const auto& snapshot = buffer.snapshot;
                const auto path = buffer.binding
                                      ? buffer.binding->path
                                      : fs::path("Untitled-" + std::to_string(snapshot.id.value));
                const auto size = snapshot.text.size_bytes();
                if (size > options.max_file_bytes || size > options.max_total_bytes - bytes) {
                    truncated = true;
                    issues.push_back(
                        {path, "Unsaved buffer exceeds the search byte budget", std::nullopt});
                    continue;
                }
                bytes += size;
                auto found = pattern.find_all(snapshot.text.str(), stop, disk_options.max_matches);
                truncated = truncated || found.truncated;
                cancelled = cancelled || found.cancelled;
                const auto label = path_text(path);
                for (auto& match : found.matches) {
                    const auto cost =
                        label.size() + 192 + match.captures.size() * sizeof(workspace::TextRange);
                    if (cost > options.max_result_bytes - metadata_bytes) {
                        truncated = true;
                        break;
                    }
                    metadata_bytes += cost;
                    const auto position = snapshot.text.byte_to_position(match.begin.value);
                    wire_matches->push_back({{"path", label},
                                             {"documentId", std::to_string(snapshot.id.value)},
                                             {"revision", std::to_string(snapshot.revision.value)},
                                             {"source", "buffer"},
                                             {"begin", match.begin.value},
                                             {"end", match.end.value},
                                             {"line", position.line},
                                             {"utf16Column", position.utf16_column}});
                    matched_files->insert(label);
                    buffer.matches.push_back(std::move(match));
                }
                disk_options.max_matches = options.max_matches - wire_matches->size();
            }
            disk_options.max_total_bytes -= bytes;
            disk_options.max_result_bytes -= metadata_bytes;
            auto result =
                std::make_shared<workspace::SearchResult>(workspace->search(disk_options, stop));
            result->truncated = result->truncated || truncated;
            result->cancelled = result->cancelled || cancelled;
            result->issues.insert(result->issues.end(), issues.begin(), issues.end());
            for (const auto& match : result->matches) {
                const auto label = path_text(match.relative_path);
                wire_matches->push_back({{"path", label},
                                         {"source", "disk"},
                                         {"begin", match.begin.value},
                                         {"end", match.end.value},
                                         {"line", match.start_position.line},
                                         {"utf16Column", match.start_position.utf16_column}});
                matched_files->insert(label);
            }
            operation->search = std::move(result);
            operation->search_buffers = buffers;
        },
        [wire_matches, matched_files](const auto& operation) {
            const auto& result = *operation->search;
            return Json{{"query", result.query},
                        {"matches", std::move(*wire_matches)},
                        {"fileCount", matched_files->size()},
                        {"scope", "workspace"},
                        {"issues", issues_json(result.issues)},
                        {"truncated", result.truncated},
                        {"cancelled", result.cancelled}};
        });
}

Json ApplicationService::replace(const Json& params) {
    fields(params, {"searchId", "replacement"});
    const auto found = operations_.find(string_field(params, "searchId"));
    if (found == operations_.end() || found->second->state != "completed" ||
        !found->second->search) {
        throw RpcError(-32002, "A completed search operation is required");
    }
    const auto buffers = found->second->search_buffers;
    if (!buffers || std::none_of(buffers->begin(), buffers->end(), [](const auto& buffer) {
            return !buffer.matches.empty();
        })) {
        return replace_disk(params);
    }
    if (replacing_ || git_mutation_ || !saving_.empty()) {
        throw RpcError(-32013, "Wait for active file mutations to finish");
    }
    const auto search_result = found->second->search;
    if (search_result->truncated || search_result->cancelled) {
        throw RpcError(-32013, "Refine an incomplete search before replacing");
    }
    const auto replacement = string_field(params, "replacement");

    struct Replacement {
        std::map<std::uint64_t, Document> prepared;
        std::map<std::uint64_t, std::vector<Selection>> selections;
        std::map<std::uint64_t, std::optional<io::FileVersion>> versions;
        std::map<std::uint64_t, DocumentSnapshot> clean_snapshots;
        workspace::ReplaceResult disk;
    };

    const auto output = std::make_shared<Replacement>();
    for (const auto& buffer : *buffers) {
        if (buffer.matches.empty()) {
            continue;
        }
        const auto id = buffer.snapshot.id.value;
        const auto document = editor_.documents_.find(id);
        const auto file = files_.find(id);
        if (document == editor_.documents_.end() ||
            document->second.revision() != buffer.snapshot.revision ||
            document->second.content_identity() != buffer.snapshot.content_identity ||
            (buffer.binding.has_value() != (file != files_.end())) ||
            (buffer.binding && !same_path(buffer.binding->path, file->second.path))) {
            throw RpcError(-32001, "Search buffer changed; search again before replacing");
        }
        output->prepared.emplace(id, document->second.clone());
        output->selections.emplace(id, document->second.selections());
        output->versions.emplace(
            id, file == files_.end() ? std::nullopt : std::optional(file->second.version));
    }
    for (const auto& match : search_result->matches) {
        for (const auto& [id, file] : files_) {
            const auto document = editor_.documents_.find(id);
            if (document != editor_.documents_.end() &&
                (same_path(file.path, match.relative_path) ||
                 same_path(file.version.path, match.version.path))) {
                if (document->second.dirty()) {
                    throw RpcError(-32013,
                                   "A disk search result now has unsaved edits; search again");
                }
                output->clean_snapshots.try_emplace(id, document->second.snapshot());
            }
        }
    }
    const auto result = start_operation(
        "replace",
        [workspace = workspace_, search_result, buffers, replacement, output](
            const auto& operation) {
            const auto stop = operation->cancellation.get_token();
            workspace::SearchPattern pattern(search_result->options);
            pattern.validate_replacement(replacement);
            for (const auto& buffer : *buffers) {
                if (buffer.matches.empty()) {
                    continue;
                }
                if (stop.stop_requested()) {
                    throw RpcError(-32013, "Replace cancelled before writing");
                }
                if (buffer.matches.size() > EditorService::max_transaction_edits) {
                    throw RpcError(-32004, "Refine a buffer search to at most 4096 replacements");
                }
                const auto original = buffer.snapshot.text.str();
                std::vector<Edit> edits;
                std::size_t size = original.size();
                for (const auto& match : buffer.matches) {
                    size -= match.end.value - match.begin.value;
                    auto text = pattern.expand_replacement(
                        original,
                        match,
                        replacement,
                        workspace::SearchPattern::max_input_bytes - size);
                    size += text.size();
                    if (!edits.empty() && edits.back().end == match.begin &&
                        (edits.back().begin == edits.back().end || match.begin == match.end)) {
                        edits.back().end = match.end;
                        edits.back().text += text;
                    } else {
                        edits.push_back({match.begin, match.end, std::move(text)});
                    }
                }
                auto& document = output->prepared.at(buffer.snapshot.id.value);
                static_cast<void>(
                    document.apply({document.revision(), std::move(edits), std::nullopt}));
            }
            const auto plan = workspace->plan_replace(*search_result, replacement, stop);
            output->disk = workspace->apply_replace(plan, stop);
        },
        [this, buffers, output](const auto& operation) {
            auto applied = Json::array();
            for (const auto& file : output->disk.applied) {
                applied.push_back(path_text(file.relative_path));
                for (const auto& [id, snapshot] : output->clean_snapshots) {
                    const auto bound = files_.find(id);
                    if (bound != files_.end() &&
                        (same_path(bound->second.path, file.relative_path) ||
                         same_path(bound->second.version.path, file.version.path))) {
                        reconcile_saved(BufferId{id}, snapshot, file);
                    }
                }
            }
            auto pending = Json::array();
            for (const auto& path : output->disk.pending) {
                pending.push_back(path_text(path));
            }
            auto modified_buffers = Json::array();
            auto pending_buffers = Json::array();
            auto events = editor_.notifications_;
            std::vector<std::uint64_t> publish;
            for (const auto& buffer : *buffers) {
                if (buffer.matches.empty()) {
                    continue;
                }
                const auto id = buffer.snapshot.id.value;
                const auto document = editor_.documents_.find(id);
                const auto file = files_.find(id);
                const bool current =
                    document != editor_.documents_.end() &&
                    document->second.revision() == buffer.snapshot.revision &&
                    document->second.content_identity() == buffer.snapshot.content_identity &&
                    document->second.selections() == output->selections.at(id) &&
                    document->second.same_saved_state(output->prepared.at(id)) &&
                    (buffer.binding.has_value() == (file != files_.end())) &&
                    (!buffer.binding ||
                     (same_path(buffer.binding->path, file->second.path) &&
                      file->second.version.raw_content == output->versions.at(id)->raw_content &&
                      same_path(file->second.version.path, output->versions.at(id)->path)));
                if (!current || operation->cancellation.stop_requested() ||
                    events.size() >= EditorService::max_pending_notifications) {
                    pending_buffers.push_back(std::to_string(id));
                    continue;
                }
                auto metadata = describe(output->prepared.at(id));
                modified_buffers.push_back(metadata);
                metadata["reason"] = "workspaceReplace";
                events.push_back(
                    protocol::make_notification("document.changed", std::move(metadata)));
                publish.push_back(id);
            }
            Json response{
                {"applied", std::move(applied)},
                {"pending", std::move(pending)},
                {"modifiedBuffers", std::move(modified_buffers)},
                {"pendingBuffers", std::move(pending_buffers)},
                {"issues", issues_json(output->disk.issues)},
                {"cancelled", output->disk.cancelled || operation->cancellation.stop_requested()}};
            for (const auto id : publish) {
                editor_.documents_.at(id) = std::move(output->prepared.at(id));
            }
            editor_.notifications_.swap(events);
            return response;
        });
    if (operations_.at(result.at("operationId").get<std::string>())->state == "pending") {
        replacing_ = true;
    }
    return result;
}
} // namespace astra::service
