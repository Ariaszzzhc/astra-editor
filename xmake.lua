set_project("astra")
set_version("0.1.0")
set_xmakever("3.1.1")
set_languages("c++23")
set_encodings("utf-8")
set_warnings("allextra", "error")
-- Freeze the whole recipe tree, including hashes, in dependencies.lock.json.
-- xmake 3.1.1's additional Git partial-clone lock path is not used.
set_policy("package.requires_lock", false)
add_repositories("astra-pinned .tools/recipes/xmake-repo-e567b25f0575b952c47b2000e0dedbd6fa698b2c")
add_rules("mode.debug", "mode.release")

if is_plat("windows") then
    -- Skia's bundled Dawn uses /MT in both configurations. Keep one CRT ABI.
    set_runtimes("MT")
    add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN", "_ITERATOR_DEBUG_LEVEL=0")
    add_cxxflags("/permissive-", {tools = {"cl", "clang_cl"}})
    if is_mode("debug") then
        -- Embed CodeView records in objects; avoid a shared compiler PDB server.
        set_symbols("debug", "embed")
    end
end

add_requires("doctest 2.4.12", {system = false})
add_requires("nlohmann_json 3.12.0", {system = false})

target("astra-core")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_headerfiles("include/(astra/core/*.hpp)")
    add_files("src/core/*.cpp")
    set_exceptions("cxx")

target("astra-core-tests")
    set_kind("binary")
    set_default(false)
    add_deps("astra-core")
    add_packages("doctest")
    add_files("tests/main.cpp", "tests/sum_tree_tests.cpp", "tests/rope_tests.cpp", "tests/document_tests.cpp")
    set_exceptions("cxx")
    add_tests("core", {runargs = {"--no-version", "--duration=true"}})

target("astra-tests")
    set_kind("binary")
    set_default(true)
    add_deps("astra-core", "astra-io", "astra-config")
    add_packages("doctest")
    add_files("tests/main.cpp", "tests/sum_tree_tests.cpp", "tests/rope_tests.cpp", "tests/document_tests.cpp", "tests/file_store_tests.cpp", "tests/editor_config_tests.cpp")
    set_exceptions("cxx")
    add_tests("unit", {runargs = {"--no-version", "--duration=true"}})

target("astra-io")
    set_kind("static")
    add_deps("astra-core")
    add_includedirs("include", {public = true})
    add_headerfiles("include/(astra/io/*.hpp)")
    add_files("src/io/*.cpp")
    set_exceptions("cxx")

target("astra-config")
    set_kind("static")
    add_deps("astra-core")
    add_includedirs("include", {public = true})
    add_headerfiles("include/(astra/config/*.hpp)")
    add_files("src/config/*.cpp")
    set_exceptions("cxx")
