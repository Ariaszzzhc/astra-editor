#include <astra/service/application_service.hpp>

#include <algorithm>
#include <cstdint>
#include <utility>

namespace astra::service {
namespace {
using protocol::Json;
using protocol::RpcError;

void fields(const Json& value, std::initializer_list<std::string_view> allowed) {
    if (!value.is_object()) { throw RpcError(-32602, "Named document search parameters required"); }
    for (auto i = value.begin(); i != value.end(); ++i) {
        if (std::find(allowed.begin(), allowed.end(), i.key()) == allowed.end()) {
            throw RpcError(-32602, "Unknown document search parameter");
        }
    }
}
std::string string_field(const Json& value, const char* key) {
    if (!value.contains(key) || !value[key].is_string()) { throw RpcError(-32602, std::string("Required string: ") + key); }
    return value[key].get<std::string>();
}
bool flag(const Json& value, const char* key, bool fallback = false) {
    if (!value.contains(key)) { return fallback; }
    if (!value[key].is_boolean()) { throw RpcError(-32602, std::string("Invalid flag: ") + key); }
    return value[key].get<bool>();
}
workspace::SearchOptions search_options(const Json& params) {
    workspace::SearchOptions options;
    options.query = string_field(params, "query");
    options.case_sensitive = flag(params, "caseSensitive", true);
    options.regex = flag(params, "regex"); options.whole_word = flag(params, "wholeWord");
    options.max_file_bytes = workspace::SearchPattern::max_input_bytes;
    options.max_matches = EditorService::max_transaction_edits;
    return options;
}
void require_current(const Document& document, const DocumentSnapshot& snapshot, bool selections) {
    if (document.revision() != snapshot.revision || document.content_identity() != snapshot.content_identity ||
        (selections && document.selections() != snapshot.selections)) {
        throw RpcError(-32001, "Document changed during search; search again",
            Json{{"id", std::to_string(snapshot.id.value)}, {"revision", std::to_string(document.revision().value)}});
    }
}
void require_not_cancelled(std::stop_token stop) {
    if (stop.stop_requested()) { throw RpcError(-32013, "Document search cancelled before publishing"); }
}
} // namespace

Json ApplicationService::find_document_text(const Json& params) {
    fields(params, {"id", "baseRevision", "query", "begin", "wrap", "select", "direction", "caseSensitive", "regex", "wholeWord"});
    auto& document = editor_.find_document(params);
    EditorService::require_revision(document, params);
    const auto snapshot = document.snapshot();
    const auto options = search_options(params);
    const auto direction=params.contains("direction") ? string_field(params,"direction") : "forward";
    if (direction!="forward" && direction!="backward") { throw RpcError(-32602,"Find direction must be forward or backward"); }
    const bool backwards=direction=="backward";
    auto begin = backwards ? snapshot.selections.front().begin() : snapshot.selections.front().head;
    if (params.contains("begin")) {
        const auto& value = params.at("begin");
        if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<std::int64_t>() < 0) ||
            value.get<std::uint64_t>() > snapshot.text.size_bytes()) {
            throw RpcError(-32602, "Find begin must be a UTF-8 byte offset in the document");
        }
        begin.value = value.get<std::size_t>();
    }
    if (!snapshot.text.is_boundary(begin.value)) { throw RpcError(-32602, "Find begin splits a Unicode scalar"); }
    const auto wrap = flag(params, "wrap", true), select = flag(params, "select");
    struct Output { workspace::TextFindResult found; Json match; };
    const auto output = std::make_shared<Output>();
    return start_operation("documentFind", [snapshot, options, begin, wrap, backwards, output](const auto& operation) {
        const auto stop = operation->cancellation.get_token();
        require_not_cancelled(stop);
        const workspace::SearchPattern pattern(options);
        const auto original=snapshot.text.str();
        output->found = backwards ? pattern.find_previous(original,begin,wrap,stop) : pattern.find_next(original,begin,wrap,stop);
        require_not_cancelled(stop);
        if (output->found.truncated) { throw RpcError(-32004, "Find exceeded its search work budget"); }
        if (const auto& match = output->found.match) {
            const auto position = snapshot.text.byte_to_position(match->begin.value);
            output->match = {{"begin", match->begin.value}, {"end", match->end.value},
                {"line", position.line}, {"utf16Column", position.utf16_column}};
        }
    }, [this, snapshot, select, output](const auto& operation) {
        require_not_cancelled(operation->cancellation.get_token());
        const auto found = editor_.documents_.find(snapshot.id.value);
        if (found == editor_.documents_.end()) { throw RpcError(-32002, "Search document was closed"); }
        require_current(found->second, snapshot, select);
        Json result{{"id", std::to_string(snapshot.id.value)}, {"revision", std::to_string(snapshot.revision.value)},
            {"match", output->match}, {"wrapped", output->found.wrapped}};
        if (select && output->found.match) {
            const auto& match = *output->found.match;
            std::vector<Selection> selections{{match.begin, match.end}};
            auto metadata = describe(found->second);
            metadata["selections"] = Json::array({{{"anchor", match.begin.value}, {"head", match.end.value}}});
            const auto position = snapshot.text.byte_to_position(match.end.value);
            metadata["selectionPositions"] = Json::array({{{"line", position.line}, {"utf16Column", position.utf16_column}}});
            result["document"] = metadata;
            if (selections != found->second.selections()) {
                editor_.require_notification_capacity();
                auto events = editor_.notifications_;
                metadata["reason"] = "find";
                events.push_back(protocol::make_notification("document.changed", std::move(metadata)));
                // Selection normalization has the strong exception guarantee;
                // no history copy is needed for an ordinary find operation.
                found->second.set_selections(std::move(selections)); editor_.notifications_.swap(events);
            }
        }
        return result;
    });
}

