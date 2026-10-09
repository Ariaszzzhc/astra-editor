#include <astra/language/syntax.hpp>
#include <astra/core/utf8.hpp>

#include <tree_sitter/api.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <initializer_list>
#include <limits>
#include <mutex>
#include <set>
#include <map>
#include <tuple>
#include <utility>

extern "C" {
const TSLanguage* tree_sitter_c();
const TSLanguage* tree_sitter_cpp();
const TSLanguage* tree_sitter_javascript();
const TSLanguage* tree_sitter_typescript();
const TSLanguage* tree_sitter_tsx();
const TSLanguage* tree_sitter_json();
const TSLanguage* tree_sitter_python();
const TSLanguage* tree_sitter_markdown();
const TSLanguage* tree_sitter_markdown_inline();
}

namespace astra::language {
namespace {

struct DeleteTree { void operator()(TSTree* tree) const noexcept { ts_tree_delete(tree); } };
struct DeleteParser { void operator()(TSParser* parser) const noexcept { ts_parser_delete(parser); } };
using Tree = std::unique_ptr<TSTree, DeleteTree>;
using Parser = std::unique_ptr<TSParser, DeleteParser>;

[[nodiscard]] std::uint32_t narrow(std::size_t value) {
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("Tree-sitter coordinates exceed 32-bit range");
    }
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] const TSLanguage* grammar(LanguageId language) {
    switch (language) {
    case LanguageId::c: return tree_sitter_c();
    case LanguageId::cpp: return tree_sitter_cpp();
    case LanguageId::javascript: return tree_sitter_javascript();
    case LanguageId::typescript: return tree_sitter_typescript();
    case LanguageId::tsx: return tree_sitter_tsx();
    case LanguageId::json: return tree_sitter_json();
    case LanguageId::python: return tree_sitter_python();
    case LanguageId::markdown: return tree_sitter_markdown();
    }
    throw std::invalid_argument("unknown syntax language");
}

[[nodiscard]] Parser make_parser(const TSLanguage* language) {
    Parser parser(ts_parser_new());
    if (!parser) { throw std::bad_alloc(); }
    if (!ts_parser_set_language(parser.get(), language)) {
        throw std::runtime_error("Tree-sitter grammar ABI does not match the runtime");
    }
    return parser;
}

struct Budget {
    const ParseOptions& options;
    std::chrono::steady_clock::time_point deadline;
    std::size_t nodes{};

    explicit Budget(const ParseOptions& input) : options(input) {
        if (options.timeout <= std::chrono::milliseconds::zero() ||
            options.timeout > std::chrono::hours(24)) {
            throw std::invalid_argument("syntax timeout must be positive and at most 24 hours");
        }
        deadline = std::chrono::steady_clock::now() + options.timeout;
        check();
    }
    [[nodiscard]] bool expired() const noexcept {
        return options.stop.stop_requested() || std::chrono::steady_clock::now() >= deadline;
    }
    void check() const { if (expired()) { throw ParseCancelled(); } }
    void visit() {
        if (++nodes > options.max_nodes) { throw std::length_error("syntax node limit exceeded"); }
        if (nodes % 256 == 0) { check(); }
    }
};

struct Reader {
    const Rope& text;
    Budget& budget;
    std::string buffer;
    std::exception_ptr failure;

