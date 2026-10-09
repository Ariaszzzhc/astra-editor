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
        import("package.tools.cmake").install(package, configs)
    end)
    on_test(function(package)
        assert(package:check_cxxsnippets({test = [[
            void test() { SDL_GetVersion(); }
        ]]}, {includes = "SDL3/SDL.h"}))
    end)
