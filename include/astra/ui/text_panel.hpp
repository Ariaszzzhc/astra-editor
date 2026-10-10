#pragma once

#include <astra/protocol/rpc.hpp>
#include <astra/ui/range_index.hpp>

#include <string>
#include <cstddef>
#include <vector>

namespace astra::ui {
enum class TextPanelTone { normal, added, removed, accent, muted };

struct TextPanelAction {
    std::string id, label, key;
    bool destructive{};
};

// Domain-neutral modal presentation. JS supplies action identities and line tones;
// the native component validates bounds and resolves input, never service commands.
class TextPanelPresentation {
  public:
    void reset(const protocol::Json& spec, std::size_t rows);
    [[nodiscard]] const std::vector<TextPanelAction>& actions() const noexcept;
    [[nodiscard]] TextPanelTone tone(std::size_t row) const;
    [[nodiscard]] const TextPanelAction* shortcut(char key) const noexcept;

  private:
    std::vector<TextPanelAction> actions_;
    RangeIndex<TextPanelTone, DocumentLineUnit> tones_;
};
} // namespace astra::ui
