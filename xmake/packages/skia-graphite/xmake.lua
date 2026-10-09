-- Include this recipe from the root project. The repository path is part of the
-- package configuration, so packages from different checkouts cannot be confused.
package("skia-graphite")
    set_homepage("https://skia.org/")
    set_description("Pinned Skia Graphite, Dawn D3D12, and text layout libraries")
    set_license("BSD-3-Clause")
    add_configs("root", {description = "Astra repository root", type = "string"})
    add_configs("configuration", {description = "Graphics build configuration", default = "Release", values = {"Debug", "Release"}})

    on_load(function (package)
        assert(package:is_plat("windows") and package:is_arch("x64"),
               "skia-graphite is currently implemented and validated only for Windows x64")
        assert(package:config("root"), "skia-graphite requires configs.root = os.projectdir()")
        assert(not package:config("shared"), "The pinned graphics package is static")
        local runtimes = package:runtimes()
        assert(not runtimes or runtimes == "MT",
               "The pinned Skia/Dawn package requires the MT runtime, including Debug builds")
    end)

    on_fetch(function (package)
        import("core.base.json")
        local root = path.absolute(package:config("root"))
        local configuration = package:config("configuration"):lower()
        local prefix = path.join(root, ".tools", "graphics", "install", "x64", configuration)
        local manifestfile = path.join(prefix, "manifest.json")
        if not os.isfile(manifestfile) then
            return
        end
        local manifest = json.loadfile(manifestfile)
        assert(manifest.schema == 1 and manifest.platform == "windows" and manifest.architecture == "x64",
               "Invalid graphics package manifest")
        assert(manifest.skia == "525f94ac3418fd0b0dae42caa220422a94b18b49" and
               manifest.dawn == "ab3c12305e776fa707503f7ecfab07364e9c7697",
               "Graphics package does not match the pinned Skia/Dawn revisions")
        assert(manifest.runtime == "MT" and manifest.iterator_debug_level == 0,
               "Graphics package has incompatible CRT settings")
        for _, artifact in ipairs(manifest.artifacts) do
            local file = path.join(prefix, artifact.file)
            assert(os.isfile(file), "Missing graphics artifact: " .. file)
            assert(hash.sha256(file) == artifact.sha256,
                   "Graphics artifact checksum mismatch: " .. file)
        end
        local includedirs = {}
        for _, directory in ipairs(manifest.includes) do
            table.insert(includedirs, path.join(prefix, directory))
        end
        local runtimefiles = {}
        for _, file in ipairs(manifest.runtime_files) do
            table.insert(runtimefiles, path.join(prefix, file))
        end
        return {
            includedirs = includedirs,
            linkdirs = {path.join(prefix, "lib")},
            links = manifest.libraries,
            syslinks = manifest.syslinks,
            defines = manifest.defines,
            extras = {astra_graphics_runtime_files = runtimefiles}
        }
    end)

    on_install("windows", function (package)
        import("lib.detect.find_program")
        local powershell = assert(find_program("pwsh"), "PowerShell 7 is required to build graphics")
        local root = path.absolute(package:config("root"))
        os.vrunv(powershell, {
            "-NoProfile", "-File", path.join(root, "scripts", "build-graphics.ps1"),
            "-Stage", "Build", "-Configuration", package:config("configuration")
        }, {curdir = root})
        -- Keep an installation marker in xmake's local package cache. The manifest
        -- is also checked by on_fetch before exposing any include/link settings.
        io.writefile(path.join(package:installdir(), "astra-graphics-root.txt"), root)
    end)

    on_test(function (package)
        assert(package:check_cxxsnippets({test = [[
            #include "include/gpu/graphite/Context.h"
            #include "include/gpu/graphite/dawn/DawnBackendContext.h"
            #include "dawn/native/DawnNative.h"
            #include "dawn/dawn_proc.h"
            int main() {
                auto proc = dawn::native::GetProcs();
                dawnProcSetProcs(&proc);
                auto make_context = &skgpu::graphite::ContextFactory::MakeDawn;
                return make_context == nullptr;
            }
        ]]}, {configs = {languages = "c++23", runtimes = "MT"}}))
    end)
package_end()

rule("astra.graphics-runtime")
    on_load(function (target)
        target:add("ldflags", "/WHOLEARCHIVE:dawn_combined.lib", {force = true})
    end)
    after_build(function (target)
        local package = assert(target:pkg("skia-graphite"), "The graphics runtime rule requires skia-graphite")
        for _, file in ipairs(package:extraconf("astra_graphics_runtime_files")) do
            os.cp(file, target:targetdir())
        end
    end)
    after_install(function (target)
        local package = assert(target:pkg("skia-graphite"), "The graphics runtime rule requires skia-graphite")
        for _, file in ipairs(package:extraconf("astra_graphics_runtime_files")) do
            os.cp(file, path.join(target:installdir(), "bin"))
        end
    end)
rule_end()
