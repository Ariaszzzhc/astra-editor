#include <astra/config/editor_config.hpp>

#include <doctest/doctest.h>

#include <array>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using astra::config::Charset;
using astra::config::EditorConfigLayer;
using astra::config::glob_matches;
using astra::config::IndentStyle;
using astra::config::LineEnding;
using astra::config::parse_editor_config;
using astra::config::resolve_editor_config;

auto resolve(std::string_view text, std::string_view path = "test.cpp") {
    const std::array layers{EditorConfigLayer{"", parse_editor_config(text)}};
    return resolve_editor_config(layers, path);
}

} // namespace

TEST_CASE("EditorConfig parses BOM CRLF comments and case-insensitive standard keys") {
    const auto config = parse_editor_config(
        "\xef\xbb\xbf  # comment\r\n ROOT = TrUe\r\nignored = preamble\r\n\r\n"
        "[*.cpp]\r\nIndent_Style = SpAcE\r\nindent_size = 4\r\nCustom_Key = MiXeD ; # value\r\n");
    CHECK(config.root);
    CHECK(config.diagnostics.empty());
    REQUIRE(config.sections.size() == 1);
    CHECK(config.sections[0].pattern == "*.cpp");
    CHECK(config.sections[0].line == 5);
    REQUIRE(config.sections[0].properties.size() == 3);
    CHECK(config.sections[0].properties[0].key == "indent_style");
    CHECK(config.sections[0].properties[0].value == "space");
    CHECK(config.sections[0].properties[2].key == "custom_key");
    CHECK(config.sections[0].properties[2].value == "MiXeD ; # value");
}

TEST_CASE("EditorConfig reports invalid lines without losing valid pairs") {
    const auto config = parse_editor_config(
        "root=true\n[*.txt]\nwrong\n = bad\nvalue=\ncustom=a=b\n[unterminated\n");
    REQUIRE(config.diagnostics.size() == 3);
    CHECK(config.diagnostics[0].line == 3);
    CHECK(config.diagnostics[1].line == 4);
    CHECK(config.diagnostics[2].line == 7);
    REQUIRE(config.sections[0].properties.size() == 2);
    CHECK(config.sections[0].properties[0].value.empty());
    CHECK(config.sections[0].properties[1].value == "a=b");
    CHECK_FALSE(parse_editor_config("# empty\n; empty\n").root);
    CHECK(parse_editor_config("").sections.empty());
}

TEST_CASE("EditorConfig glob stars and question marks respect path components") {
    CHECK(glob_matches("*.cpp", "src/deep/main.cpp"));
    CHECK(glob_matches("*.cpp", "main.cpp"));
    CHECK_FALSE(glob_matches("*.cpp", "main.CPP"));
    CHECK(glob_matches("*", ".gitignore"));
    CHECK(glob_matches("a?c", "abc"));
    CHECK_FALSE(glob_matches("a?c", "ac"));
    CHECK_FALSE(glob_matches("a?c", "abbc"));
    CHECK_FALSE(glob_matches("a?c", "a/c"));
    CHECK(glob_matches("src/*.cpp", "src/main.cpp"));
    CHECK_FALSE(glob_matches("src/*.cpp", "src/deep/main.cpp"));
    CHECK_FALSE(glob_matches("src/*.cpp", "lib/src/main.cpp"));
    CHECK(glob_matches("/src/*.cpp", "src/main.cpp"));
    CHECK_FALSE(glob_matches("/main.cpp", "src/main.cpp"));
    CHECK_FALSE(glob_matches("src/", "src/file"));
}

TEST_CASE("EditorConfig recursive globs include zero or many directories") {
    CHECK(glob_matches("**/main.cpp", "main.cpp"));
    CHECK(glob_matches("**/main.cpp", "src/main.cpp"));
    CHECK(glob_matches("**/main.cpp", "src/deep/main.cpp"));
    CHECK(glob_matches("src/**/main.cpp", "src/main.cpp"));
    CHECK(glob_matches("src/**/main.cpp", "src/a/b/main.cpp"));
    CHECK_FALSE(glob_matches("src/**/main.cpp", "src/somemain.cpp"));
    CHECK(glob_matches("a**z.c", "a/mn/z.c"));
    CHECK(glob_matches("a**z.c", "amnz.c"));
    CHECK(glob_matches("b/**z.c", "b/z.c"));
    CHECK_FALSE(glob_matches("b/**z.c", "bmnz.c"));
    CHECK(glob_matches("c**/z.c", "cmn/z.c"));
    CHECK_FALSE(glob_matches("c**/z.c", "cz.c"));
}

