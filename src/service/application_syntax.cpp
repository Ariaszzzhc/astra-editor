#include <astra/service/application_service.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <utility>

namespace astra::service {
namespace {
using protocol::Json;
using protocol::RpcError;

void allowed_fields(const Json& params, std::initializer_list<std::string_view> fields) {
    if (!params.is_object()) {
        throw RpcError(-32602, "Named parameters required");
    }
    for (auto i = params.begin(); i != params.end(); ++i) {
        if (std::find(fields.begin(), fields.end(), i.key()) == fields.end()) {
            throw RpcError(-32602, "Unknown syntax parameter");
        }
    }
}

std::string_view highlight_name(language::HighlightKind kind) {
    static constexpr std::array names{"comment",
                                      "string",
                                      "number",
                                      "keyword",
                                      "type",
                                      "function",
                                      "property",
                                      "variable",
                                      "operator",
                                      "punctuation",
                                      "heading",
                                      "error"};
    return names.at(static_cast<std::size_t>(kind));
}

std::string_view outline_name(language::OutlineKind kind) {
    static constexpr std::array names{
        "function", "class", "struct", "enum", "namespace", "type", "property", "heading"};
    return names.at(static_cast<std::size_t>(kind));
}

std::size_t offset_field(const Json& params, const char* key, std::size_t fallback) {
    if (!params.contains(key)) {
        return fallback;
    }
    const auto& value = params[key];
    if (!value.is_number_integer() ||
        (!value.is_number_unsigned() && value.get<std::int64_t>() < 0) ||
        value.get<std::uint64_t>() > std::numeric_limits<std::size_t>::max()) {
        throw RpcError(-32602, "Syntax offsets must be nonnegative byte integers");
    }
    return value.get<std::size_t>();
}
} // namespace

Json ApplicationService::syntax_view(const Json& params) {
    allowed_fields(params,
                   {"id",
                    "baseRevision",
                    "begin",
                    "end",
                    "language",
                    "outlineBegin",
                    "outlineLimit",
                    "includeOutline"});
    const auto& document = editor_.find_document(params);
    EditorService::require_revision(document, params);
    const auto snapshot = document.snapshot();
    const auto begin = offset_field(params, "begin", 0);
    const auto end = offset_field(params, "end", snapshot.text.size_bytes());
    const auto outline_begin = offset_field(params, "outlineBegin", 0),
               outline_limit = offset_field(params, "outlineLimit", 1000);
    if (outline_limit > 1000 ||
        (params.contains("includeOutline") && !params.at("includeOutline").is_boolean()))
        throw RpcError(-32602, "Invalid syntax outline page");
    const auto include_outline = params.value("includeOutline", true);
    if (begin > end || end > snapshot.text.size_bytes() || !snapshot.text.is_boundary(begin) ||
        !snapshot.text.is_boundary(end)) {
        throw RpcError(-32602, "Invalid syntax viewport");
    }
    std::optional<language::LanguageId> language;
    if (params.contains("language")) {
        if (!params["language"].is_string()) {
            throw RpcError(-32602, "Language must be a string");
        }
        for (const auto candidate : {language::LanguageId::c,
                                     language::LanguageId::cpp,
                                     language::LanguageId::javascript,
                                     language::LanguageId::typescript,
                                     language::LanguageId::tsx,
                                     language::LanguageId::json,
                                     language::LanguageId::python,
                                     language::LanguageId::markdown}) {
            if (params["language"].get_ref<const std::string&>() ==
                language::language_name(candidate)) {
                language = candidate;
                break;
            }
        }
        if (!language) {
            throw RpcError(-32602, "Unsupported syntax language");
        }
    } else if (const auto file = files_.find(snapshot.id.value); file != files_.end()) {
        const auto bytes = file->second.path.generic_u8string();
        language = language::language_for_path(
            std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    }
    Json result{{"id", std::to_string(snapshot.id.value)},
                {"revision", std::to_string(snapshot.revision.value)}};
    if (!language) {
        result["state"] = "unsupported";
        return result;
    }
    result["language"] = language::language_name(*language);
    const auto cached = syntax_.find(snapshot.id.value);
    if (cached != syntax_.end() && cached->second.matches(snapshot) &&
        cached->second.language() == *language) {
        const auto& syntax = cached->second;
        result["state"] = "ready";
        result["hasErrors"] = syntax.has_errors();
        result["incremental"] = syntax.incremental();
        auto highlights = Json::array();
        for (const auto& span : syntax.highlights().intersecting({{begin}, {end}})) {
            if (highlights.size() == 10000) {
                result["highlightsTruncated"] = true;
                break;
            }
            highlights.push_back({{"begin", span.range.begin.value},
                                  {"end", span.range.end.value},
                                  {"kind", highlight_name(span.kind)}});
        }
        auto outline = Json::array();
        const auto total = syntax.outline_size(), first = std::min(outline_begin, total),
                   last = first + std::min(include_outline ? outline_limit : 0, total - first);
        result["outlinePage"] = {{"offset", first},
                                 {"total", total},
                                 {"nextOffset", last < total ? Json(last) : Json(nullptr)}};
        if (last - first < total)
            result["outlineTruncated"] = true;
        for (const auto& item : syntax.outline_range(first, last)) {
            outline.push_back({{"name", item.name},
                               {"kind", outline_name(item.kind)},
                               {"begin", item.range.begin.value},
                               {"end", item.range.end.value},
                               {"selectionBegin", item.selection.begin.value},
                               {"selectionEnd", item.selection.end.value},
                               {"depth", item.depth}});
        }
        result["highlights"] = std::move(highlights);
        result["outline"] = std::move(outline);
        return result;
    }
    if (const auto job = syntax_jobs_.find(snapshot.id.value); job != syntax_jobs_.end()) {
        if (const auto operation = operations_.find(job->second.operation);
            operation != operations_.end()) {
            if (job->second.revision == snapshot.revision && job->second.language == *language &&
                (operation->second->state == "pending" || operation->second->state == "failed")) {
                result["state"] = operation->second->state;
                result["operationId"] = operation->first;
                if (!operation->second->error.is_null()) {
                    result["error"] = operation->second->error;
                }
                return result;
            }
            operation->second->cancellation.request_stop();
        }
    }
    std::optional<language::SyntaxSnapshot> previous;
    if (cached != syntax_.end() && cached->second.language() == *language &&
        cached->second.revision() < snapshot.revision) {
        previous = cached->second;
    }
    auto parsed = std::make_shared<std::optional<language::SyntaxSnapshot>>();
    const auto started = start_operation(
        "syntax",
        [snapshot, language = *language, previous, parsed](const auto& operation) {
            language::SyntaxParser parser(language);
            language::ParseOptions options;
            options.stop = operation->cancellation.get_token();
            if (previous) {
                const auto extent = previous->text().changed_extent(snapshot.text);
                Transaction transaction{previous->revision(), {}, std::nullopt};
                if (extent.begin != extent.old_end || extent.begin != extent.new_end) {
                    transaction.edits.push_back(
                        {{extent.begin},
                         {extent.old_end},
                         snapshot.text.slice(extent.begin, extent.new_end)});
                }
                *parsed = parser.update(*previous,
                                        std::move(transaction),
                                        snapshot.revision,
                                        snapshot.content_identity,
                                        options);
            } else {
                *parsed = parser.parse(snapshot, options);
            }
        },
        [this, snapshot, language = *language, parsed](const auto& operation) {
            Json completed{{"id", std::to_string(snapshot.id.value)},
                           {"revision", std::to_string(snapshot.revision.value)},
                           {"language", language::language_name(language)}};
            const auto current = editor_.documents_.find(snapshot.id.value);
            const auto latest = syntax_jobs_.find(snapshot.id.value);
            if (current == editor_.documents_.end() ||
                !parsed->value().matches(current->second.snapshot()) ||
                (latest != syntax_jobs_.end() && latest->second.operation != operation->id)) {
                completed["state"] = "obsolete";
                return completed;
            }
            syntax_.insert_or_assign(snapshot.id.value, std::move(parsed->value()));
            completed["state"] = "ready";
            notifications_.push_back(
                protocol::make_notification("document.syntaxReady", completed));
            return completed;
        });
    syntax_jobs_.insert_or_assign(
        snapshot.id.value,
        SyntaxJob{snapshot.revision, *language, started["operationId"].get<std::string>()});
    result["state"] = "pending";
    result["operationId"] = started["operationId"];
    return result;
}

Json ApplicationService::select_object(const Json& params) {
    allowed_fields(params, {"id", "baseRevision", "kind", "inner", "expand"});
    auto& document = editor_.find_document(params);
    EditorService::require_revision(document, params);
    if (!params.contains("kind") || !params["kind"].is_string()) {
        throw RpcError(-32602, "Textobject kind is required");
    }
    std::optional<language::TextObjectKind> kind;
    static const std::map<std::string_view, language::TextObjectKind> kinds{
        {"function", language::TextObjectKind::function},
        {"class", language::TextObjectKind::class_},
        {"comment", language::TextObjectKind::comment},
        {"string", language::TextObjectKind::string},
        {"block", language::TextObjectKind::block},
        {"parameter", language::TextObjectKind::parameter},
        {"heading", language::TextObjectKind::heading}};
    const auto name = params["kind"].get<std::string>();
    if (const auto found = kinds.find(name); found != kinds.end()) {
        kind = found->second;
    }
    if (!kind) {
        throw RpcError(-32602, "Unknown textobject kind");
    }
    for (const auto flag : {"inner", "expand"}) {
        if (params.contains(flag) && !params[flag].is_boolean()) {
            throw RpcError(-32602, "Textobject flags must be boolean");
        }
    }
    const auto syntax = syntax_.find(document.id().value);
    if (syntax == syntax_.end() || !syntax->second.matches(document.snapshot())) {
        throw RpcError(-32014, "Request current syntax before selecting a textobject");
    }
    auto selections = document.selections();
    for (auto& selection : selections) {
        const auto range = syntax->second.enclosing_object({selection.begin(), selection.end()},
                                                           *kind,
                                                           params.value("inner", false),
                                                           params.value("expand", false));
        if (range) {
            selection = {range->begin, range->end};
        }
    }
    editor_.require_notification_capacity();
    const auto before = document.selections();
    document.set_selections(std::move(selections));
    if (before != document.selections()) {
        editor_.changed(document, "textObject");
    }
    return describe(document);
}
} // namespace astra::service
