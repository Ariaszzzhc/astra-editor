-- Official generated C parsers; scripts/prepare-grammars.ps1 verifies the pinned
-- archives and every compiled source/header against grammars/manifest.json.
local grammar_specs = {
    {"c", "tree-sitter-c-b780e47fc780ddc8da13afa35a3f4ed5c157823d", "src", false},
    {"cpp", "tree-sitter-cpp-f41e1a044c8a84ea9fa8577fdd2eab92ec96de02", "src", true},
    {"javascript", "tree-sitter-javascript-44c892e0be055ac465d5eeddae6d3e194424e7de", "src", true},
    {"typescript", "tree-sitter-typescript-f975a621f4e7f532fe322e13c4f79495e0a7b2e7", "typescript/src", true},
    {"tsx", "tree-sitter-typescript-f975a621f4e7f532fe322e13c4f79495e0a7b2e7", "tsx/src", true},
    {"json", "tree-sitter-json-ee35a6ebefcef0c5c416c0d1ccec7370cfca5a24", "src", false},
    {"python", "tree-sitter-python-293fdc02038ee2bf0e2e206711b69c90ac0d413f", "src", true},
    {"markdown", "tree-sitter-markdown-f969cd3ae3f9fbd4e43205431d0ae286014c05b5", "tree-sitter-markdown/src", true},
    {"markdown-inline", "tree-sitter-markdown-f969cd3ae3f9fbd4e43205431d0ae286014c05b5", "tree-sitter-markdown-inline/src", true}
}
for _, spec in ipairs(grammar_specs) do
    target("astra-grammar-" .. spec[1])
        set_kind("static")
        set_default(false)
        set_languages("c11")
        -- Generated third-party C has its own upstream warning policy.
        set_warnings("none")
        local source = path.join(os.projectdir(), ".tools/grammars", spec[2], spec[3])
        add_includedirs(source)
        add_files(path.join(source, "parser.c"))
        if spec[4] then
            add_files(path.join(source, "scanner.c"))
        end
        add_cflags("/bigobj", {tools = {"cl", "clang_cl"}})
end