TEST_CASE("EditorConfig glob character sets negation and escaped literals") {
    CHECK(glob_matches("[abc].txt", "b.txt"));
    CHECK_FALSE(glob_matches("[abc].txt", "d.txt"));
    CHECK(glob_matches("[!abc].txt", "d.txt"));
    CHECK_FALSE(glob_matches("[!abc].txt", "a.txt"));
    CHECK(glob_matches("[a-f].txt", "c.txt"));
    CHECK_FALSE(glob_matches("[a-f].txt", "h.txt"));
    CHECK(glob_matches("[-ab].txt", "-.txt"));
    CHECK(glob_matches("[]].txt", "].txt"));
    CHECK(glob_matches("[!]].txt", "a.txt"));
    CHECK(glob_matches("ab[e/]cd.i", "ab[e/]cd.i"));
    CHECK_FALSE(glob_matches("ab[e/]cd.i", "abecd.i"));
    CHECK_FALSE(glob_matches("ab[e/]cd.i", "ab/cd.i"));
    CHECK(glob_matches(R"(literal\*.txt)", "literal*.txt"));
    CHECK_FALSE(glob_matches(R"(literal\*.txt)", "literala.txt"));
    CHECK(glob_matches(R"(file\[x\].txt)", "file[x].txt"));
    CHECK(glob_matches("[ab*c{1..2}]", "*"));
    CHECK(glob_matches("[ab*c{1..2}]", "{"));
}

TEST_CASE("EditorConfig alternatives support nesting empty branches and escaping") {
    CHECK(glob_matches("*.{cpp,hpp}", "src/main.cpp"));
    CHECK(glob_matches("*.{cpp,hpp}", "main.hpp"));
    CHECK_FALSE(glob_matches("*.{cpp,hpp}", "main.c"));
    CHECK(glob_matches("{{a,b},c}.txt", "a.txt"));
    CHECK(glob_matches("{a,{b,c}}.txt", "c.txt"));
    CHECK(glob_matches("a{,b,,c,}.txt", "a.txt"));
    CHECK(glob_matches("{single}.txt", "{single}.txt"));
    CHECK_FALSE(glob_matches("{single}.txt", "single.txt"));
    CHECK(glob_matches("{}.txt", "{}.txt"));
    CHECK_FALSE(glob_matches("{}.txt", ".txt"));
    CHECK(glob_matches("{word,{also},this}.g", "{also}.g"));
    CHECK(glob_matches(R"({a\,b,cd}.txt)", "a,b.txt"));
    CHECK(glob_matches(R"({e,\},f}.txt)", "}.txt"));
    CHECK(glob_matches("{some,a{*c,b}[ef]}.j", "abce.j"));
    CHECK_FALSE(glob_matches("{some,a{*c,b}[ef]}.j", "abcg.j"));
    CHECK(glob_matches("{{,b,c{d}.i", "{{,b,c{d}.i"));
}

TEST_CASE("EditorConfig integer glob ranges do not expand large ranges") {
    CHECK(glob_matches("file{1..3}.cpp", "file1.cpp"));
    CHECK(glob_matches("file{1..3}.cpp", "file3.cpp"));
    CHECK_FALSE(glob_matches("file{1..3}.cpp", "file4.cpp"));
    CHECK(glob_matches("{-10..10}", "-8"));
    CHECK(glob_matches("{-10..10}", "0"));
    CHECK_FALSE(glob_matches("{-10..10}", "-0"));
    CHECK_FALSE(glob_matches("{3..120}", "060"));
    CHECK(glob_matches("{1..999999999999999999}", "800000000000000000"));
    CHECK(glob_matches("{1..20}0", "120"));
    CHECK(glob_matches("{3..1}", "{3..1}"));
    CHECK_FALSE(glob_matches("{3..1}", "2"));
    CHECK(glob_matches("{a..c}", "{a..c}"));
    CHECK_FALSE(glob_matches("{a..c}", "b"));
}

TEST_CASE("EditorConfig Unicode globs match scalars and preserve case") {
    CHECK(glob_matches("?.txt", "\xf0\x9f\x98\x80.txt"));
    CHECK(glob_matches("[\xe4\xb8\xad\xe6\x96\x87].txt", "\xe4\xb8\xad.txt"));
    CHECK(glob_matches("\xe4\xb8\xad*.txt", "src/\xe4\xb8\xad\xe6\x96\x87.txt"));
    CHECK_FALSE(glob_matches("?.txt", "e\xcc\x81.txt"));
    CHECK_FALSE(glob_matches("FILE.txt", "file.txt"));
}

