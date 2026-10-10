#include <astra/ui/document_metadata.hpp>
#include <charconv>
#include <stdexcept>
#include <string>
#include <string_view>

namespace astra::ui {
namespace {
std::uint64_t unsigned_decimal(std::string_view text) {
    if (text.empty() || (text.size() > 1 && text.front() == '0'))
        throw std::invalid_argument("Document metadata identity is not canonical decimal");
    std::uint64_t value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        throw std::invalid_argument("Document metadata identity exceeds unsigned 64-bit range");
    return value;
}

std::uint64_t decimal(const protocol::Json& metadata, std::string_view field) {
    const auto found = metadata.find(std::string(field));
    if (found == metadata.end() || !found->is_string())
        throw std::invalid_argument("Document metadata requires decimal revision and sequence");
    return unsigned_decimal(found->get_ref<const std::string&>());
}
} // namespace

std::uint64_t document_id(std::string_view text) {
    const auto value = unsigned_decimal(text);
    if (!value)
        throw std::invalid_argument("Document IDs must be positive");
    return value;
}

DocumentMetadataStamp DocumentMetadataStamp::read(const protocol::Json& metadata) {
    if (!metadata.is_object())
        throw std::invalid_argument("Document metadata requires an object");
    const auto revision = decimal(metadata, "revision");
    const auto sequence = decimal(metadata, "metadataSequence");
    if (!sequence)
        throw std::invalid_argument("Document metadata sequence must be positive");
    return {revision, sequence};
}

bool DocumentMetadataStamp::follows(const DocumentMetadataStamp& current) const noexcept {
    return revision >= current.revision && sequence >= current.sequence &&
           (sequence != current.sequence || revision == current.revision);
}

bool ClosedDocumentIds::contains(std::uint64_t id) const {
    const auto found = ranges_.seek_prefix([id](Summary summary) {
        return summary.last >= id;
    });
    return found.item && found.item->first <= id;
}

bool ClosedDocumentIds::retire(std::uint64_t id) {
    if (!id)
        throw std::invalid_argument("Cannot retire an invalid document ID");
    const auto found = ranges_.seek_prefix([id](Summary summary) {
        return summary.last >= id;
    });
    if (found.item && found.item->first <= id)
        return false;
    Range range{id, id};
    auto begin = found.index;
    auto end = found.index;
    if (begin && ranges_.at(begin - 1).last == id - 1) {
        range.first = ranges_.at(begin - 1).first;
        --begin;
    }
    if (found.item && found.item->first - 1 == id) {
        range.last = found.item->last;
        ++end;
    }
    ranges_ = ranges_.erase(begin, end).insert(begin, range);
    return true;
}

std::size_t ClosedDocumentIds::range_count() const noexcept {
    return ranges_.size();
}

bool ClosedDocumentIds::validate() const {
    auto valid = ranges_.validate();
    std::uint64_t previous{};
    ranges_.for_each([&](const Range& range) {
        valid = valid && range.first && range.first <= range.last &&
                (!previous || (range.first > previous && range.first - previous > 1));
        previous = range.last;
    });
    return valid;
}
} // namespace astra::ui