Json ApplicationService::replace_document_text(const Json& params) {
    fields(params, {"id", "baseRevision", "query", "replacement", "caseSensitive", "regex", "wholeWord"});
    const auto& document = editor_.find_document(params);
    EditorService::require_revision(document, params);
    const auto snapshot = document.snapshot();
    const auto options = search_options(params);
    const auto replacement = string_field(params, "replacement");
    struct Output {
        Document prepared;
        std::size_t replacements{};
        bool changed{};
    };
    const auto output = std::make_shared<Output>(Output{document.clone()});
    return start_operation("documentReplace", [snapshot, options, replacement, output](const auto& operation) {
        const auto stop = operation->cancellation.get_token();
        require_not_cancelled(stop);
        const workspace::SearchPattern pattern(options);
        pattern.validate_replacement(replacement);
        const auto original = snapshot.text.str();
        const auto found = pattern.find_all(original, stop);
        require_not_cancelled(stop);
        if (found.truncated) { throw RpcError(-32004, "Refine document replacement to at most 4096 matches within the metadata budget"); }
        std::vector<Edit> edits;
        edits.reserve(found.matches.size());
        auto size = original.size();
        for (const auto& match : found.matches) {
            require_not_cancelled(stop);
            size -= match.end.value - match.begin.value;
            auto text = pattern.expand_replacement(original, match, replacement, workspace::SearchPattern::max_output_bytes - size);
            size += text.size();
            if (!edits.empty() && edits.back().end == match.begin &&
                (edits.back().begin == edits.back().end || match.begin == match.end)) {
                edits.back().end = match.end; edits.back().text += text;
            } else { edits.push_back({match.begin, match.end, std::move(text)}); }
        }
        output->replacements = found.matches.size();
        output->changed = output->prepared.apply({snapshot.revision, std::move(edits), std::nullopt});
    }, [this, snapshot, output](const auto& operation) {
        require_not_cancelled(operation->cancellation.get_token());
        const auto found = editor_.documents_.find(snapshot.id.value);
        if (found == editor_.documents_.end()) { throw RpcError(-32002, "Replacement document was closed"); }
        require_current(found->second, snapshot, true);
        if (!found->second.same_saved_state(output->prepared)) {
            throw RpcError(-32001, "Saved state changed during replacement; search again");
        }
        auto metadata = describe(output->prepared);
        Json result{{"document", metadata}, {"replacements", output->replacements}, {"changed", output->changed}};
        if (output->changed) {
            editor_.require_notification_capacity();
            auto events = editor_.notifications_;
            metadata["reason"] = "replaceAll";
            events.push_back(protocol::make_notification("document.changed", std::move(metadata)));
            found->second = std::move(output->prepared); editor_.notifications_.swap(events);
        }
        return result;
    });
}
} // namespace astra::service
