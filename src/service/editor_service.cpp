#include <astra/service/editor_service.hpp>
#include <astra/editing/commands.hpp>

#include <algorithm>
#include <charconv>
#include <initializer_list>
#include <limits>
#include <utility>

namespace astra::service {
namespace {

using protocol::Json;
using protocol::RpcError;
constexpr std::uint64_t max_js_integer = 9007199254740991ULL;

[[noreturn]] void invalid_params(std::string_view detail) {
    throw RpcError(-32602, "Invalid params", Json{{"detail", detail}});
}

void object_params(const Json& params, std::initializer_list<std::string_view> allowed) {
    if (!params.is_object()) {
        invalid_params("Named object parameters are required");
    }
    for (auto field = params.begin(); field != params.end(); ++field) {
        if (std::find(allowed.begin(), allowed.end(), field.key()) == allowed.end()) {
            invalid_params("Unrecognized parameter");
        }
    }
}

const Json& required(const Json& params, std::string_view key) {
    const auto found = params.find(std::string(key));
    if (found == params.end()) {
        invalid_params("Missing required parameter");
    }
    return *found;
}

const std::string& string_value(const Json& value) {
    if (!value.is_string()) {
        invalid_params("String parameter required");
    }
    return value.get_ref<const std::string&>();
}

std::uint64_t decimal_id(const Json& value) {
    const auto& text = string_value(value);
    if (text.empty() || (text.size() != 1 && text.front() == '0')) {
        invalid_params("ID and revision require canonical unsigned decimal strings");
    }
    std::uint64_t number{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), number);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        invalid_params("ID or revision is outside the unsigned 64-bit range");
    }
    return number;
}

std::size_t byte_offset(const Json& value) {
    if (!value.is_number_integer() ||
        (!value.is_number_unsigned() && value.get<std::int64_t>() < 0)) {
        invalid_params("Byte offsets must be nonnegative integers");
    }
    const auto number = value.get<std::uint64_t>();
    if (number > max_js_integer || number > std::numeric_limits<std::size_t>::max()) {
        invalid_params("Byte offset exceeds the JavaScript-safe integer range");
    }
    return static_cast<std::size_t>(number);
}

std::vector<Selection> parse_selections(const Json& value) {
    if (!value.is_array() || value.empty() || value.size() > EditorService::max_selections) {
        invalid_params("Selections must contain between 1 and 4096 entries");
    }
    std::vector<Selection> result;
    result.reserve(value.size());
    for (const auto& entry : value) {
        object_params(entry, {"anchor", "head"});
        result.push_back({ByteOffset{byte_offset(required(entry, "anchor"))},
                          ByteOffset{byte_offset(required(entry, "head"))}});
    }
    return result;
}

std::vector<Edit> parse_edits(const Json& value) {
    if (!value.is_array() || value.size() > EditorService::max_transaction_edits) {
        invalid_params("Edits must be an array with at most 4096 entries");
    }
    std::vector<Edit> result;
    result.reserve(value.size());
    for (const auto& entry : value) {
        object_params(entry, {"begin", "end", "text"});
        result.push_back({ByteOffset{byte_offset(required(entry, "begin"))},
                          ByteOffset{byte_offset(required(entry, "end"))},
                          string_value(required(entry, "text"))});
    }
    return result;
}

template <typename Function>
Json document_errors(Function&& function) {
    try {
        return std::forward<Function>(function)();
    } catch (const StaleRevision&) {
        throw RpcError(-32001, "Stale document revision");
    } catch (const std::invalid_argument& error) {
        invalid_params(error.what());
    } catch (const std::out_of_range& error) {
        invalid_params(error.what());
    } catch (const std::length_error&) {
        throw RpcError(-32004, "Document capacity exceeded");
    } catch (const std::overflow_error&) {
        throw RpcError(-32004, "Document revision or identity capacity exceeded");
    }
}

} // namespace

