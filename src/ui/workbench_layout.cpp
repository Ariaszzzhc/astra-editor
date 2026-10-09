#include <astra/ui/workbench_layout.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace astra::ui {
ConfirmationLayout ConfirmationLayout::measure(float width,float height,float scale,bool has_items) {
    if(!std::isfinite(width)||!std::isfinite(height)||!std::isfinite(scale)||width<=0||height<=0||scale<=0)throw std::invalid_argument("Invalid confirmation dimensions");
    ConfirmationLayout result;
    const auto pw=std::max(0.0F,std::min(600*scale,width-24*scale));
    const auto ph=std::max(0.0F,std::min((has_items?430.0F:175.0F)*scale,height-24*scale));
    const auto x=(width-pw)/2,y=std::min(90*scale,std::max(0.0F,height-ph-12*scale));
    result.panel={x,y,pw,ph};
    const auto padding=std::min(24*scale,pw/6),button_y=std::max(y,result.panel.bottom()-54*scale);
    const auto bw=std::max(0.0F,(pw-3*padding)/2);
    result.accept={x+padding,button_y,bw,std::min(34*scale,result.panel.bottom()-button_y)};
    result.cancel={x+2*padding+bw,button_y,bw,result.accept.height};
    const auto title_y=std::min(y+18*scale,button_y);
    result.title={x+padding,title_y,pw-2*padding,std::min(24*scale,std::max(0.0F,button_y-title_y))};
    const auto dy=std::min(y+55*scale,button_y);
    result.detail={x+padding,dy,pw-2*padding,std::min(58*scale,std::max(0.0F,button_y-dy-8*scale))};
    const auto iy=std::min(result.detail.bottom()+8*scale,button_y);
    result.items={x+padding,iy,pw-2*padding,has_items?std::max(0.0F,button_y-iy-12*scale):0.0F};
    return result;
}
WorkbenchLayout WorkbenchLayout::measure(float width, float height, float scale,
                                         bool sidebar, float sidebar_width, bool outline, bool explorer) {
    if (!std::isfinite(width) || !std::isfinite(height) || !std::isfinite(scale) ||
        !std::isfinite(sidebar_width) || width <= 0 || height <= 0 || scale <= 0) { throw std::invalid_argument("Invalid workbench dimensions"); }
    const auto logical_width = width / scale;
    const auto logical_height = height / scale;
    // Keep the document usable when a narrow window cannot fit the requested sidebar.
    const auto activity_width = std::min(44.0F, logical_width);
    const auto available = std::max(0.0F, logical_width - activity_width - 240.0F);
    const auto side = sidebar && available>=180.0F ? std::min(std::clamp(sidebar_width, 180.0F, 480.0F), available) : 0;
    const auto top = std::min(32.0F, logical_height);
    const auto status_top = std::max(top, logical_height - 24.0F);
    const auto tab_bottom = std::min(status_top, top + 34.0F);
    const auto text_top = std::min(status_top, tab_bottom + 24.0F);
    const auto left = activity_width + side;
    const auto box = [scale](float x, float y, float w, float h) { return Box{x*scale, y*scale, std::max(0.0F,w)*scale, std::max(0.0F,h)*scale}; };
    WorkbenchLayout result;
    result.scale = scale; result.sidebar_row_height = 24 * scale;
    result.header = box(0, 0, logical_width, top);
    result.activity = box(0, top, activity_width, status_top - top);
    result.sidebar = box(activity_width, top, side, status_top - top);
    result.tabs = box(left, top, logical_width-left, tab_bottom-top);
    result.breadcrumb = box(left, tab_bottom, logical_width-left, text_top-tab_bottom);
    result.editor = box(left, text_top, logical_width-left, status_top-text_top);
    result.status = box(0, status_top, logical_width, logical_height-status_top);
    const auto outline_header = explorer && side > 0 ? std::max(std::min(top + 56.0F, status_top), status_top - (outline ? std::min(220.0F, (status_top-top)*0.4F) : 26.0F)) : status_top;
    result.sidebar_rows = box(activity_width, std::min(top+56.0F,status_top), side, outline_header - std::min(top+56.0F,status_top));
    result.outline_header = box(activity_width, outline_header, side, explorer ? std::min(26.0F,status_top-outline_header) : 0);
    result.outline_rows = box(activity_width, result.outline_header.bottom()/scale, side, outline && explorer ? status_top-result.outline_header.bottom()/scale : 0);
    result.sidebar_handle = box(left-3, top, side > 0 ? 6.0F : 0.0F, status_top-top);
    return result;
}

TabStrip TabStrip::measure(Box bounds, float scale, const std::vector<float>& labels,
                           std::size_t first, std::size_t active, bool reveal) {
    TabStrip result;
    if (labels.empty()) { return result; }
    if (scale <= 0 || !std::isfinite(scale)) { throw std::invalid_argument("Invalid tab scale"); }
    std::vector<float> widths;
    float total{};
    for (const auto width : labels) {
        if (!std::isfinite(width) || width < 0) { throw std::invalid_argument("Invalid tab label width"); }
        widths.push_back(std::clamp(width + 64*scale, 110*scale, 260*scale)); total += widths.back();
    }
    const bool overflow = total > bounds.width;
    const auto available = std::max(0.0F, bounds.width - (overflow ? 52*scale : 0));
    result.first = std::min(first, labels.size()-1);
    active = std::min(active, labels.size()-1);
    if (reveal) {
        if (active < result.first) { result.first = active; }
        float length{};
        for (auto i=result.first; i<=active; ++i) { length += widths[i]; }
        while (result.first < active && length > available) { length -= widths[result.first++]; }
    }
    float x = bounds.x;
    for (auto i=result.first; i<labels.size() && x < bounds.x + available; ++i) {
        const auto width = std::min(widths[i], bounds.x+available-x);
        if (width < 24*scale) { break; }
        result.visible.push_back({i, {x,bounds.y,width,bounds.height}, {x+width-26*scale,bounds.y,26*scale,bounds.height}});
        x += width;
    }
    if (overflow) {
        result.previous = {bounds.right()-52*scale,bounds.y,26*scale,bounds.height};
        result.next = {bounds.right()-26*scale,bounds.y,26*scale,bounds.height};
    }
    return result;
}
} // namespace astra::ui