TEST_CASE("EditorConfig nearest sections and layers override earlier properties") {
    const std::array layers{
        EditorConfigLayer{
            "", parse_editor_config("[*]\nindent_size=8\ncharset=utf-8\n[*.cpp]\nindent_size=4\n")},
        EditorConfigLayer{
            "src", parse_editor_config("[*]\nindent_size=2\n[deep/*.cpp]\nend_of_line=crlf\n")},
        EditorConfigLayer{"unrelated", parse_editor_config("root=true\n[*]\nindent_size=99\n")},
    };
    const auto result = resolve_editor_config(layers, "src/deep/test.cpp");
    CHECK(result.settings.indent_size == 2);
    CHECK(result.settings.tab_width == 2);
    CHECK(result.settings.charset == Charset::utf8);
    CHECK(result.settings.end_of_line == LineEnding::crlf);
    CHECK(result.properties.at("indent_size") == "2");
    CHECK(resolve_editor_config(layers, "src2/test.cpp").settings.indent_size == 4);
}

TEST_CASE("EditorConfig root applies only in the preamble of applicable ancestors") {
    const std::array layers{
        EditorConfigLayer{"", parse_editor_config("[*]\nindent_size=8\ncharset=utf-8\n")},
        EditorConfigLayer{"src",
                          parse_editor_config("root=TRUE\n[*.cpp]\nindent_size=2\nroot=false\n")},
    };
    const auto result = resolve_editor_config(layers, "src/main.cpp");
    CHECK(result.settings.indent_size == 2);
    CHECK_FALSE(result.settings.charset.has_value());
    CHECK(result.properties.at("root") == "false");
    const auto unmatched = resolve_editor_config(layers, "src/README.md");
    CHECK(unmatched.properties.empty());
    const auto root_in_section = parse_editor_config("[*]\nroot=true\n");
    CHECK_FALSE(root_in_section.root);
}

TEST_CASE("EditorConfig unset removes effects and inhibits implicit indent inheritance") {
    const std::array layers{
        EditorConfigLayer{
            "",
            parse_editor_config(
                "[*]\nindent_style=tab\nindent_size=4\ncharset=utf-8\ncustom=value\n")},
        EditorConfigLayer{
            "src", parse_editor_config("[*]\nindent_size=UNSET\ncharset=unset\ncustom=unset\n")},
    };
    const auto result = resolve_editor_config(layers, "src/main.cpp");
    CHECK(result.settings.indent_style == IndentStyle::tabs);
    CHECK_FALSE(result.settings.indent_size.has_value());
    CHECK_FALSE(result.settings.tab_width.has_value());
    CHECK_FALSE(result.settings.indent_size_uses_tab);
    CHECK_FALSE(result.settings.charset.has_value());
    CHECK_FALSE(result.properties.contains("custom"));
    const auto overwritten = resolve("[*]\nindent_size=unset\nindent_size=3\n");
    CHECK(overwritten.settings.indent_size == 3);
    CHECK(overwritten.settings.tab_width == 3);
}

TEST_CASE("EditorConfig indent and tab widths resolve without overriding editor defaults") {
    SUBCASE("numeric indent supplies default tab width") {
        const auto result = resolve("[*]\nindent_style=space\nindent_size=3\n");
        CHECK(result.settings.indent_style == IndentStyle::spaces);
        CHECK(result.settings.indent_size == 3);
        CHECK(result.settings.tab_width == 3);
    }
    SUBCASE("explicit widths differ") {
        const auto result = resolve("[*]\nindent_style=tab\nindent_size=4\ntab_width=3\n");
        CHECK(result.settings.indent_size == 4);
        CHECK(result.settings.tab_width == 3);
    }
    SUBCASE("tab indent takes tab width") {
        const auto result = resolve("[*]\nindent_size=TAB\ntab_width=8\n");
        CHECK(result.settings.indent_size_uses_tab);
        CHECK(result.settings.indent_size == 8);
    }
    SUBCASE("tab style supplies implicit tab indent") {
        const auto result = resolve("[*]\nindent_style=tab\n");
        CHECK(result.settings.indent_size_uses_tab);
        CHECK_FALSE(result.settings.indent_size.has_value());
        CHECK_FALSE(result.settings.tab_width.has_value());
    }
    SUBCASE("tab width unset remains an editor default") {
        const auto result = resolve("[*]\nindent_size=4\ntab_width=unset\n");
        CHECK(result.settings.indent_size == 4);
        CHECK_FALSE(result.settings.tab_width.has_value());
    }
}