EditorService::EditorService() {
    dispatcher_.register_method("document.list", [this](const Json& p) {
        return document_errors([&] { return list(p); });
    });
    dispatcher_.register_method("document.lines", [this](const Json& p) {
        return document_errors([&] { return lines(p); });
    });
    dispatcher_.register_method("document.command", [this](const Json& p) {
        return document_errors([&] { return command(p); });
    });
    dispatcher_.register_method("document.create", [this](const Json& p) {
        return document_errors([&] { return create(p); });
    });
    dispatcher_.register_method("document.get", [this](const Json& p) {
        return document_errors([&] { return get(p); });
    });
    dispatcher_.register_method("document.edit", [this](const Json& p) {
        return document_errors([&] { return edit(p); });
    });
    dispatcher_.register_method("document.insert", [this](const Json& p) {
        return document_errors([&] { return insert(p); });
    });
    dispatcher_.register_method("document.setSelections", [this](const Json& p) {
        return document_errors([&] { return set_selections(p); });
    });
    dispatcher_.register_method("document.undo", [this](const Json& p) {
        return document_errors([&] { return undo(p); });
    });
    dispatcher_.register_method("document.redo", [this](const Json& p) {
        return document_errors([&] { return redo(p); });
    });
    dispatcher_.register_method("document.close", [this](const Json& p) {
        return document_errors([&] { return close(p); });
    });
}

protocol::Dispatcher& EditorService::dispatcher() noexcept { return dispatcher_; }

const protocol::Dispatcher& EditorService::dispatcher() const noexcept { return dispatcher_; }

std::optional<Json> EditorService::dispatch(const Json& request) {
    return dispatcher_.dispatch(request);
}

std::optional<std::string> EditorService::dispatch_text(std::string_view body) {
    return dispatcher_.dispatch_text(body);
}

std::size_t EditorService::document_count() const noexcept { return documents_.size(); }

std::vector<Json> EditorService::take_notifications() {
    std::vector<Json> result;
    result.reserve(notifications_.size());
    while (!notifications_.empty()) {
        result.push_back(std::move(notifications_.front()));
        notifications_.pop_front();
    }
    return result;
}

void EditorService::require_notification_capacity() const {
    if (notifications_.size() >= max_pending_notifications) {
        throw RpcError(-32004, "Notification queue is full; drain events before editing");
    }
}

Json EditorService::metadata(const Document& document) {
    auto selections = Json::array();
    auto positions = Json::array();
    for (const auto& selection : document.selections()) {
        const auto position = document.text().byte_to_position(selection.head.value);
        selections.push_back({{"anchor", selection.anchor.value}, {"head", selection.head.value}});
        positions.push_back({{"line", position.line}, {"utf16Column", position.utf16_column}});
    }
    return Json{{"id", std::to_string(document.id().value)},
                {"revision", std::to_string(document.revision().value)},
                {"sizeBytes", document.text().size_bytes()},
                {"sizeUtf16", document.text().size_utf16()},
                {"lineCount", document.text().line_count()},
                {"selections", std::move(selections)},
                {"selectionPositions", std::move(positions)},
                {"dirty", document.dirty()}, {"canUndo", document.can_undo()},
                {"canRedo", document.can_redo()}};
}

void EditorService::changed(const Document& document, std::string_view reason) {
    auto params = metadata(document);
    params["reason"] = reason;
    notifications_.push_back(protocol::make_notification("document.changed", std::move(params)));
}

Document& EditorService::find_document(const Json& params) {
    const auto id = decimal_id(required(params, "id"));
    const auto found = documents_.find(id);
    if (found == documents_.end()) {
        throw RpcError(-32002, "Document not found", Json{{"id", std::to_string(id)}});
    }
    return found->second;
}

void EditorService::require_revision(const Document& document, const Json& params) {
    const auto revision = decimal_id(required(params, "baseRevision"));
    if (revision != document.revision().value) {
        throw RpcError(-32001, "Stale document revision",
                       Json{{"expected", std::to_string(document.revision().value)},
                            {"received", std::to_string(revision)}});
    }
}

Json EditorService::create(const Json& params) {
    object_params(params, {"text"});
    const std::string_view text = params.contains("text")
                                     ? std::string_view(string_value(params["text"]))
                                     : std::string_view{};
    if (next_id_ == std::numeric_limits<std::uint64_t>::max()) {
        throw RpcError(-32004, "Document ID capacity exceeded");
    }
    require_notification_capacity();
    Document document(BufferId{next_id_}, text);
    auto result = metadata(document);
    auto notification = protocol::make_notification("document.created", result);
    // Prepare the queue entry before inserting, so allocation failure cannot
    // leave a newly created document with no corresponding event.
    notifications_.push_back(std::move(notification));
    try {
        documents_.emplace(next_id_, std::move(document));
    } catch (...) {
        notifications_.pop_back();
        throw;
    }
    ++next_id_;
    return result;
}

