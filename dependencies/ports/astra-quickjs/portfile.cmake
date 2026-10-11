vcpkg_download_distfile(ARCHIVE
    URLS "https://github.com/quickjs-ng/quickjs/archive/refs/tags/v0.15.1.tar.gz"
    FILENAME "astra-astra-quickjs-0.15.1.tar.gz"
    SHA512 c963ebee04247e695601e13abddf552866d2ab54411563d46224c5974b1e84818e14c1e99a7fe776d08210d5acda5c2443f185cc9f0fdfc032820e436b7aa13b
)
file(SHA256 "${ARCHIVE}" source_sha256)
if(NOT source_sha256 STREQUAL "c4e813951b7c46845096a948e978c620b11ab4cf5fd622ca09c727ec31f42623")
    message(FATAL_ERROR "Pinned astra-quickjs archive SHA-256 mismatch")
endif()
vcpkg_extract_source_archive(SOURCE_PATH ARCHIVE "${ARCHIVE}" PATCHES host-async-context.patch)

file(READ "${SOURCE_PATH}/quickjs.h" qjs_header)
if(NOT qjs_header MATCHES "JS_GetHostAsyncContext")
    message(FATAL_ERROR "Required host async context patch is missing")
endif()
file(APPEND "${SOURCE_PATH}/CMakeLists.txt" "
get_property(astra_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(astra_target IN LISTS astra_targets)
    if(NOT astra_target STREQUAL qjs)
        set_property(TARGET \${astra_target} PROPERTY EXCLUDE_FROM_ALL TRUE)
    endif()
endforeach()
if(MSVC)
    # The interpreter serves the same 8 ms workbench budget in both builds.
    # Keep Debug symbols, assertions and /MDd while optimizing its dispatch loop.
    target_compile_options(qjs PRIVATE \$<\$<CONFIG:Debug>:/O2> \$<\$<CONFIG:Debug>:/Ob2>)
endif()
install(TARGETS qjs EXPORT astra-quickjs-config ARCHIVE DESTINATION lib)
install(FILES quickjs.h DESTINATION include)
install(EXPORT astra-quickjs-config NAMESPACE Astra:: DESTINATION share/astra-quickjs)
")
set(astra_debug_options)
if(VCPKG_TARGET_IS_WINDOWS)
    # /RTC is incompatible with an optimized interpreter. Astra's own Debug
    # targets retain their normal runtime checks and STL iterator diagnostics.
    list(APPEND astra_debug_options "-DCMAKE_C_FLAGS_DEBUG=/MDd /Z7")
endif()
vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}" OPTIONS
    -DQJS_BUILD_LIBC=OFF -DQJS_ENABLE_INSTALL=OFF -DQJS_BUILD_EXAMPLES=OFF
    -DBUILD_SHARED_LIBS=OFF OPTIONS_DEBUG ${astra_debug_options})
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME astra-quickjs CONFIG_PATH share/astra-quickjs)
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
