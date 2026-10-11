-- All operations live inside xmake sandbox callbacks; project-scope functions
-- do not inherit the callback environment.
rule("astra.resources")
    on_load(function(target)
        target:add("deps", "astra-clipboard-host", {inherit = false})
        if target:is_plat("windows") then target:add("deps", "astra-process-host", {inherit = false}) end
    end)
    after_build(function(target)
        local json = import("core.base.json")
        local destination = target:targetdir()
        os.mkdir(destination)
        local clipboard = assert(target:dep("astra-clipboard-host"), "Resources require the clipboard helper")
        assert(os.isfile(clipboard:targetfile()), "Build astra-clipboard-host before staging resources")
        if path.absolute(path.directory(clipboard:targetfile())) ~= path.absolute(destination) then
            os.cp(clipboard:targetfile(), destination)
        end
        os.cp(path.join(os.projectdir(), "data"), destination)
        if target:is_plat("windows") then
            local pointerfile = path.join(os.projectdir(), ".tools/runtime/windows-x64.json")
            assert(os.isfile(pointerfile), "Run scripts/prepare-windows-runtime.ps1 before building/installing Astra")
            local pointer = json.loadfile(pointerfile)
            assert(not path.is_absolute(pointer.directory) and pointer.directory:find(".tools/runtime/", 1, true) == 1
                and not pointer.directory:find("..", 1, true), "Invalid Windows runtime directory")
            local runtime = path.join(os.projectdir(), pointer.directory)
            assert(hash.sha256(path.join(runtime, "runtime-manifest.json")) == pointer.manifestSha256,
                "Runtime manifest changed; prepare the verified Windows runtime again")
            for _, name in ipairs({"tools", "licenses", "provenance", "THIRD-PARTY.md", "runtime-manifest.json"}) do
                os.cp(path.join(runtime, name), destination)
            end
            local helper = assert(target:dep("astra-process-host"), "Windows resources require astra-process-host")
            assert(os.isfile(helper:targetfile()), "Build astra-process-host before staging resources")
            if path.absolute(path.directory(helper:targetfile())) ~= path.absolute(destination) then
                os.cp(helper:targetfile(), destination)
            end
        elseif target:is_plat("linux", "macosx") then
            local architecture = target:is_arch("x64", "x86_64") and "x64" or "arm64"
            local key = target:plat() .. "-" .. architecture
            local pointerfile = path.join(os.projectdir(), ".tools/runtime/" .. key .. ".json")
            assert(os.isfile(pointerfile), "Run scripts/prepare-native-runtime.py on the native host before building/installing Astra")
            local pointer = json.loadfile(pointerfile)
            assert(not path.is_absolute(pointer.directory) and pointer.directory:find(".tools/runtime/", 1, true) == 1
                and not pointer.directory:find("..", 1, true), "Invalid native runtime directory")
            local runtime = path.join(os.projectdir(), pointer.directory)
            assert(hash.sha256(path.join(runtime, "runtime-manifest.json")) == pointer.manifestSha256,
                "Native runtime manifest changed; prepare it again")
            local manifest = json.loadfile(path.join(runtime, "runtime-manifest.json"))
            assert(manifest.platform == target:plat() and manifest.architecture == architecture,
                "Runtime platform/architecture differs from the native target")
            for _, name in ipairs({"tools", "licenses", "provenance", "THIRD-PARTY.md", "runtime-manifest.json"}) do
                os.cp(path.join(runtime, name), destination)
            end
        end
    end)
    after_install(function(target)
        local json = import("core.base.json")
        local destination = path.join(target:installdir(), "bin")
        os.mkdir(destination)
        local clipboard = assert(target:dep("astra-clipboard-host"), "Installed resources require the clipboard helper")
        os.cp(clipboard:targetfile(), destination)
        local sdl = clipboard:pkg("libsdl3") or clipboard:pkg("astra-sdl3")
        local pattern = target:is_plat("windows") and "bin/SDL3.dll"
            or (target:is_plat("macosx") and "lib/libSDL3*.dylib" or "lib/libSDL3.so*")
        local libraries = os.files(path.join(assert(sdl:installdir()), pattern))
        assert(#libraries > 0, "Clipboard helper requires the SDL shared runtime")
        for _, file in ipairs(libraries) do os.cp(file, destination) end
        os.cp(path.join(os.projectdir(), "data"), destination)
        if target:is_plat("windows") then
            local pointerfile = path.join(os.projectdir(), ".tools/runtime/windows-x64.json")
            assert(os.isfile(pointerfile), "Run scripts/prepare-windows-runtime.ps1 before building/installing Astra")
            local pointer = json.loadfile(pointerfile)
            assert(not path.is_absolute(pointer.directory) and pointer.directory:find(".tools/runtime/", 1, true) == 1
                and not pointer.directory:find("..", 1, true), "Invalid Windows runtime directory")
            local runtime = path.join(os.projectdir(), pointer.directory)
            assert(hash.sha256(path.join(runtime, "runtime-manifest.json")) == pointer.manifestSha256,
                "Runtime manifest changed; prepare the verified Windows runtime again")
            for _, name in ipairs({"tools", "licenses", "provenance", "THIRD-PARTY.md", "runtime-manifest.json"}) do
                os.cp(path.join(runtime, name), destination)
            end
            local helper = assert(target:dep("astra-process-host"), "Windows resources require astra-process-host")
            assert(os.isfile(helper:targetfile()), "Build astra-process-host before staging resources")
            if path.absolute(path.directory(helper:targetfile())) ~= path.absolute(destination) then
                os.cp(helper:targetfile(), destination)
            end
        elseif target:is_plat("linux", "macosx") then
            local architecture = target:is_arch("x64", "x86_64") and "x64" or "arm64"
            local key = target:plat() .. "-" .. architecture
            local pointerfile = path.join(os.projectdir(), ".tools/runtime/" .. key .. ".json")
            assert(os.isfile(pointerfile), "Run scripts/prepare-native-runtime.py on the native host before building/installing Astra")
            local pointer = json.loadfile(pointerfile)
            assert(not path.is_absolute(pointer.directory) and pointer.directory:find(".tools/runtime/", 1, true) == 1
                and not pointer.directory:find("..", 1, true), "Invalid native runtime directory")
            local runtime = path.join(os.projectdir(), pointer.directory)
            assert(hash.sha256(path.join(runtime, "runtime-manifest.json")) == pointer.manifestSha256,
                "Native runtime manifest changed; prepare it again")
            local manifest = json.loadfile(path.join(runtime, "runtime-manifest.json"))
            assert(manifest.platform == target:plat() and manifest.architecture == architecture,
                "Runtime platform/architecture differs from the native target")
            for _, name in ipairs({"tools", "licenses", "provenance", "THIRD-PARTY.md", "runtime-manifest.json"}) do
                os.cp(path.join(runtime, name), destination)
            end
        end
    end)
rule_end()

rule("astra.clipboard-runtime")
    on_load(function(target)
        if target:is_plat("macosx") then target:add("rpathdirs", "@executable_path")
        elseif target:is_plat("linux") then target:add("rpathdirs", "$ORIGIN") end
    end)
    after_build(function(target)
        local sdl = target:pkg("libsdl3") or target:pkg("astra-sdl3")
        local pattern = target:is_plat("windows") and "bin/SDL3.dll"
            or (target:is_plat("macosx") and "lib/libSDL3*.dylib" or "lib/libSDL3.so*")
        local libraries = os.files(path.join(assert(sdl:installdir()), pattern))
        assert(#libraries > 0, "Clipboard helper requires the SDL shared runtime")
        for _, file in ipairs(libraries) do os.cp(file, target:targetdir()) end
    end)
rule_end()
