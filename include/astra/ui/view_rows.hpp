#pragma once

#include <astra/ui/icons.hpp>
#include <astra/ui/text_labels.hpp>
#include <astra/ui/view_index.hpp>
#include <astra/ui/view_rows_layout.hpp>
#include <astra/ui/workbench_style.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

class SkCanvas;

namespace astra::ui {
// Native presentation only: immutable indexed rows and JS-supplied descriptors.
// The caller owns documents, providers, selection identity and all row actions.
class ViewRows final {
  public:
    struct Model {
        std::string_view cursor, empty;
        bool focused{}, expanded{true};
        float pointer_x{}, pointer_y{};
    };

    struct Decoration {
        Icon icon{Icon::document};
        std::uint32_t color{};
    };

    using ResolveIcon = std::function<std::optional<Decoration>(const ViewRow&)>;

    explicit ViewRows(TextLabels& labels, IconRenderer& icons) : labels_(labels), icons_(icons) {
    }

    [[nodiscard]] std::size_t paint(SkCanvas*,
                                    const ViewIndex&,
                                    const ViewRowsLayout&,
                                    const WorkbenchStyle&,
                                    const Model&,
                                    const ResolveIcon&);

  private:
    TextLabels& labels_;
    IconRenderer& icons_;
};
} // namespace astra::ui
