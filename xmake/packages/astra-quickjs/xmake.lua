package("astra-quickjs")
    set_base("quickjs-ng")
    set_description("Pinned QuickJS-NG with host task context for async continuations")
    add_urls("https://github.com/quickjs-ng/quickjs/archive/refs/tags/$(version).tar.gz")
    add_versions("v0.15.1", "c4e813951b7c46845096a948e978c620b11ab4cf5fd622ca09c727ec31f42623")
    add_configs("context_patch", {
        description = "SHA-256 of Astra's host async context extension",
        default = "97fe8762e74c107e912bc73f7bf023cd6fdb7f68aeeae3be8fae410272ae48e1",
        type = "string"
    })
    add_patches("v0.15.1", path.join(os.scriptdir(), "host-async-context.patch"),
        "97fe8762e74c107e912bc73f7bf023cd6fdb7f68aeeae3be8fae410272ae48e1")
    on_load(function(package)
        assert(package:patches() and #package:patches() == 1,
            "Astra requires its verified QuickJS-NG host context patch")
        local patch = package:patches()[1]
        assert(hash.sha256(patch.url) == patch.sha256 and
            package:config("context_patch") == patch.sha256,
            "QuickJS-NG patch must match its configured cache identity")
    end)
    on_install(function(package)
        if not io.readfile("quickjs.h"):find("JS_GetHostAsyncContext", 1, true) then
            -- xmake's git apply can skip every hunk when the dependency cache
            -- lives inside this checkout. Stop Git discovery at the source's
            -- parent so relative patch paths address this extracted source.
            local patch = package:patches()[1]
            assert(hash.sha256(patch.url) == patch.sha256, "QuickJS-NG patch hash mismatch")
            local git = import("lib.detect.find_tool")("git")
            assert(git, "Git is required to apply the verified QuickJS-NG patch")
            local options = {envs = {GIT_CEILING_DIRECTORIES = path.directory(os.curdir())}}
            os.vrunv(git.program, {"apply", "--check", patch.url}, options)
            os.vrunv(git.program, {"apply", patch.url}, options)
        end
        assert(io.readfile("quickjs.c"):find("rd->host_async_context", 1, true),
            "QuickJS-NG promise reactions must retain their registered host context")
        package:base():script("install")(package)
    end)
    on_test(function(package)
        assert(package:has_cfuncs("JS_GetHostAsyncContext", {includes = "quickjs.h"}))
        assert(package:has_cfuncs("JS_SetHostAsyncContext", {includes = "quickjs.h"}))
    end)
package_end()
