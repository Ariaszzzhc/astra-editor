#pragma once

#include <astra/ui/workbench_layout.hpp>

namespace astra::ui {
class TextLabels;
struct WorkbenchStyle;

// Cached native line probes include CJK and emoji fallback. Pixel rounding is
// converted back to logical extents, so layout and input use one DPI transform.
[[nodiscard]] DialogMetrics measure_dialog_metrics(TextLabels&,
                                                   const WorkbenchStyle&,
                                                   float dpi,
                                                   bool monospace_detail = false);
} // namespace astra::ui