    static const char* read(void* payload, std::uint32_t offset, TSPoint,
                            std::uint32_t* count) noexcept {
        auto& self = *static_cast<Reader*>(payload);
        *count = 0;
        try {
            self.budget.check();
            if (offset >= self.text.size_bytes()) { return nullptr; }
            // Tree-sitter may seek to any byte. Rope slices use scalar boundaries;
            // retain up to three leading bytes and return a pointer past them.
            auto begin = static_cast<std::size_t>(offset);
            while (!self.text.is_boundary(begin)) { --begin; }
            auto end = std::min(self.text.size_bytes(), begin + 4096);
            while (!self.text.is_boundary(end)) { --end; }
            self.buffer = self.text.slice(begin, end);
            *count = narrow(end - offset);
            return self.buffer.data() + (offset - begin);
        } catch (...) {
            self.failure = std::current_exception();
            return nullptr;
        }
    }
};

bool progress(TSParseState* state) noexcept {
    return static_cast<Budget*>(state->payload)->expired();
}

[[nodiscard]] Tree parse_tree(TSParser* parser, const Rope& text,
                              const TSTree* previous, Budget& budget) {
    budget.check();
    Reader reader{text, budget, {}, {}};
    const TSInput input{&reader, &Reader::read, TSInputEncodingUTF8, nullptr};
    const TSParseOptions options{&budget, &progress};
    Tree result(ts_parser_parse_with_options(parser, previous, input, options));
    // A cancelled parser retains partial state. Always reset before another task
    // so a later full parse cannot resume work from an unrelated document.
    ts_parser_reset(parser);
    if (reader.failure) { std::rethrow_exception(reader.failure); }
    budget.check();
    if (!result) { throw std::runtime_error("Tree-sitter did not return a syntax tree"); }
    return result;
}

[[nodiscard]] TSPoint point(const Rope& text, std::size_t offset) {
    const auto position = text.byte_to_position(offset);
    return {narrow(position.line), narrow(offset - text.line_start(position.line))};
}

[[nodiscard]] TSPoint advanced_point(TSPoint start, std::string_view replacement) {
    const auto summary = MeasureText{}(replacement);
    return {narrow(static_cast<std::size_t>(start.row) + summary.line_breaks),
        narrow(summary.line_breaks == 0 ? start.column + replacement.size() : summary.trailing_bytes)};
}

[[nodiscard]] SyntaxRange range(TSNode node) {
    return {{ts_node_start_byte(node)}, {ts_node_end_byte(node)}};
}

[[nodiscard]] TSRange ts_range(TSNode node) {
    return {ts_node_start_point(node), ts_node_end_point(node),
            ts_node_start_byte(node), ts_node_end_byte(node)};
}

[[nodiscard]] TSNode field(TSNode node, std::string_view name) {
    return ts_node_child_by_field_name(node, name.data(), narrow(name.size()));
}

[[nodiscard]] bool one_of(std::string_view value, std::initializer_list<std::string_view> set) {
    return std::ranges::find(set, value) != set.end();
}

[[nodiscard]] bool string_node(std::string_view type) {
    return one_of(type, {"string", "string_literal", "char_literal", "character_literal",
        "raw_string_literal", "template_string", "string_content", "string_fragment",
        "escape_sequence", "system_lib_string", "code_span", "link_destination",
        "uri_autolink", "email_autolink"});
}

[[nodiscard]] bool comment_node(std::string_view type) { return type.find("comment") != type.npos; }

[[nodiscard]] std::optional<HighlightKind> color(TSNode node, const Rope& text) {
    const std::string_view type(ts_node_type(node));
    if (ts_node_is_error(node) || ts_node_is_missing(node)) { return HighlightKind::error; }
    if (comment_node(type)) { return HighlightKind::comment; }
    if (string_node(type)) {
        const auto parent = ts_node_parent(node);
        if (!ts_node_is_null(parent) && std::string_view(ts_node_type(parent)) == "pair" &&
            ts_node_eq(field(parent, "key"), node)) { return HighlightKind::property; }
        // JSON key content inherits the property's scope instead of overriding
        // it with the generic string-content scope.
        if (!ts_node_is_null(parent) && std::string_view(ts_node_type(parent)) == "string") {
            const auto pair = ts_node_parent(parent);
            if (!ts_node_is_null(pair) && std::string_view(ts_node_type(pair)) == "pair" &&
                ts_node_eq(field(pair, "key"), parent)) { return std::nullopt; }
        }
        return HighlightKind::string;
    }
    if (one_of(type, {"number", "number_literal", "integer", "float", "float_literal", "integer_literal"})) {
        return HighlightKind::number;
    }
    if (one_of(type, {"true", "false", "null", "none", "True", "False", "None"})) {
        return HighlightKind::keyword;
    }
    if (one_of(type, {"primitive_type", "type_identifier", "predefined_type", "sized_type_specifier"})) {
        return HighlightKind::type;
    }
    if (one_of(type, {"atx_heading", "setext_heading"})) { return HighlightKind::heading; }
    if (one_of(type, {"property_identifier", "field_identifier", "shorthand_property_identifier_pattern"})) {
        return HighlightKind::property;
    }
    if (type == "identifier") {
        const auto parent = ts_node_parent(node);
        const std::string_view parent_type(ts_node_type(parent));
        const auto function = field(parent, "function");
        const auto name = field(parent, "name");
        if ((!ts_node_is_null(function) && ts_node_eq(function, node)) ||
            parent_type == "function_declarator" ||
            (one_of(parent_type, {"function_definition", "function_declaration", "method_definition",
                                  "generator_function_declaration"}) && ts_node_eq(name, node))) {
            return HighlightKind::function;
        }
        if (one_of(parent_type, {"class_definition", "class_declaration", "interface_declaration",
                                "type_alias_declaration", "enum_declaration"}) && ts_node_eq(name, node)) {
            return HighlightKind::type;
        }
        return HighlightKind::variable;
    }
    if (!ts_node_is_named(node) && ts_node_child_count(node) == 0) {
        const auto token_range = range(node);
        if (token_range.end.value - token_range.begin.value > 64) { return std::nullopt; }
        const auto token = text.slice(token_range.begin.value, token_range.end.value);
        if (token.empty()) { return std::nullopt; }
        const auto parent = ts_node_parent(node);
        if (!ts_node_is_null(parent) && (string_node(ts_node_type(parent)) || comment_node(ts_node_type(parent)))) {
            return std::nullopt;
        }
        const auto first = token.front();
        if ((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') || first == '#') {
            return HighlightKind::keyword;
        }
        if (one_of(token, {"+", "-", "*", "/", "%", "=", "==", "===", "!=", "!==", "<", ">",
            "<=", ">=", "!", "&&", "||", "&", "|", "^", "~", "++", "--", "+=", "-=", "=>", "->", "?", "??", "**"})) {
            return HighlightKind::operator_;
        }
        return HighlightKind::punctuation;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<OutlineKind> outline_kind(std::string_view type) {
    if (one_of(type, {"function_definition", "function_declaration", "generator_function_declaration", "method_definition"})) {
        return OutlineKind::function;
    }
    if (one_of(type, {"class_definition", "class_declaration", "class_specifier", "interface_declaration"})) {
        return OutlineKind::class_;
    }
    if (one_of(type, {"struct_specifier", "union_specifier"})) { return OutlineKind::struct_; }
    if (one_of(type, {"enum_specifier", "enum_declaration"})) { return OutlineKind::enum_; }
    if (one_of(type, {"namespace_definition", "internal_module"})) { return OutlineKind::namespace_; }
    if (one_of(type, {"type_alias_declaration", "type_definition"})) { return OutlineKind::type; }
    if (type == "pair") { return OutlineKind::property; }
    if (one_of(type, {"atx_heading", "setext_heading"})) { return OutlineKind::heading; }
    return std::nullopt;
}

[[nodiscard]] TSNode node_name(TSNode node) {
    auto name = field(node, "name");
    if (!ts_node_is_null(name)) { return name; }
    name = field(node, "key");
    if (!ts_node_is_null(name)) { return name; }
    name = field(node, "declarator");
    for (std::size_t depth = 0; depth < 128 && !ts_node_is_null(name); ++depth) {
        const auto nested = field(name, "declarator");
        if (ts_node_is_null(nested)) { return name; }
        name = nested;
    }
    const std::string_view type(ts_node_type(node));
    if (one_of(type, {"atx_heading", "setext_heading"})) {
        name = field(node, "heading_content");
        if (!ts_node_is_null(name)) { return name; }
        for (std::uint32_t index = 0; index < ts_node_named_child_count(node); ++index) {
            const auto child = ts_node_named_child(node, index);
            if (std::string_view(ts_node_type(child)) == "inline") { return child; }
        }
    }
    return TSNode{};
}

[[nodiscard]] SyntaxRange without_delimiters(SyntaxRange original, const Rope& text) {
    const auto size = original.end.value - original.begin.value;
    if (size < 2) { return original; }
    auto prefix_end = original.begin.value + std::min(size, std::size_t{3});
    while (!text.is_boundary(prefix_end)) { --prefix_end; }
    if (prefix_end == original.begin.value) { return original; }
    const auto prefix = text.slice(original.begin.value, prefix_end);
    const auto char_at = [&text](std::size_t position, std::string_view expected) {
        return text.is_boundary(position) && text.is_boundary(position + expected.size()) &&
            text.slice(position, position + expected.size()) == expected;
    };
    for (const auto quote : {std::string_view("\"\"\""), std::string_view("'''"), std::string_view("/*")}) {
        const auto ending = quote == "/*" ? std::string_view("*/") : quote;
        if (size >= quote.size() + ending.size() && prefix.starts_with(quote) &&
            char_at(original.end.value - ending.size(), ending)) {
            return {{original.begin.value + quote.size()}, {original.end.value - ending.size()}};
        }
    }
    if (prefix.starts_with("//") || prefix.starts_with("#")) {
        auto begin = original.begin.value + (prefix.starts_with("//") ? 2 : 1);
        if (begin < original.end.value && char_at(begin, " ")) { ++begin; }
        return {{begin}, original.end};
    }
    const auto first = prefix.front();
    char last = 0;
    switch (first) {
    case '{': last = '}'; break;
    case '[': last = ']'; break;
    case '(': last = ')'; break;
    case '\'': case '"': case '`': last = first; break;
    default: return original;
    }
    const std::string expected(1, last);
    if (char_at(original.end.value - 1, expected)) {
        return {{original.begin.value + 1}, {original.end.value - 1}};
    }
    return original;
}

[[nodiscard]] std::optional<TextObjectKind> object_kind(TSNode node) {
    const std::string_view type(ts_node_type(node));
    const auto outline = outline_kind(type);
    if (outline == OutlineKind::function) { return TextObjectKind::function; }
    if (outline == OutlineKind::class_ || outline == OutlineKind::struct_) { return TextObjectKind::class_; }
    if (outline == OutlineKind::heading) { return TextObjectKind::heading; }
    if (comment_node(type)) { return TextObjectKind::comment; }
    if (string_node(type) && !one_of(type, {"string_content", "string_fragment", "escape_sequence"})) {
        return TextObjectKind::string;
    }
    if (one_of(type, {"compound_statement", "statement_block", "block", "object", "array", "list", "dictionary"})) {
        return TextObjectKind::block;
    }
    if (one_of(type, {"parameter_declaration", "required_parameter", "optional_parameter", "default_parameter",
                      "typed_parameter", "typed_default_parameter"})) { return TextObjectKind::parameter; }
    const auto parent = ts_node_parent(node);
    if (ts_node_is_named(node) && !ts_node_is_null(parent) &&
        one_of(ts_node_type(parent), {"parameters", "formal_parameters", "parameter_list"}) && type == "identifier") {
        return TextObjectKind::parameter;
    }
    return std::nullopt;
}

struct Capture { SyntaxRange range; HighlightKind kind; std::size_t priority; };
struct Analysis {
    std::vector<Capture> captures;
    std::vector<OutlineItem> outline;
    std::vector<TextObject> objects;
    std::vector<std::vector<TSRange>> inline_groups;
};

class Cursor {
public:
    explicit Cursor(TSNode root) : value_(ts_tree_cursor_new(root)) {}
    ~Cursor() { ts_tree_cursor_delete(&value_); }
    Cursor(const Cursor&) = delete;
    Cursor& operator=(const Cursor&) = delete;
    [[nodiscard]] TSTreeCursor* get() noexcept { return &value_; }
private:
    TSTreeCursor value_;
};

void add_inline_ranges(TSNode node, Analysis& analysis) {
    auto current = ts_range(node);
    std::vector<TSRange> ranges;
    for (std::uint32_t index = 0; index < ts_node_child_count(node); ++index) {
        const auto child = ts_node_child(node, index);
        // Anonymous punctuation belongs to inline syntax (notably backticks).
        // Only block-continuation children must be excluded from inline input.
        if (!ts_node_is_named(child)) { continue; }
        const auto excluded = ts_range(child);
        if (current.start_byte < excluded.start_byte) {
            ranges.push_back({current.start_point, excluded.start_point,
                current.start_byte, excluded.start_byte});
        }
        current.start_byte = excluded.end_byte;
        current.start_point = excluded.end_point;
    }
    if (current.start_byte < current.end_byte) { ranges.push_back(current); }
    if (!ranges.empty()) { analysis.inline_groups.push_back(std::move(ranges)); }
}

void analyze_node(TSNode root, const Rope& text, Analysis& output, Budget& budget,
             bool markdown_inline = false, bool shallow = false) {
    Cursor cursor(root);
    std::vector<std::size_t> outline_depths;
    bool more = true;
    while (more) {
        budget.visit();
        const auto node = ts_tree_cursor_current_node(cursor.get());
        const auto depth = static_cast<std::size_t>(ts_tree_cursor_current_depth(cursor.get()));
        while (!outline_depths.empty() && outline_depths.back() >= depth) { outline_depths.pop_back(); }
        const auto node_range = range(node);
        const std::string_view type(ts_node_type(node));
        if (const auto kind = color(node, text); kind && node_range.begin < node_range.end) {
            if (output.captures.size() >= budget.options.max_spans) { throw std::length_error("syntax span limit exceeded"); }
            output.captures.push_back({node_range, *kind, depth + (markdown_inline ? 1'000'000 : 0)});
        }
        if (const auto kind = outline_kind(type); kind && !markdown_inline) {
            const auto name = node_name(node);
            if (!ts_node_is_null(name)) {
                if (output.outline.size() >= budget.options.max_outline_items) { throw std::length_error("syntax outline limit exceeded"); }
                const auto name_range = range(name);
                auto end = std::min(name_range.end.value, name_range.begin.value + 512);
                while (!text.is_boundary(end)) { --end; }
                auto item_depth = outline_depths.size();
                if (*kind == OutlineKind::heading) {
                    const auto marker = ts_node_child(node, 0);
                    const std::string_view marker_type(ts_node_type(marker));
                    if (marker_type.starts_with("atx_h") && marker_type.size() > 5 &&
                        marker_type[5] >= '1' && marker_type[5] <= '6') {
                        item_depth = static_cast<std::size_t>(marker_type[5] - '1');
                    } else { item_depth = marker_type == "setext_h2_underline" ? 1 : 0; }
                }
                output.outline.push_back({text.slice(name_range.begin.value, end), *kind, node_range, name_range, item_depth});
                outline_depths.push_back(depth);
            }
        }
        if (const auto kind = object_kind(node)) {
            if (output.objects.size() >= budget.options.max_text_objects) { throw std::length_error("syntax text-object limit exceeded"); }
            auto inner = without_delimiters(node_range, text);
            if (const auto body = field(node, "body"); !ts_node_is_null(body)) {
                inner = without_delimiters(range(body), text);
            }
            output.objects.push_back({*kind, node_range, inner});
        }
        if (!markdown_inline && one_of(type, {"inline", "pipe_table_cell"})) { add_inline_ranges(node, output); }
        if (shallow) break;
        if (ts_tree_cursor_goto_first_child(cursor.get())) { continue; }
        while (!ts_tree_cursor_goto_next_sibling(cursor.get())) {
            if (!ts_tree_cursor_goto_parent(cursor.get())) { more = false; break; }
        }
    }
}
void analyze(const TSTree* tree,const Rope& text,Analysis& output,Budget& budget,bool markdown_inline=false) {
    analyze_node(ts_tree_root_node(tree),text,output,budget,markdown_inline);
}

[[nodiscard]] HighlightIndex index_captures(std::vector<Capture> captures,
                                           std::size_t bytes, Budget& budget) {
    struct Event { std::size_t offset; std::size_t capture; bool start; };
    std::vector<Event> events;
    events.reserve(captures.size() * 2);
    for (std::size_t index = 0; index < captures.size(); ++index) {
        events.push_back({captures[index].range.begin.value, index, true});
        events.push_back({captures[index].range.end.value, index, false});
    }
    std::ranges::sort(events, {}, &Event::offset);
    std::set<std::pair<std::size_t, std::size_t>> active;
    std::vector<HighlightRun> runs;
    const auto append = [&runs, &budget](std::size_t length, std::optional<HighlightKind> kind) {
        if (length == 0) { return; }
        if (!runs.empty() && runs.back().kind == kind) { runs.back().bytes += length; }
        else {
            if (runs.size() >= budget.options.max_spans) { throw std::length_error("syntax index limit exceeded"); }
            runs.push_back({length, kind});
        }
    };
    std::size_t previous = 0;
    std::size_t index = 0;
    while (index < events.size()) {
        if (index % 256 == 0) { budget.check(); }
        const auto offset = events[index].offset;
        append(offset - previous, active.empty() ? std::nullopt :
            std::optional(captures[active.rbegin()->second].kind));
        while (index < events.size() && events[index].offset == offset) {
            const auto& event = events[index++];
            const auto key = std::pair(captures[event.capture].priority, event.capture);
            if (event.start) { active.insert(key); } else { active.erase(key); }
        }
        previous = offset;
    }
    append(bytes - previous, std::nullopt);
    budget.check();
    return HighlightIndex(std::move(runs));
}

struct SemanticUnit;
struct SemanticSummary {
    std::size_t bytes{},outlines{},objects{},nodes{},captures{};
    friend bool operator==(const SemanticSummary&,const SemanticSummary&) = default;
    friend SemanticSummary operator+(SemanticSummary left,SemanticSummary right) {
        const auto add=[](std::size_t a,std::size_t b){
            if(b>std::numeric_limits<std::size_t>::max()-a)throw std::length_error("Semantic summary overflow");
            return a+b;
        };
        return {add(left.bytes,right.bytes),add(left.outlines,right.outlines),add(left.objects,right.objects),add(left.nodes,right.nodes),add(left.captures,right.captures)};
    }
};
struct SemanticPiece { std::size_t bytes{};std::shared_ptr<const SemanticUnit> unit; };
struct MeasureSemantic { SemanticSummary operator()(const SemanticPiece& piece) const; };
using SemanticPieces=SumTree<SemanticPiece,SemanticSummary,MeasureSemantic>;
// IDs are looked up only while the previous TSTree is retained. Parent-derived
// classification and inherited color are part of the key; byte origins are not.
using SemanticKey=std::tuple<std::uintptr_t,std::uint16_t,int,int,int,std::size_t,std::string>;
struct SemanticEntry { std::shared_ptr<const SemanticUnit> unit;std::size_t origin{}; };
using SemanticCache=std::map<SemanticKey,SemanticEntry>;
using GapCache=std::map<std::pair<std::size_t,std::optional<HighlightKind>>,HighlightIndex>;
using ChildStamp=std::tuple<std::uintptr_t,std::uint16_t,std::uint16_t,std::uint32_t,std::size_t,std::size_t,std::string>;
std::vector<ChildStamp> child_stamps(TSNode node) {
    std::vector<ChildStamp> result;result.reserve(ts_node_child_count(node));
    const auto origin=ts_node_start_byte(node);
    for(std::uint32_t index=0;index<ts_node_child_count(node);++index){
        const auto child=ts_node_child(node,index);
        const auto name=ts_node_field_name_for_child(node,index);
        result.emplace_back(reinterpret_cast<std::uintptr_t>(child.id),ts_node_symbol(child),ts_node_grammar_symbol(child),child.context[3],
            ts_node_start_byte(child)-origin,ts_node_end_byte(child)-origin,name ? name : "");
    }
    return result;
}
struct SemanticUnit {
    SemanticKey key;
    SemanticSummary summary;
    HighlightIndex highlights;
    std::vector<OutlineItem> outline;
    std::vector<TextObject> objects;
    SemanticPieces children;
    bool errors{};
    std::vector<ChildStamp> stamps;
};
SemanticSummary MeasureSemantic::operator()(const SemanticPiece& piece) const {
    auto result=piece.unit ? piece.unit->summary : SemanticSummary{};
    result.bytes=piece.bytes;return result;
}
SyntaxRange relative(SyntaxRange value,std::size_t origin) {
    return {{value.begin.value-origin},{value.end.value-origin}};
}
SyntaxRange absolute(SyntaxRange value,std::size_t origin) {
    return {{value.begin.value+origin},{value.end.value+origin}};
}
void semantic_limits(const SemanticUnit& unit,const ParseOptions& options) {
    if(unit.summary.nodes>options.max_nodes)throw std::length_error("syntax node limit exceeded");
    if(unit.summary.captures>options.max_spans || unit.highlights.run_count()>options.max_spans)throw std::length_error("syntax span limit exceeded");
    if(unit.summary.outlines>options.max_outline_items)throw std::length_error("syntax outline limit exceeded");
    if(unit.summary.objects>options.max_text_objects)throw std::length_error("syntax text-object limit exceeded");
}
std::shared_ptr<const SemanticUnit> build_semantic(TSNode node,const Rope& text,
    std::optional<HighlightKind> inherited,std::size_t depth,const SemanticCache* previous,
    const RopeChangedExtent* changed,SemanticCache& built,const GapCache* previous_gaps,GapCache& gaps,Budget& budget,AnalysisStatistics& statistics) {
    budget.check();
    const auto node_range=range(node);
    const auto bytes=node_range.end.value-node_range.begin.value;
    const auto own_color=color(node,text);
    const auto own_object=object_kind(node);
    const auto code=[](const auto& value){return value ? static_cast<int>(*value) : -1;};
    // A node's slot can move when a sibling changes. The first visible child
    // selects a candidate; child stamps and an unchanged Rope range certify it.
    const auto literal=ts_node_child_count(node)==0 && bytes<=1024;
    const auto anchor=literal ? nullptr : (ts_node_child_count(node) ? ts_node_child(node,0).id : node.id);
    // Short leaves have no retained child array. Exact bounded text plus their
    // semantic context safely interns equivalent tokens without relying on slots.
    const SemanticKey key{reinterpret_cast<std::uintptr_t>(anchor),ts_node_symbol(node),code(inherited),code(own_color),code(own_object),depth,
        literal ? text.slice(node_range.begin.value,node_range.end.value) : std::string{}};
    if(literal){if(const auto found=built.find(key);found!=built.end()){semantic_limits(*found->second.unit,budget.options);return found->second.unit;}}
    std::optional<std::size_t> old_origin;
    if(changed){
        if(node_range.end.value<=changed->begin)old_origin=node_range.begin.value;
        else if(node_range.begin.value>=changed->new_end)old_origin=node_range.begin.value-changed->new_end+changed->old_end;
    }
    if(previous && (literal || !ts_node_has_changes(node))){
        if(const auto found=previous->find(key);found!=previous->end() && found->second.unit->summary.bytes==bytes &&
            (literal || (old_origin && found->second.origin==*old_origin && !found->second.unit->errors && !ts_node_has_error(node) && found->second.unit->stamps==child_stamps(node)))){
            semantic_limits(*found->second.unit,budget.options);
            ++statistics.reused_units;statistics.reused_bytes+=bytes;
            built.insert_or_assign(key,SemanticEntry{found->second.unit,node_range.begin.value});
            return found->second.unit;
        }
    }
    auto unit=std::make_shared<SemanticUnit>();unit->key=key;
    unit->errors=ts_node_has_error(node);
    unit->stamps=child_stamps(node);
    ++statistics.rebuilt_units;
    constexpr std::size_t leaf_bytes=1024,maximum_depth=128;
    const auto split=bytes>leaf_bytes && ts_node_child_count(node)>0 && depth<maximum_depth;
    Analysis local;
    const auto visited=budget.nodes;
    analyze_node(node,text,local,budget,false,split);
    unit->summary={bytes,local.outline.size(),local.objects.size(),budget.nodes-visited,local.captures.size()};
    unit->outline=std::move(local.outline);unit->objects=std::move(local.objects);
    for(auto& item:unit->outline){item.range=relative(item.range,node_range.begin.value);item.selection=relative(item.selection,node_range.begin.value);}
    for(auto& item:unit->objects){item.outer=relative(item.outer,node_range.begin.value);item.inner=relative(item.inner,node_range.begin.value);}
    if(!split){
        for(auto& capture:local.captures){capture.range=relative(capture.range,node_range.begin.value);++capture.priority;}
        if(inherited && bytes)local.captures.insert(local.captures.begin(),{{{0},{bytes}},*inherited,0});
        unit->highlights=index_captures(std::move(local.captures),bytes,budget);
    }else{
        std::vector<SemanticPiece> pieces;
        std::size_t position=node_range.begin.value;
        const auto fill=own_color ? own_color : inherited;
        const auto gap=[&](std::size_t length){
            if(!length)return;
            pieces.push_back({length,{}});
            const auto gap_key=std::pair(length,fill);
            auto found=gaps.find(gap_key);
            if(found==gaps.end()){
                HighlightIndex index({{length,fill}});
                if(previous_gaps){if(const auto retained=previous_gaps->find(gap_key);retained!=previous_gaps->end())index=retained->second;}
                found=gaps.emplace(gap_key,std::move(index)).first;
            }
            unit->highlights=unit->highlights.concat(found->second);
        };
        for(std::uint32_t index=0;index<ts_node_child_count(node);++index){
            budget.check();
            const auto child=ts_node_child(node,index);
            const auto child_range=range(child);
            if(child_range.begin.value<position || child_range.end>node_range.end)throw std::logic_error("Syntax children overlap their semantic partition");
            gap(child_range.begin.value-position);
            auto child_unit=build_semantic(child,text,fill,depth+1,previous,changed,built,previous_gaps,gaps,budget,statistics);
            unit->highlights=unit->highlights.concat(child_unit->highlights);
            pieces.push_back({child_unit->summary.bytes,std::move(child_unit)});
            position=child_range.end.value;
        }
        gap(node_range.end.value-position);
        unit->children=SemanticPieces(std::move(pieces));
        auto own=unit->summary;own.bytes=0;
        unit->summary=own+unit->children.summary();
    }
    semantic_limits(*unit,budget.options);built.insert_or_assign(key,SemanticEntry{unit,node_range.begin.value});return unit;
}
void collect_cache(const std::shared_ptr<const SemanticUnit>& unit,std::size_t origin,SemanticCache& cache,Budget& budget) {
    budget.check();cache.insert_or_assign(unit->key,SemanticEntry{unit,origin});
    auto offset=origin;
    unit->children.for_each_range(0,unit->children.size(),[&](const SemanticPiece& piece){if(piece.unit)collect_cache(piece.unit,offset,cache,budget);offset+=piece.bytes;});
}
void collect_outline(const SemanticUnit& unit,std::size_t origin,std::size_t depth,
    std::size_t begin,std::size_t end,std::vector<OutlineItem>& result) {
    const auto own=unit.outline.size();
    for(auto index=begin;index<std::min(end,own);++index){
        auto item=unit.outline[index];item.range=absolute(item.range,origin);item.selection=absolute(item.selection,origin);
        if(item.kind!=OutlineKind::heading)item.depth+=depth;
        result.push_back(std::move(item));
    }
    if(end<=own)return;
    const auto first=unit.children.seek(begin>own ? begin-own : 0,[](SemanticSummary value){return value.outlines;});
    const auto last=unit.children.seek(end-own-1,[](SemanticSummary value){return value.outlines;});
    auto row=own+first.prefix.outlines,offset=origin+first.prefix.bytes;
    unit.children.for_each_range(first.index,last.index+1,[&](const SemanticPiece& piece){
        if(piece.unit){
            const auto size=piece.unit->summary.outlines;
            if(size && row<end)collect_outline(*piece.unit,offset,depth+own,begin>row ? begin-row : 0,std::min(end-row,size),result);
            row+=size;
        }
        offset+=piece.bytes;
    });
}
void collect_objects(const SemanticUnit& unit,std::size_t origin,std::vector<TextObject>& result) {
    for(auto item:unit.objects){item.outer=absolute(item.outer,origin);item.inner=absolute(item.inner,origin);result.push_back(std::move(item));}
    auto offset=origin;
    unit.children.for_each_range(0,unit.children.size(),[&](const SemanticPiece& piece){
        if(piece.unit)collect_objects(*piece.unit,offset,result);offset+=piece.bytes;
    });
}
template<typename Function>
void containing_objects(const SemanticUnit& unit,std::size_t origin,SyntaxRange selection,Function&& function) {
    for(auto item:unit.objects){item.outer=absolute(item.outer,origin);item.inner=absolute(item.inner,origin);function(item);}
    if(selection.begin.value<origin || selection.end.value>origin+unit.summary.bytes || unit.children.empty())return;
    const auto child=unit.children.seek(selection.begin.value-origin,[](SemanticSummary value){return value.bytes;});
    if(!child.item || !child.item->unit)return;
    const auto child_origin=origin+child.prefix.bytes;
    if(selection.end.value<=child_origin+child.item->bytes)containing_objects(*child.item->unit,child_origin,selection,function);
}
void normalize_ranges(std::vector<SyntaxRange>& ranges) {
    std::ranges::sort(ranges);
    std::vector<SyntaxRange> merged;
    for (const auto& item : ranges) {
        if (!merged.empty() && item.begin <= merged.back().end) {
            merged.back().end = std::max(merged.back().end, item.end);
        } else { merged.push_back(item); }
    }
    ranges = std::move(merged);
}

} // namespace

struct SyntaxSnapshot::State {
    BufferId id;
    Revision revision;
    ContentId identity;
    LanguageId language;
    Rope text;
    Tree tree;
    std::vector<Tree> inline_trees;
    HighlightIndex highlights;
    mutable std::vector<OutlineItem> outline;
    mutable std::vector<TextObject> objects;
    std::shared_ptr<const SemanticUnit> semantic;
    std::size_t semantic_origin{};
    SemanticCache semantic_cache;
    GapCache gap_cache;
    AnalysisStatistics statistics;
    mutable std::once_flag outline_once,objects_once;
    std::vector<SyntaxRange> changed;
    bool errors{};
    bool incremental{};
    mutable std::mutex copy_mutex;
    void index_semantics(const State* previous,Budget& budget) {
        const auto root=ts_tree_root_node(tree.get());semantic_origin=ts_node_start_byte(root);
        const auto extent=previous ? previous->text.changed_extent(text) : RopeChangedExtent{};
        semantic=build_semantic(root,text,std::nullopt,0,previous ? &previous->semantic_cache : nullptr,previous ? &extent : nullptr,semantic_cache,previous ? &previous->gap_cache : nullptr,gap_cache,budget,statistics);
        if(semantic_origin)highlights=HighlightIndex({{semantic_origin,std::nullopt}});
        highlights=highlights.concat(semantic->highlights);
        const auto ending=semantic_origin+semantic->summary.bytes;
        if(ending<text.size_bytes())highlights=highlights.concat(HighlightIndex({{text.size_bytes()-ending,std::nullopt}}));
        if(highlights.run_count()>budget.options.max_spans)throw std::length_error("syntax index limit exceeded");
        collect_cache(semantic,semantic_origin,semantic_cache,budget);
        statistics.visited_nodes=budget.nodes;
        errors=ts_node_has_error(root);
    }
};

struct SyntaxParser::Impl {
    LanguageId language;
    Parser parser;
    Parser inline_parser;
    explicit Impl(LanguageId id) : language(id), parser(make_parser(grammar(id))) {
        if (id == LanguageId::markdown) { inline_parser = make_parser(tree_sitter_markdown_inline()); }
    }
};

std::string_view language_name(LanguageId language) {
    switch (language) {
    case LanguageId::c: return "c";
    case LanguageId::cpp: return "cpp";
    case LanguageId::javascript: return "javascript";
    case LanguageId::typescript: return "typescript";
    case LanguageId::tsx: return "tsx";
    case LanguageId::json: return "json";
    case LanguageId::python: return "python";
    case LanguageId::markdown: return "markdown";
    }
    throw std::invalid_argument("unknown syntax language");
}

std::optional<LanguageId> language_for_path(std::string_view path) {
    const auto dot = path.rfind('.');
    if (dot == path.npos) { return std::nullopt; }
    auto extension = std::string(path.substr(dot));
    for (auto& character : extension) {
        if (character >= 'A' && character <= 'Z') { character = static_cast<char>(character + ('a' - 'A')); }
    }
    if (extension == ".c") { return LanguageId::c; }
    if (one_of(extension, {".h", ".cc", ".cpp", ".cxx", ".hpp", ".hxx", ".hh"})) { return LanguageId::cpp; }
    if (one_of(extension, {".js", ".jsx", ".mjs", ".cjs"})) { return LanguageId::javascript; }
    if (one_of(extension, {".ts", ".mts", ".cts"})) { return LanguageId::typescript; }
    if (extension == ".tsx") { return LanguageId::tsx; }
    if (extension == ".json") { return LanguageId::json; }
    if (one_of(extension, {".py", ".pyi"})) { return LanguageId::python; }
    if (one_of(extension, {".md", ".markdown"})) { return LanguageId::markdown; }
    return std::nullopt;
}

HighlightSummary operator+(HighlightSummary left, HighlightSummary right) {
    if (right.bytes > std::numeric_limits<std::size_t>::max() - left.bytes) {
        throw std::length_error("syntax highlight summary overflow");
    }
    return {left.bytes + right.bytes};
}

HighlightSummary MeasureHighlight::operator()(const HighlightRun& run) const noexcept { return {run.bytes}; }

HighlightIndex::HighlightIndex(std::vector<HighlightRun> runs) : runs_([&runs] {
    for (const auto& run : runs) {
        if (run.bytes == 0) { throw std::invalid_argument("syntax highlight runs must be nonempty"); }
    }
    return std::move(runs);
}()) {}
std::size_t HighlightIndex::size_bytes() const noexcept { return runs_.summary().bytes; }
std::size_t HighlightIndex::run_count() const noexcept { return runs_.size(); }
std::size_t HighlightIndex::tree_height() const noexcept { return runs_.height(); }
bool HighlightIndex::validate() const { return runs_.validate(); }
HighlightIndex HighlightIndex::concat(const HighlightIndex& other) const {
    HighlightIndex result;
    if(runs_.empty()){result.runs_=other.runs_;return result;}
    if(other.runs_.empty()){result.runs_=runs_;return result;}
    const auto& last=runs_.at(runs_.size()-1);const auto& first=other.runs_.at(0);
    if(last.kind==first.kind){
        const auto bytes=(HighlightSummary{last.bytes}+HighlightSummary{first.bytes}).bytes;
        result.runs_=runs_.split(runs_.size()-1).first.concat(decltype(runs_)::singleton({bytes,last.kind})).concat(other.runs_.split(1).second);
    }else result.runs_=runs_.concat(other.runs_);
    return result;
}
std::size_t HighlightIndex::shared_prefix_bytes(const HighlightIndex& other) const { return runs_.shared_prefix_summary(other.runs_).bytes; }
std::size_t HighlightIndex::shared_suffix_bytes(const HighlightIndex& other) const { return runs_.shared_suffix_summary(other.runs_).bytes; }

std::vector<HighlightSpan> HighlightIndex::intersecting(SyntaxRange query) const {
    if (query.begin > query.end || query.end.value > size_bytes()) {
        throw std::out_of_range("syntax highlight range exceeds document");
    }
    std::vector<HighlightSpan> result;
    if (query.begin == query.end) { return result; }
    const auto projection = [](HighlightSummary summary) { return summary.bytes; };
    const auto first = runs_.seek(query.begin.value, projection);
    const auto last = runs_.seek(query.end.value - 1, projection);
    auto offset = first.prefix.bytes;
    runs_.for_each_range(first.index, last.index + 1, [&](const HighlightRun& run) {
        if (run.kind) {
            result.push_back({{{std::max(offset, query.begin.value)},
                {std::min(offset + run.bytes, query.end.value)}}, *run.kind});
        }
        offset += run.bytes;
    });
    return result;
}

ParseCancelled::ParseCancelled() : std::runtime_error("syntax parsing cancelled or deadline exceeded") {}
SyntaxSnapshot::SyntaxSnapshot(std::shared_ptr<const State> state) : state_(std::move(state)) {}
BufferId SyntaxSnapshot::id() const noexcept { return state_->id; }
Revision SyntaxSnapshot::revision() const noexcept { return state_->revision; }
ContentId SyntaxSnapshot::content_identity() const noexcept { return state_->identity; }
LanguageId SyntaxSnapshot::language() const noexcept { return state_->language; }
const Rope& SyntaxSnapshot::text() const noexcept { return state_->text; }
const HighlightIndex& SyntaxSnapshot::highlights() const noexcept { return state_->highlights; }
std::size_t SyntaxSnapshot::outline_size() const noexcept { return state_->semantic ? state_->semantic->summary.outlines : state_->outline.size(); }
std::vector<OutlineItem> SyntaxSnapshot::outline_range(std::size_t begin,std::size_t end) const {
    if(begin>end || end>outline_size())throw std::out_of_range("Syntax outline range exceeds its index");
    std::vector<OutlineItem> result;result.reserve(end-begin);
    if(state_->semantic){if(begin!=end)collect_outline(*state_->semantic,state_->semantic_origin,0,begin,end,result);}
    else result.assign(state_->outline.begin()+static_cast<std::ptrdiff_t>(begin),state_->outline.begin()+static_cast<std::ptrdiff_t>(end));
    return result;
}
const std::vector<OutlineItem>& SyntaxSnapshot::outline() const {
    if(state_->semantic)std::call_once(state_->outline_once,[&]{state_->outline=outline_range(0,outline_size());});
    return state_->outline;
}
const std::vector<TextObject>& SyntaxSnapshot::text_objects() const {
    if(state_->semantic)std::call_once(state_->objects_once,[&]{
        std::vector<TextObject> result;result.reserve(state_->semantic->summary.objects);
        collect_objects(*state_->semantic,state_->semantic_origin,result);state_->objects=std::move(result);
    });
    return state_->objects;
}
AnalysisStatistics SyntaxSnapshot::analysis_statistics() const noexcept { return state_->statistics; }
const std::vector<SyntaxRange>& SyntaxSnapshot::changed_ranges() const noexcept { return state_->changed; }
bool SyntaxSnapshot::has_errors() const noexcept { return state_->errors; }
bool SyntaxSnapshot::incremental() const noexcept { return state_->incremental; }
bool SyntaxSnapshot::matches(const DocumentSnapshot& document) const noexcept {
    return id() == document.id && revision() == document.revision && content_identity() == document.content_identity;
}

std::optional<SyntaxRange> SyntaxSnapshot::enclosing_object(SyntaxRange selection,
    TextObjectKind kind, bool inner, bool strictly_larger) const {
    if (selection.begin > selection.end || selection.end.value > text().size_bytes() ||
        !text().is_boundary(selection.begin.value) || !text().is_boundary(selection.end.value)) {
        throw std::invalid_argument("text-object selection is not a valid UTF-8 range");
    }
    std::optional<SyntaxRange> best;
    std::size_t best_size = std::numeric_limits<std::size_t>::max();
    const auto consider=[&](const TextObject& object) {
        if (object.kind != kind) { return; }
        const auto candidate = inner ? object.inner : object.outer;
        if (object.outer.begin <= selection.begin && object.outer.end >= selection.end &&
            !(strictly_larger && candidate == selection) &&
            // A caret on the end boundary belongs to the following object.
            !(selection.begin == selection.end && object.outer.end == selection.begin && object.outer.begin < object.outer.end) &&
            object.outer.end.value - object.outer.begin.value < best_size) {
            best = candidate;
            best_size = object.outer.end.value - object.outer.begin.value;
        }
    };
    if(state_->semantic)containing_objects(*state_->semantic,state_->semantic_origin,selection,consider);
    else for(const auto& object:state_->objects)consider(object);
    return best;
}

SyntaxParser::SyntaxParser(LanguageId language) : impl_(std::make_unique<Impl>(language)) {}
SyntaxParser::~SyntaxParser() = default;
SyntaxParser::SyntaxParser(SyntaxParser&&) noexcept = default;
SyntaxParser& SyntaxParser::operator=(SyntaxParser&&) noexcept = default;

SyntaxSnapshot SyntaxParser::parse(const DocumentSnapshot& document, const ParseOptions& options) {
    if (!impl_) { throw std::logic_error("syntax parser has been moved from"); }
    Budget budget(options);
    if (document.text.size_bytes() > options.max_bytes) { throw std::length_error("syntax input limit exceeded"); }
    (void)narrow(document.text.size_bytes());
    auto state = std::make_shared<SyntaxSnapshot::State>();
    state->id = document.id;
    state->revision = document.revision;
    state->identity = document.content_identity;
    state->language = impl_->language;
    state->text = document.text;
    state->tree = parse_tree(impl_->parser.get(), state->text, nullptr, budget);
    if(!impl_->inline_parser && options.incremental_analysis)state->index_semantics(nullptr,budget);
    else {
    Analysis analysis;
    analyze(state->tree.get(), state->text, analysis, budget);
    state->errors = ts_node_has_error(ts_tree_root_node(state->tree.get()));
    if (impl_->inline_parser) {
        for (const auto& group : analysis.inline_groups) {
            if (!ts_parser_set_included_ranges(impl_->inline_parser.get(), group.data(), narrow(group.size()))) {
                throw std::runtime_error("Markdown inline ranges overlap");
            }
            auto tree = parse_tree(impl_->inline_parser.get(), state->text, nullptr, budget);
            state->errors = state->errors || ts_node_has_error(ts_tree_root_node(tree.get()));
            analyze(tree.get(), state->text, analysis, budget, true);
            state->inline_trees.push_back(std::move(tree));
        }
    }
    state->highlights = index_captures(std::move(analysis.captures), state->text.size_bytes(), budget);
    state->outline = std::move(analysis.outline);
    state->objects = std::move(analysis.objects);
    state->statistics.visited_nodes=budget.nodes;
    }
    if (!state->text.empty()) { state->changed.push_back({{0}, {state->text.size_bytes()}}); }
    budget.check();
    return SyntaxSnapshot(std::move(state));
}

SyntaxSnapshot SyntaxParser::update(const SyntaxSnapshot& previous, Transaction transaction,
    Revision resulting_revision, ContentId resulting_identity, const ParseOptions& options) {
    if (!impl_) { throw std::logic_error("syntax parser has been moved from"); }
    Budget budget(options);
    if (previous.language() != impl_->language) { throw std::invalid_argument("incremental syntax language differs"); }
    if (transaction.base_revision != previous.revision()) { throw StaleRevision(); }
    if (resulting_revision <= previous.revision()) { throw std::invalid_argument("syntax result revision must advance"); }
    auto& edits = transaction.edits;
    std::ranges::sort(edits, [](const Edit& left, const Edit& right) {
        return left.begin == right.begin ? left.end < right.end : left.begin < right.begin;
    });
    const Edit* last = nullptr;
    auto resulting_size = previous.text().size_bytes();
    for (const auto& edit : edits) {
        budget.check();
        if (edit.begin > edit.end || edit.end.value > previous.text().size_bytes() ||
            !previous.text().is_boundary(edit.begin.value) || !previous.text().is_boundary(edit.end.value)) {
            throw std::invalid_argument("incremental syntax edit has invalid range");
        }
        if (last && (last->end > edit.begin || (last->end == edit.begin &&
            (last->begin == last->end || edit.begin == edit.end)))) {
            throw std::invalid_argument("incremental syntax edits overlap or have ambiguous insertion order");
        }
        const auto removed = edit.end.value - edit.begin.value;
        if (edit.text.size() > options.max_bytes ||
            resulting_size - removed > options.max_bytes - edit.text.size()) {
            throw std::length_error("syntax input limit exceeded");
        }
        resulting_size = resulting_size - removed + edit.text.size();
        utf8::validate(edit.text);
        last = &edit;
    }
    auto state = std::make_shared<SyntaxSnapshot::State>();
    state->id = previous.id();
    state->revision = resulting_revision;
    state->identity = resulting_identity;
    state->language = impl_->language;
    state->text = previous.text();
    Tree old_tree;
    std::vector<Tree> old_inline;
    {
        const std::lock_guard lock(previous.state_->copy_mutex);
        old_tree.reset(ts_tree_copy(previous.state_->tree.get()));
        for (const auto& tree : previous.state_->inline_trees) {
            old_inline.emplace_back(ts_tree_copy(tree.get()));
        }
    }
    if (!old_tree) { throw std::bad_alloc(); }
    for (auto edit = edits.rbegin(); edit != edits.rend(); ++edit) {
        budget.check();
        const auto start = point(previous.text(), edit->begin.value);
        const TSInputEdit input_edit{narrow(edit->begin.value), narrow(edit->end.value),
            narrow(edit->begin.value + edit->text.size()), start,
            point(previous.text(), edit->end.value), advanced_point(start, edit->text)};
        ts_tree_edit(old_tree.get(), &input_edit);
        for (const auto& tree : old_inline) { ts_tree_edit(tree.get(), &input_edit); }
        state->text = state->text.replace(edit->begin.value, edit->end.value, edit->text);
    }
    if (state->text.size_bytes() > options.max_bytes) { throw std::length_error("syntax input limit exceeded"); }
    (void)narrow(state->text.size_bytes());
    state->tree = parse_tree(impl_->parser.get(), state->text, old_tree.get(), budget);
    // Error recovery can depend on earlier ambiguous incremental states even
    // when their visible captures agree. A fresh tree restores the grammar's
    // canonical result before retaining semantic indexes for malformed input.
    const auto fresh_recovery=previous.has_errors() || ts_node_has_error(ts_tree_root_node(state->tree.get()));
    if(fresh_recovery){state->tree=parse_tree(impl_->parser.get(),state->text,nullptr,budget);old_inline.clear();}
    if(!impl_->inline_parser && options.incremental_analysis)state->index_semantics(previous.state_.get(),budget);
    else {
    Analysis analysis;
    analyze(state->tree.get(), state->text, analysis, budget);
    state->errors = ts_node_has_error(ts_tree_root_node(state->tree.get()));
    if (impl_->inline_parser) {
        for (std::size_t index = 0; index < analysis.inline_groups.size(); ++index) {
            const auto& group = analysis.inline_groups[index];
            if (!ts_parser_set_included_ranges(impl_->inline_parser.get(), group.data(), narrow(group.size()))) {
                throw std::runtime_error("Markdown inline ranges overlap");
            }
            auto tree = parse_tree(impl_->inline_parser.get(), state->text,
                index < old_inline.size() ? old_inline[index].get() : nullptr, budget);
            state->errors = state->errors || ts_node_has_error(ts_tree_root_node(tree.get()));
            analyze(tree.get(), state->text, analysis, budget, true);
            state->inline_trees.push_back(std::move(tree));
        }
    }
    state->highlights = index_captures(std::move(analysis.captures), state->text.size_bytes(), budget);
    state->outline = std::move(analysis.outline);
    state->objects = std::move(analysis.objects);
    state->statistics.visited_nodes=budget.nodes;
    }
    std::uint32_t count = 0;
    const std::unique_ptr<TSRange, decltype(&std::free)> changed(
        ts_tree_get_changed_ranges(old_tree.get(), state->tree.get(), &count), &std::free);
    for (std::uint32_t index = 0; index < count; ++index) {
        state->changed.push_back({{changed.get()[index].start_byte}, {changed.get()[index].end_byte}});
    }
    std::size_t original_offset = 0;
    std::size_t updated_offset = 0;
    for (const auto& edit : edits) {
        updated_offset += edit.begin.value - original_offset;
        const auto begin = updated_offset;
        updated_offset += edit.text.size();
        state->changed.push_back({{begin}, {updated_offset}});
        original_offset = edit.end.value;
    }
    normalize_ranges(state->changed);
    state->incremental = !fresh_recovery;
    budget.check();
    return SyntaxSnapshot(std::move(state));
}

} // namespace astra::language
