package("astra-sdl3")
    set_homepage("https://libsdl.org")
    set_description("SDL3 windows and input for Astra's Graphite renderer")
    set_license("zlib")
    add_urls("https://github.com/libsdl-org/SDL/releases/download/release-$(version)/SDL3-$(version).zip")
    add_versions("3.4.12", "3d4de8967a49c0451e775a0c1e9022092c19fdef41ba38a83fcf031c5a6496e2")
    add_deps("cmake")
    add_links("SDL3")
    on_install(function(package)
        -- Astra uses SDL for native windows and input. Skia/Dawn supplies rendering;
        -- SDL's separate GL/Vulkan/GPU renderers and their header packages are unused.
        local configs = {
            "-DSDL_SHARED=ON", "-DSDL_STATIC=OFF", "-DSDL_TESTS=OFF",
            "-DSDL_TEST_LIBRARY=OFF", "-DSDL_EXAMPLES=OFF",
            "-DSDL_RENDER=OFF", "-DSDL_GPU=OFF", "-DSDL_OPENGL=OFF",
            "-DSDL_OPENGLES=OFF", "-DSDL_VULKAN=OFF"
        }
        if package:is_plat("macosx") then
            table.insert(configs, "-DSDL_METAL=ON")
            table.insert(configs, "-DCMAKE_MACOSX_RPATH=ON")
            table.insert(configs, "-DCMAKE_INSTALL_NAME_DIR=@rpath")
        elseif package:is_plat("linux") then
            table.insert(configs, "-DSDL_X11=ON")
            table.insert(configs, "-DSDL_WAYLAND=ON")
        end
        local cmake = import("package.tools.cmake")
        if package:is_plat("linux", "macosx") then
            cmake.configure(package, configs)
            local configuration = io.readfile(path.join(package:builddir(), "CMakeFiles", "SDL_build_config.h.intermediate"))
            local capabilities = package:is_plat("macosx") and {"SDL_VIDEO_DRIVER_COCOA", "SDL_VIDEO_METAL"}
                or {"SDL_VIDEO_DRIVER_X11", "SDL_VIDEO_DRIVER_WAYLAND"}
            for _, capability in ipairs(capabilities) do
                assert(configuration and configuration:find("#define " .. capability .. " 1", 1, true),
                    "SDL3 is missing required native capability " .. capability .. "; install its development dependencies")
            end
        end
        cmake.install(package, configs)
    end)
    on_test(function(package)
        assert(package:check_cxxsnippets({test = [[
            void test() { SDL_GetVersion(); }
        ]]}, {includes = "SDL3/SDL.h"}))
    end)