Json EditorService::get(const Json& params) {
    object_params(params, {"id", "begin", "end"});
    const auto& document = find_document(params);
    const auto& text = document.text();
    const auto begin = params.contains("begin") ? byte_offset(params["begin"]) : 0;
    const auto requested_end = params.contains("end") ? byte_offset(params["end"])
                                                      : text.size_bytes();
    if (begin > requested_end || requested_end > text.size_bytes() ||
        !text.is_boundary(begin) || !text.is_boundary(requested_end)) {
        invalid_params("Read range is invalid or splits a UTF-8 scalar");
    }
    auto end = begin + std::min(max_read_bytes, requested_end - begin);
    while (!text.is_boundary(end)) {
        --end;
    }
    auto result = metadata(document);
    result["text"] = text.slice(begin, end);
    result["range"] = Json{{"begin", begin}, {"end", end}};
    result["full"] = begin == 0 && end == text.size_bytes();
    return result;
}

Json EditorService::list(const Json& params) {
    object_params(params, {});
    auto result = Json::array();
    for (const auto& [id, document] : documents_) {
        static_cast<void>(id);
        result.push_back(metadata(document));
    }
    return result;
}

Json EditorService::lines(const Json& params) {
    object_params(params, {"id", "firstLine", "count", "begin"});
    const auto& document = find_document(params);
    const auto& text = document.text();
    auto line = params.contains("firstLine") ? byte_offset(params["firstLine"]) : 0;
    const auto count = params.contains("count") ? byte_offset(params["count"]) : 100;
    if (line >= text.line_count() || count == 0 || count > 256) {
        invalid_params("Viewport requires a valid first line and 1 to 256 lines");
    }
    const auto end_line = line + std::min(count, text.line_count() - line);
    auto begin = params.contains("begin") ? byte_offset(params["begin"]) : text.line_start(line);
    if (begin < text.line_start(line) || begin > text.line_end(line) || !text.is_boundary(begin)) {
        invalid_params("Viewport continuation must lie inside its first line");
    }
    auto result = metadata(document);
    auto records = Json::array();
    std::size_t remaining = max_read_bytes;
    for (; line < end_line; ++line) {
        const auto content_end = text.line_end(line);
        auto end = begin + std::min(remaining, content_end - begin);
        while (!text.is_boundary(end)) { --end; }
        const auto next = line + 1 < text.line_count() ? text.line_start(line + 1) : text.size_bytes();
        records.push_back({{"line", line}, {"begin", begin}, {"end", end},
                           {"nextBegin", next}, {"text", text.slice(begin, end)},
                           {"complete", end == content_end}});
        remaining -= end - begin;
        if (end != content_end) {
            result["continuation"] = {{"firstLine", line}, {"begin", end}};
            break;
        }
        begin = next;
        if (remaining == 0) { ++line; break; }
    }
    result["lines"] = std::move(records);
    result["nextLine"] = line;
    return result;
}