TEST_CASE("EditorConfig resolves save policy flags encodings and spelling language") {
    const auto result = resolve("[*]\nend_of_line=CRLF\ncharset=UTF-16LE\n"
                                "trim_trailing_whitespace=TRUE\ninsert_final_newline="
                                "FALSE\nspelling_language=en-US\nmax_line_length=100\n");
    CHECK(result.settings.end_of_line == LineEnding::crlf);
    CHECK(result.settings.charset == Charset::utf16_le);
    CHECK(result.settings.trim_trailing_whitespace == true);
    CHECK(result.settings.insert_final_newline == false);
    CHECK(result.settings.spelling_language == "en-US");
    CHECK(result.settings.max_line_length == 100);
    CHECK(resolve("[*]\ncharset=latin1\n").settings.charset == Charset::latin1);
    CHECK(resolve("[*]\ncharset=utf-8-bom\n").settings.charset == Charset::utf8_bom);
    CHECK(resolve("[*]\ncharset=utf-16be\n").settings.charset == Charset::utf16_be);
    CHECK(resolve("[*]\nend_of_line=cr\n").settings.end_of_line == LineEnding::cr);
    CHECK(resolve("[*]\nend_of_line=lf\n").settings.end_of_line == LineEnding::lf);
}

TEST_CASE("EditorConfig typed settings ignore invalid values while preserving raw pairs") {
    const auto result =
        resolve("[*]\nindent_style=other\nindent_size=0\ntab_width=-3\n"
                "charset=unknown\nend_of_line=CRLF ; comment\ntrim_trailing_whitespace=yes\n"
                "insert_final_newline=1\nspelling_language=english\nmax_line_length="
                "4294967296\ncustom=MiXeD\n");
    CHECK_FALSE(result.settings.indent_style.has_value());
    CHECK_FALSE(result.settings.indent_size.has_value());
    CHECK_FALSE(result.settings.tab_width.has_value());
    CHECK_FALSE(result.settings.charset.has_value());
    CHECK_FALSE(result.settings.end_of_line.has_value());
    CHECK_FALSE(result.settings.trim_trailing_whitespace.has_value());
    CHECK_FALSE(result.settings.insert_final_newline.has_value());
    CHECK_FALSE(result.settings.spelling_language.has_value());
    CHECK_FALSE(result.settings.max_line_length.has_value());
    CHECK(result.properties.at("custom") == "MiXeD");
    CHECK(result.properties.at("end_of_line") == "crlf ; comment");
}

TEST_CASE("EditorConfig honors required minimum key value and section lengths") {
    const std::string key(1024, 'k');
    const std::string value(4096, 'v');
    const std::string pattern(1024, 'a');
    const auto config = parse_editor_config("[" + pattern + "]\n" + key + "=" + value);
    REQUIRE(config.sections.size() == 1);
    REQUIRE(config.sections[0].properties.size() == 1);
    CHECK(config.sections[0].properties[0].key == key);
    CHECK(config.sections[0].properties[0].value == value);
    CHECK(glob_matches(pattern, pattern));
}

TEST_CASE("EditorConfig enforces input and evaluation limits without regex backtracking") {
    CHECK_THROWS_AS((void)parse_editor_config(std::string(1024U * 1024U + 1, 'x')),
                    std::length_error);
    CHECK_THROWS_AS((void)parse_editor_config("\xc0\xaf"), std::invalid_argument);
    CHECK_THROWS_AS((void)parse_editor_config(std::string("a\0b", 3)), std::invalid_argument);
    CHECK_THROWS_AS((void)glob_matches(std::string(4097, '*'), "a"), std::length_error);
    CHECK_THROWS_AS((void)glob_matches("*", "../file"), std::invalid_argument);
    CHECK_THROWS_AS((void)glob_matches("*", R"(dir\file)"), std::invalid_argument);
    CHECK_THROWS_AS((void)glob_matches("*", "C:/file"), std::invalid_argument);
    CHECK_THROWS_AS((void)glob_matches("*", "/file"), std::invalid_argument);
    CHECK_THROWS_AS((void)glob_matches(std::string(4096, '?'), std::string(32768, 'a')),
                    std::length_error);
}

TEST_CASE("EditorConfig random alternative ranges agree with explicit alternatives") {
    constexpr unsigned int seed = 0x45444346U;
    INFO("seed = ", seed);
    std::mt19937 random(seed);
    for (std::size_t iteration = 0; iteration < 1200; ++iteration) {
        const int minimum = static_cast<int>(random() % 20) - 10;
        const int maximum = minimum + 1 + static_cast<int>(random() % 10);
        const int value = static_cast<int>(random() % 40) - 15;
        const std::string prefix = random() % 2 == 0 ? "src/**/file" : "file";
        const std::string pattern =
            prefix + "{" + std::to_string(minimum) + ".." + std::to_string(maximum) + "}.{cpp,hpp}";
        const std::string path = (prefix.starts_with("src") ? "src/deep/file" : "other/file") +
                                 std::to_string(value) + ".cpp";
        CHECK(glob_matches(pattern, path) == (value >= minimum && value <= maximum));
    }
}
