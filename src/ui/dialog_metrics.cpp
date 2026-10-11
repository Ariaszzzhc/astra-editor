#include <astra/ui/dialog_metrics.hpp>
#include <astra/ui/text_labels.hpp>
#include <astra/ui/workbench_style.hpp>
#include <algorithm>
#include <cmath>

namespace astra::ui {
namespace {
struct LineProbe {
    TextLabels& labels;
    const WorkbenchStyle& theme;
    float dpi;

    float
    operator()(float reference, float line_height, float minimum, bool monospace = false) const {
        const auto paragraph =
            labels.prepare("Mg中🙂", theme.text(reference, dpi, 0, line_height, monospace));
        return std::max(minimum, std::ceil(paragraph->height()) / dpi);
    }
};
} // namespace

DialogMetrics measure_dialog_metrics(TextLabels& labels,
                                     const WorkbenchStyle& theme,
                                     float dpi,
                                     bool monospace_detail) {
    const LineProbe line{labels, theme, dpi};
    return {line(16, 1.15F, 24),
            line(13, 1.4F, 18, monospace_detail),
            line(14, 1, 20),
            line(13, 1.15F, 18),
            line(13, 1.15F, 18)};
}

DialogMetrics measure_inline_metrics(TextLabels& labels, const WorkbenchStyle& theme, float dpi) {
    const LineProbe line{labels, theme, dpi};
    return {line(12, 1.15F, 20),
            line(11, 1.15F, 15),
            line(13, 1, 18),
            line(11, 1.15F, 12),
            line(11, 1.15F, 14)};
}
} // namespace astra::ui