Json EditorService::command(const Json& params) {
    object_params(params, {"id", "baseRevision", "command", "extend", "tabSize",
                           "insertSpaces", "commentPrefix", "newline", "text"});
    auto& document = find_document(params);
    require_revision(document, params);
    const auto& name = string_value(required(params, "command"));
    using editing::Command;
    static const std::map<std::string_view, Command> commands{
        {"move_left", Command::move_left}, {"move_right", Command::move_right},
        {"move_word_left", Command::move_word_left}, {"move_word_right", Command::move_word_right},
        {"move_up", Command::move_up}, {"move_down", Command::move_down},
        {"move_home", Command::move_home}, {"move_end", Command::move_end},
        {"move_document_start", Command::move_document_start}, {"move_document_end", Command::move_document_end},
        {"select_all", Command::select_all}, {"select_word", Command::select_word},
        {"select_next_occurrence", Command::select_next_occurrence},
        {"backspace", Command::backspace}, {"delete_forward", Command::delete_forward},
        {"delete_word_backward", Command::delete_word_backward}, {"delete_word_forward", Command::delete_word_forward},
        {"newline", Command::newline}, {"indent", Command::indent}, {"outdent", Command::outdent},
        {"toggle_line_comment", Command::toggle_line_comment}, {"duplicate_line", Command::duplicate_line},
        {"add_cursor_above", Command::add_cursor_above}, {"add_cursor_below", Command::add_cursor_below},
        {"insert_text", Command::insert_text}};
    const auto found = commands.find(name);
    if (found == commands.end()) { invalid_params("Unknown editing command"); }
    auto options = command_defaults_ ? command_defaults_(document) : editing::Options{};
    for (const auto field : {"extend", "insertSpaces"}) {
        if (params.contains(field) && !params[field].is_boolean()) {
            invalid_params("Command flags must be boolean");
        }
    }
    options.extend = params.value("extend", false);
    options.insert_spaces = params.value("insertSpaces", options.insert_spaces);
    if (params.contains("tabSize")) { options.tab_size = byte_offset(params["tabSize"]); }
    if (params.contains("commentPrefix")) { options.comment_prefix = string_value(params["commentPrefix"]); }
    if (params.contains("newline")) { options.newline = string_value(params["newline"]); }
    if (params.contains("text")) { options.text = string_value(params["text"]); }
    require_notification_capacity();
    if (editing::execute(document, found->second, options)) { changed(document, name); }
    return metadata(document);
}

Json EditorService::edit(const Json& params) {
    object_params(params, {"id", "baseRevision", "edits", "selectionsAfter"});
    auto& document = find_document(params);
    require_revision(document, params);
    Transaction transaction{document.revision(), parse_edits(required(params, "edits")),
                            std::nullopt};
    if (params.contains("selectionsAfter")) {
        transaction.selections_after = parse_selections(params["selectionsAfter"]);
    }
    require_notification_capacity();
    const auto before = document.selections();
    const bool text_changed = document.apply(std::move(transaction));
    if (text_changed || document.selections() != before) {
        changed(document, "edit");
    }
    return metadata(document);
}

Json EditorService::insert(const Json& params) {
    object_params(params, {"id", "baseRevision", "text"});
    auto& document = find_document(params);
    require_revision(document, params);
    const auto& text = string_value(required(params, "text"));
    require_notification_capacity();
    const auto before = document.selections();
    const bool text_changed = document.replace_selections(text);
    if (text_changed || document.selections() != before) {
        changed(document, "insert");
    }
    return metadata(document);
}

Json EditorService::set_selections(const Json& params) {
    object_params(params, {"id", "baseRevision", "selections"});
    auto& document = find_document(params);
    require_revision(document, params);
    auto selections = parse_selections(required(params, "selections"));
    require_notification_capacity();
    const auto before = document.selections();
    document.set_selections(std::move(selections));
    if (document.selections() != before) {
        changed(document, "selections");
    }
    return metadata(document);
}

Json EditorService::undo(const Json& params) {
    object_params(params, {"id", "baseRevision"});
    auto& document = find_document(params);
    require_revision(document, params);
    require_notification_capacity();
    if (document.undo()) {
        changed(document, "undo");
    }
    return metadata(document);
}

Json EditorService::redo(const Json& params) {
    object_params(params, {"id", "baseRevision"});
    auto& document = find_document(params);
    require_revision(document, params);
    require_notification_capacity();
    if (document.redo()) {
        changed(document, "redo");
    }
    return metadata(document);
}

Json EditorService::close(const Json& params) {
    object_params(params, {"id", "baseRevision", "discardChanges"});
    const auto& document = find_document(params);
    require_revision(document, params);
    bool discard = false;
    if (params.contains("discardChanges")) {
        if (!params["discardChanges"].is_boolean()) {
            invalid_params("discardChanges must be boolean");
        }
        discard = params["discardChanges"].get<bool>();
    }
    if (document.dirty() && !discard) {
        throw RpcError(-32003, "Document has unsaved changes");
    }
    require_notification_capacity();
    const auto id = document.id().value;
    Json result{{"id", std::to_string(id)}, {"closed", true}};
    auto event_params = metadata(document);
    notifications_.push_back(protocol::make_notification("document.closed", std::move(event_params)));
    documents_.erase(id);
    return result;
}

} // namespace astra::service
