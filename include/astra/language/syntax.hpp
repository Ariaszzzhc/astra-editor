#pragma once

#include <astra/core/document.hpp>
#include <astra/core/sum_tree.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace astra::language {

enum class LanguageId { c, cpp, javascript, typescript, tsx, json, python, markdown };
[[nodiscard]] std::string_view language_name(LanguageId language);
[[nodiscard]] std::optional<LanguageId> language_for_path(std::string_view path);

enum class HighlightKind {
    comment, string, number, keyword, type, function, property, variable,
    operator_, punctuation, heading, error
};

struct SyntaxRange {
    ByteOffset begin;
    ByteOffset end;
    auto operator<=>(const SyntaxRange&) const = default;
};

struct HighlightSpan {
    SyntaxRange range;
    HighlightKind kind;
    auto operator<=>(const HighlightSpan&) const = default;
};

struct HighlightRun {
    std::size_t bytes{};
    std::optional<HighlightKind> kind;
};

struct HighlightSummary {
    std::size_t bytes{};
    friend HighlightSummary operator+(HighlightSummary left, HighlightSummary right);
    bool operator==(const HighlightSummary&) const = default;
};

struct MeasureHighlight {
    [[nodiscard]] HighlightSummary operator()(const HighlightRun& run) const noexcept;
};

// Non-overlapping runs include uncolored gaps. Byte seeking is O(log n), and a
// viewport visits only its intersecting runs. Copies retain immutable SumTree nodes.
class HighlightIndex {
public:
    HighlightIndex() = default;
    explicit HighlightIndex(std::vector<HighlightRun> runs);
    [[nodiscard]] std::size_t size_bytes() const noexcept;
    [[nodiscard]] std::size_t run_count() const noexcept;
    [[nodiscard]] std::size_t tree_height() const noexcept;
    [[nodiscard]] std::vector<HighlightSpan> intersecting(SyntaxRange range) const;
    [[nodiscard]] bool validate() const;
private:
    SumTree<HighlightRun, HighlightSummary, MeasureHighlight> runs_;
};

enum class OutlineKind { function, class_, struct_, enum_, namespace_, type, property, heading };
struct OutlineItem {
    std::string name;
    OutlineKind kind;
    SyntaxRange range;
    SyntaxRange selection;
    std::size_t depth{};
    bool operator==(const OutlineItem&) const = default;
};

enum class TextObjectKind { function, class_, comment, string, block, parameter, heading };
struct TextObject {
    TextObjectKind kind;
    SyntaxRange outer;
    SyntaxRange inner;
    bool operator==(const TextObject&) const = default;
};

struct ParseOptions {
    std::stop_token stop;
    std::chrono::milliseconds timeout{250};
    std::size_t max_bytes{32 * 1024 * 1024};
    std::size_t max_nodes{2'000'000};
    std::size_t max_spans{1'000'000};
    std::size_t max_outline_items{100'000};
    std::size_t max_text_objects{250'000};
};

class ParseCancelled final : public std::runtime_error {
public:
    ParseCancelled();
};

class SyntaxSnapshot {
public:
    [[nodiscard]] BufferId id() const noexcept;
    [[nodiscard]] Revision revision() const noexcept;
    [[nodiscard]] ContentId content_identity() const noexcept;
    [[nodiscard]] LanguageId language() const noexcept;
    [[nodiscard]] const Rope& text() const noexcept;
    [[nodiscard]] const HighlightIndex& highlights() const noexcept;
    [[nodiscard]] const std::vector<OutlineItem>& outline() const noexcept;
    [[nodiscard]] const std::vector<TextObject>& text_objects() const noexcept;
    [[nodiscard]] const std::vector<SyntaxRange>& changed_ranges() const noexcept;
    [[nodiscard]] bool has_errors() const noexcept;
    [[nodiscard]] bool incremental() const noexcept;
    [[nodiscard]] bool matches(const DocumentSnapshot& document) const noexcept;
    // Chooses the smallest containing object; a nonempty selection can be
    // expanded repeatedly to an enclosing object. Coordinates are UTF-8 bytes.
    [[nodiscard]] std::optional<SyntaxRange> enclosing_object(
        SyntaxRange selection, TextObjectKind kind, bool inner = false,
        bool strictly_larger = false) const;

private:
    struct State;
    std::shared_ptr<const State> state_;
    explicit SyntaxSnapshot(std::shared_ptr<const State> state);
    friend class SyntaxParser;
};

// Synchronous worker-only parser. One owner serializes calls. Immutable results
// may cross worker/service boundaries; only publish a result if matches() holds.
// Cancellation/deadline/limits throw and preserve the previous snapshot.
class SyntaxParser {
public:
    explicit SyntaxParser(LanguageId language);
    ~SyntaxParser();
    SyntaxParser(SyntaxParser&&) noexcept;
    SyntaxParser& operator=(SyntaxParser&&) noexcept;
    SyntaxParser(const SyntaxParser&) = delete;
    SyntaxParser& operator=(const SyntaxParser&) = delete;

    [[nodiscard]] SyntaxSnapshot parse(const DocumentSnapshot& document,
                                       const ParseOptions& options = {});
    // The exact committed transaction uses coordinates in previous.text().
    // Undo/redo without a transaction log use parse() instead. The resulting
    // revision must advance; resulting_identity comes from the document service.
    [[nodiscard]] SyntaxSnapshot update(const SyntaxSnapshot& previous,
        Transaction transaction, Revision resulting_revision,
        ContentId resulting_identity, const ParseOptions& options = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace astra::language
