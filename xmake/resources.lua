-- All operations live inside xmake sandbox callbacks; project-scope functions
-- do not inherit the callback environment.
rule("astra.resources")
    on_load(function(target)
        if target:is_plat("windows") then target:add("deps", "astra-process-host", {inherit = false}) end
    end)
    after_build(function(target)
        local json = import("core.base.json")
        local destination = target:targetdir()
        os.mkdir(destination)
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