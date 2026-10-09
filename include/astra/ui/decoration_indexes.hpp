#pragma once

#include <astra/ui/range_index.hpp>
#include <astra/protocol/rpc.hpp>

#include <cstddef>
#include <string>

namespace astra::ui {
struct DiagnosticColumns {
    std::size_t start_utf16{}, end_utf16{};
    int severity{1};
    friend bool operator==(const DiagnosticColumns&, const DiagnosticColumns&) = default;
};
struct DecorationIndexes {
    RangeIndex<std::string, Utf8ByteUnit> highlights;
    RangeIndex<DiagnosticColumns, DocumentLineUnit> diagnostics;
    RangeIndex<std::string, DocumentLineUnit> gutter;
    void set_highlights(const protocol::Json& values);
    void set_diagnostics(const protocol::Json& values);
    void set_gutter(const protocol::Json& values);
};
} // namespace astra::ui
