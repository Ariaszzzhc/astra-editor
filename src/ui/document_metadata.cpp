#include <astra/ui/document_metadata.hpp>
#include <charconv>
#include <stdexcept>
#include <string>
#include <string_view>

namespace astra::ui {
namespace {
std::uint64_t decimal(const protocol::Json& metadata, std::string_view field) {
    const auto found = metadata.find(std::string(field));
    if (found == metadata.end() || !found->is_string())
        throw std::invalid_argument("Document metadata requires decimal revision and sequence");
    const auto& text = found->get_ref<const std::string&>();
    if (text.empty() || (text.size() > 1 && text.front() == '0'))
        throw std::invalid_argument("Document metadata identity is not canonical decimal");
    std::uint64_t value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        throw std::invalid_argument("Document metadata identity exceeds unsigned 64-bit range");
    return value;
}
} // namespace

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
} // namespace astra::ui
