find_package(nlohmann_json CONFIG REQUIRED)
find_package(libuv CONFIG REQUIRED)
find_package(re2 CONFIG REQUIRED)
find_package(utf8proc CONFIG REQUIRED)
find_package(unofficial-tree-sitter CONFIG REQUIRED)
find_package(astra-quickjs CONFIG REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
find_package(unofficial-skia CONFIG REQUIRED)
find_package(Dawn CONFIG REQUIRED)
add_library(astra-graphics INTERFACE)
add_library(Astra::Graphics ALIAS astra-graphics)
target_link_libraries(astra-graphics INTERFACE
    unofficial::skia::skia unofficial::skia::modules::skparagraph
    unofficial::skia::modules::skshaper
    unofficial::skia::modules::skunicode_icu dawn::webgpu_dawn)

# DXC is a runtime dependency even when Dawn itself is linked statically.
set(ASTRA_GRAPHICS_RUNTIME_FILES)
if(WIN32)
    foreach(name IN ITEMS dxcompiler.dll dxil.dll)
        find_file(astra_${name} NAMES "${name}"
            PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin" NO_DEFAULT_PATH REQUIRED)
        list(APPEND ASTRA_GRAPHICS_RUNTIME_FILES "${astra_${name}}")
    endforeach()
endif()

# Restored archives can preserve older header timestamps. Fingerprint the
# installed ABI records so dependency changes also invalidate consumer commands.
file(SHA256 "${VCPKG_INSTALLED_DIR}/vcpkg/status" ASTRA_VCPKG_STATE_SHA256)
target_compile_definitions(astra-options INTERFACE
    ASTRA_VCPKG_STATE_SHA256="${ASTRA_VCPKG_STATE_SHA256}")

include(CheckCSourceCompiles)
include(CheckCXXSourceCompiles)
unset(ASTRA_HAS_SHAPING_CACHE CACHE)
set(CMAKE_REQUIRED_LIBRARIES unofficial::skia::modules::skparagraph)
check_cxx_source_compiles("#include <modules/skparagraph/include/FontCollection.h>\n#include <modules/skshaper/include/SkShaper_harfbuzz.h>\nint main() { skia::textlayout::FontCollection fonts; fonts.setShapingCache({}); return fonts.getShapingCache() ? 1 : 0; }" ASTRA_HAS_SHAPING_CACHE)
unset(CMAKE_REQUIRED_LIBRARIES)
if(NOT ASTRA_HAS_SHAPING_CACHE)
    message(FATAL_ERROR "Skia must include the reviewed glyph-cache adapter")
endif()
set(CMAKE_REQUIRED_LIBRARIES Astra::qjs)
unset(ASTRA_HAS_HOST_CONTEXT CACHE)
check_c_source_compiles("#include <quickjs.h>\nint main(void) { JSRuntime* r=JS_NewRuntime(); JS_SetHostAsyncContext(r,1); return JS_GetHostAsyncContext(r)!=1; }" ASTRA_HAS_HOST_CONTEXT)
unset(CMAKE_REQUIRED_LIBRARIES)
if(NOT ASTRA_HAS_HOST_CONTEXT)
    message(FATAL_ERROR "QuickJS must include the pinned host async context extension")
endif()
if(BUILD_TESTING)
    find_package(doctest CONFIG REQUIRED)
endif()
