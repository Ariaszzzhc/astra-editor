#include <astra/ui/workbench_style.hpp>
#include <astra/core/utf8.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace astra::ui {
namespace {
using Json = protocol::Json;

std::uint32_t color(const Json& theme, std::string_view key) {
    const auto& value = theme.at(std::string(key));
    if (!value.is_string()) {
        throw std::invalid_argument("Theme color must be a #RRGGBB string: " + std::string(key));
    }
    const auto& text = value.get_ref<const std::string&>();
    if (text.size() != 7 || text.front() != '#') {
        throw std::invalid_argument("Theme color must be #RRGGBB: " + std::string(key));
    }
    std::uint32_t rgb{};
    for (const auto digit : std::string_view(text).substr(1)) {
        unsigned nibble{};
        if (digit >= '0' && digit <= '9') {
            nibble = static_cast<unsigned>(digit - '0');
        } else if (digit >= 'a' && digit <= 'f') {
            nibble = static_cast<unsigned>(digit - 'a') + 10;
        } else if (digit >= 'A' && digit <= 'F') {
            nibble = static_cast<unsigned>(digit - 'A') + 10;
        } else {
            throw std::invalid_argument("Theme color contains a non-hexadecimal digit: " +
                                        std::string(key));
        }
        rgb = (rgb << 4) | nibble;
    }
    return 0xFF000000U | rgb;
}

std::vector<std::string> families(const Json& workbench, std::string_view key) {
    const auto& value = workbench.at(std::string(key));
    if (!value.is_array() || value.empty() || value.size() > 8) {
        throw std::invalid_argument("UI font families require 1 to 8 names: " + std::string(key));
    }
    std::vector<std::string> result;
    for (const auto& entry : value) {
        const auto text = entry.get<std::string>();
        if (text.empty() || text.size() > 128 || text.find_first_of("\r\n") != text.npos ||
            text.find('\0') != text.npos) {
            throw std::invalid_argument("Invalid UI font family: " + std::string(key));
        }
        utf8::validate(text);
        result.push_back(text);
    }
    return result;
}
} // namespace

WorkbenchStyle WorkbenchStyle::compile(const Json& settings) {
    const auto& theme = settings.at("theme");
    const auto& workbench = settings.at("workbench");
    WorkbenchStyle result;
    result.name = theme.at("name").get<std::string>();
    if (result.name.empty() || result.name.size() > 128 ||
        result.name.find('\0') != result.name.npos) {
        throw std::invalid_argument("Theme name requires 1 to 128 UTF-8 bytes");
    }
    utf8::validate(result.name);
    const std::pair<std::string_view, std::uint32_t Colors::*> roles[] = {
        {"background", &Colors::background},
        {"foreground", &Colors::foreground},
        {"sidebar", &Colors::sidebar},
        {"activityBar", &Colors::activity},
        {"tabs", &Colors::tabs},
        {"panel", &Colors::panel},
        {"activeTab", &Colors::active_tab},
        {"border", &Colors::border},
        {"selection", &Colors::selection},
        {"accent", &Colors::accent},
        {"status", &Colors::status},
        {"muted", &Colors::muted},
        {"statusForeground", &Colors::status_foreground},
        {"hover", &Colors::hover},
        {"currentLine", &Colors::current_line},
        {"input", &Colors::input},
        {"cursor", &Colors::cursor},
        {"scrollbar", &Colors::scrollbar},
        {"folder", &Colors::folder},
        {"codeIcon", &Colors::code_icon},
        {"scriptIcon", &Colors::script_icon},
        {"added", &Colors::added},
        {"changed", &Colors::changed},
        {"noticeBackground", &Colors::notice_background},
        {"recoveryBackground", &Colors::recovery_background},
        {"errorBackground", &Colors::error_background},
        {"error", &Colors::error},
        {"warning", &Colors::warning}};
    for (const auto& [key, member] : roles) {
        result.colors.*member = color(theme, key);
    }
    for (const auto key : {"comment",
                           "string",
                           "number",
                           "keyword",
                           "type",
                           "function",
                           "property",
                           "variable",
                           "heading"}) {
        result.syntax.emplace(key, color(theme, key));
    }
    result.syntax.emplace("error", color(theme, "syntaxError"));
    result.font_size = workbench.at("uiFontSize").get<float>();
    const auto sidebar = workbench.at("sidebarWidth").get<float>();
    if (!std::isfinite(result.font_size) || result.font_size < 9 || result.font_size > 16 ||
        !std::isfinite(sidebar)) {
        throw std::invalid_argument(
            "UI font size requires 9 to 16 and sidebar width must be finite");
    }
    result.sidebar_width = std::clamp(sidebar, 180.0F, 480.0F);
    result.font_families = families(workbench, "uiFontFamilies");
    result.monospace_families = families(workbench, "uiMonospaceFontFamilies");
    return result;
}

TextLayoutOptions WorkbenchStyle::text(float reference_size,
                                       float dpi,
                                       std::uint32_t color,
                                       float line_height,
                                       bool monospace,
                                       float wrap_width) const {
    if (!std::isfinite(reference_size) || reference_size <= 0 || !std::isfinite(dpi) || dpi <= 0 ||
        !std::isfinite(line_height) || line_height < 1 || !std::isfinite(wrap_width) ||
        wrap_width < 0 || !std::isfinite(font_size) || font_size < 9 || font_size > 16 ||
        font_families.empty() || monospace_families.empty()) {
        throw std::invalid_argument("Invalid UI text geometry");
    }
    TextLayoutOptions result;
    result.font_size = reference_size * font_size / 13.0F;
    result.dpi_scale = dpi;
    result.line_height = line_height;
    result.color = color;
    result.direction = TextDirection::automatic;
    result.wrap_width = wrap_width;
    result.font_families = monospace ? monospace_families : font_families;
    return result;
}
} // namespace astra::ui
