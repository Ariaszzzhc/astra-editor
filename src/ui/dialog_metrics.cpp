#include <astra/ui/dialog_metrics.hpp>
#include <astra/ui/text_labels.hpp>
#include <astra/ui/workbench_style.hpp>
#include <algorithm>
#include <cmath>

namespace astra::ui {
DialogMetrics measure_dialog_metrics(TextLabels& labels,
                                     const WorkbenchStyle& theme,
                                     float dpi,
                                     bool monospace_detail) {
    const auto line =
        [&](float reference, float line_height, float minimum, bool monospace = false) {
            const auto paragraph =
                labels.prepare("Mg中🙂", theme.text(reference, dpi, 0, line_height, monospace));
            return std::max(minimum, std::ceil(paragraph->height()) / dpi);
        };
    return {line(16, 1.15F, 24),
            line(13, 1.4F, 18, monospace_detail),
            line(14, 1, 20),
            line(13, 1.15F, 18),
            line(13, 1.15F, 18)};
}
} // namespace astra::ui
