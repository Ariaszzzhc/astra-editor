-- Include this recipe from the root project. The repository path is part of the
-- package configuration, so packages from different checkouts cannot be confused.
package("skia-graphite")
    set_homepage("https://skia.org/")
    set_description("Pinned Skia Graphite, native Dawn backend, and text layout libraries")
    set_license("BSD-3-Clause")
    add_configs("root", {description = "Astra repository root", type = "string"})
    add_configs("configuration", {description = "Graphics build configuration", default = "Release", values = {"Debug", "Release"}})

    on_load(function (package)
        assert(package:is_plat("windows", "linux", "macosx"), "Unsupported native graphics platform")
        assert(package:is_arch("x64", "x86_64", "arm64", "aarch64"), "Unsupported graphics architecture")
        if package:is_plat("windows") then
            assert(package:is_arch("x64"), "Windows graphics requires x64")
        end
        assert(package:config("root"), "skia-graphite requires configs.root = os.projectdir()")
        assert(not package:config("shared"), "The pinned graphics package is static")
        local runtimes = package:runtimes()
        assert(not package:is_plat("windows") or not runtimes or runtimes == "MT",
               "The pinned Skia/Dawn package requires the MT runtime, including Debug builds")
    end)

    on_fetch(function (package)
        import("core.base.json")
        local root = path.absolute(package:config("root"))
        local configuration = package:config("configuration"):lower()
        local arch = package:is_arch("x64", "x86_64") and "x64" or "arm64"
        local prefix = package:is_plat("windows")
            and path.join(root, ".tools", "graphics", "install", arch, configuration)
            or path.join(root, ".tools", "graphics", "install", package:plat(), arch, configuration)
        local manifestfile = path.join(prefix, "manifest.json")
        if not os.isfile(manifestfile) then
            return
        end
        local manifest = json.loadfile(manifestfile)
        assert(manifest.schema == 1 and manifest.platform == package:plat() and manifest.architecture == arch
               and manifest.configuration:lower() == configuration and manifest.trivial_abi == false,
               "Invalid graphics package manifest")
        assert(manifest.skia == "525f94ac3418fd0b0dae42caa220422a94b18b49" and
               manifest.dawn == "ab3c12305e776fa707503f7ecfab07364e9c7697",
               "Graphics package does not match the pinned Skia/Dawn revisions")
        if package:is_plat("windows") then
            assert(manifest.runtime == "MT" and manifest.iterator_debug_level == 0,
                   "Graphics package has incompatible CRT settings")
        else
            local expected = package:is_plat("macosx") and "libc++" or "libstdc++"
            assert(manifest.runtime == expected, "Graphics package has incompatible C++ standard library")
        end
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
        local result = {
            includedirs = includedirs,
            linkdirs = {path.join(prefix, "lib")},
            links = manifest.libraries,
            syslinks = manifest.syslinks,
            frameworks = manifest.frameworks,
            defines = manifest.defines,
            extras = {astra_graphics_runtime_files = runtimefiles}
        }
        -- GNU ld needs an archive group for mutually dependent Skia modules.
        -- xmake maps this to --start-group/--end-group; Apple ld needs neither.
        if package:is_plat("linux") then
            result.links = nil
            result.linkgroups = {manifest.libraries}
            result.extras.linkgroups = {[table.concat(manifest.libraries, "_")] = {group = true}}
        end
        return result
    end)

    on_install("linux", "macosx", function (package)
        import("lib.detect.find_program")
        local python = assert(find_program("python3"), "Python 3.12+ is required for the native graphics build")
        local root = path.absolute(package:config("root"))
        os.vrunv(python, {path.join(root, "scripts", "build-graphics.py"),
            "--configuration", package:config("configuration"), "--architecture", package:arch()}, {curdir = root})
        io.writefile(path.join(package:installdir(), "astra-graphics-root.txt"), root)
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
        local configs = {languages = "c++23"}
        if package:is_plat("windows") then configs.runtimes = "MT" end
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
        ]]}, {configs = configs}))
    end)
package_end()

rule("astra.graphics-runtime")
    on_load(function (target)
        if target:is_plat("windows") then
            target:add("ldflags", "/WHOLEARCHIVE:dawn_combined.lib", {force = true})
        elseif target:is_plat("macosx") then
            target:add("rpathdirs", "@executable_path")
        elseif target:is_plat("linux") then
            target:add("rpathdirs", "$ORIGIN")
        end
    end)
    after_build(function (target)
        local package = assert(target:pkg("skia-graphite"), "The graphics runtime rule requires skia-graphite")
        for _, file in ipairs(package:extraconf("astra_graphics_runtime_files")) do
            os.cp(file, target:targetdir())
        end
        local sdl = target:pkg("libsdl3") or target:pkg("astra-sdl3")
        if sdl then
            local pattern = target:is_plat("windows") and "bin/SDL3.dll"
                or (target:is_plat("macosx") and "lib/libSDL3*.dylib" or "lib/libSDL3.so*")
            local files = os.files(path.join(assert(sdl:installdir()), pattern))
            assert(#files > 0, "SDL3 shared runtime is missing from its package")
            for _, file in ipairs(files) do os.cp(file, target:targetdir()) end
        end
    end)
    after_install(function (target)
        local package = assert(target:pkg("skia-graphite"), "The graphics runtime rule requires skia-graphite")
        for _, file in ipairs(package:extraconf("astra_graphics_runtime_files")) do
            os.cp(file, path.join(target:installdir(), "bin"))
        end
        local sdl = target:pkg("libsdl3") or target:pkg("astra-sdl3")
        if sdl then
            local pattern = target:is_plat("windows") and "bin/SDL3.dll"
                or (target:is_plat("macosx") and "lib/libSDL3*.dylib" or "lib/libSDL3.so*")
            local files = os.files(path.join(assert(sdl:installdir()), pattern))
            assert(#files > 0, "SDL3 shared runtime is missing from its package")
            for _, file in ipairs(files) do os.cp(file, path.join(target:installdir(), "bin")) end
        end
    end)
rule_end()
