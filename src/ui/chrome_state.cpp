#include <astra/ui/chrome_state.hpp>
#include <astra/core/utf8.hpp>
#include <limits>
#include <utility>

namespace astra::ui {
using Json = protocol::Json;

namespace {
void caption(const Json& value) {
    const auto text = value.get<std::string>();
    if (text.size() > 128 || text.find_first_of("\r\n") != text.npos ||
        text.find('\0') != text.npos)
        throw protocol::RpcError(-32602, "Invalid chrome caption");
    utf8::validate(text);
}
} // namespace

ChromeState::ChromeState() : model_({{"brand", ""}, {"hint", ""}}) {
    for (const auto name : chrome_banks)
        model_[name] = Json::array();
}

void ChromeState::sync(Json context) {
    if (context == context_)
        return;
    if (!context.is_object() || context.dump().size() > 64 * 1024)
        throw protocol::RpcError(-32602, "Chrome context exceeds its bounded object");
    invalidate(std::move(context));
}

void ChromeState::invalidate(Json context) {
    if (version_ == std::numeric_limits<std::uint64_t>::max())
        throw protocol::RpcError(-32004, "Chrome generation exhausted");
    context_ = std::move(context);
    ++version_;
    dirty_ = pending_ = true;
}

void ChromeState::refresh() {
    invalidate(context_);
}

std::optional<Json> ChromeState::take_request() {
    if (!dirty_)
        return std::nullopt;
    auto result = Json{{"version", std::to_string(version_)}, {"context", context_}};
    dirty_ = false;
    return result;
}

bool ChromeState::publish(const Json& params) {
    if (!pending_ || params.at("version") != std::to_string(version_))
        return false;
    if (params.size() != 2 || !params.contains("model"))
        throw protocol::RpcError(-32602, "Chrome publication requires version and model");
    auto candidate = params.at("model");
    if (!candidate.is_object() || candidate.size() != 8 || candidate.dump().size() > 64 * 1024)
        throw protocol::RpcError(-32602,
                                 "Chrome model requires six bounded row banks and captions");
    caption(candidate.at("brand"));
    caption(candidate.at("hint"));
    std::array<ViewIndex, 6> prepared;
    for (std::size_t bank = 0; bank < chrome_banks.size(); ++bank) {
        const auto& values = candidate.at(chrome_banks[bank]);
        const auto maximum = bank == 0 || bank == 2 ? 1U : bank == 1 ? 16U : 32U;
        if (!values.is_array() || values.size() > maximum)
            throw protocol::RpcError(-32602, "Chrome row bank exceeds its capacity");
        for (const auto& value : values) {
            if (value.value("depth", 0) != 0)
                throw protocol::RpcError(-32602, "Chrome rows must be flat");
            for (const auto& [field, unused] : value.items()) {
                static_cast<void>(unused);
                if (field != "id" && field != "label" && field != "icon" && field != "tone" &&
                    field != "iconTone" && field != "selected" && field != "data")
                    throw protocol::RpcError(-32602, "Unknown chrome row field");
            }
        }
        prepared[bank] = ViewIndex::page(values);
    }
    rows_ = std::move(prepared);
    model_ = std::move(candidate);
    published_ = version_;
    pending_ = false;
    return true;
}

bool ChromeState::reject(std::string_view version) {
    if (!pending_ || version != std::to_string(version_))
        return false;
    pending_ = false;
    return true;
}

Json ChromeState::activate(std::string_view version,
                           std::string_view bank,
                           std::string_view row) const {
    if (pending_ || published_ != version_ || version != std::to_string(version_))
        return nullptr;
    const auto found = std::find(chrome_banks.begin(), chrome_banks.end(), bank);
    if (found == chrome_banks.end())
        throw protocol::RpcError(-32602, "Unknown chrome row bank");
    const auto& source = rows_.at(static_cast<std::size_t>(found - chrome_banks.begin()));
    const auto position = source.position(row);
    if (!position)
        return nullptr;
    return {{"row", *source.at(*position).value}, {"context", context_}, {"bank", bank}};
}

Json ChromeState::state() const {
    return {{"version", std::to_string(version_)},
            {"modelVersion", std::to_string(published_)},
            {"context", context_},
            {"pending", pending_},
            {"model", model_}};
}
} // namespace astra::ui
