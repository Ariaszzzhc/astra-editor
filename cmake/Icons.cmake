# Assets are checked in. Configure verifies hashes without a network fetch.
file(GLOB_RECURSE astra_icon_inputs CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/resources/icons/*.svg"
    "${PROJECT_SOURCE_DIR}/dependencies/icons/licenses/*")
list(APPEND astra_icon_inputs
    "${PROJECT_SOURCE_DIR}/dependencies/icons/manifest.json"
    "${PROJECT_SOURCE_DIR}/resources/icons/associations.json"
    "${PROJECT_SOURCE_DIR}/scripts/prepare-icons.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${astra_icon_inputs})
set(ASTRA_ICON_OUTPUT "${CMAKE_BINARY_DIR}/generated/icons")
if(WIN32)
    execute_process(COMMAND "${PROJECT_SOURCE_DIR}/third_party/vcpkg/vcpkg.exe" fetch python3
        OUTPUT_VARIABLE ASTRA_ICON_PYTHON OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
    # First acquisition also reports progress on stdout; the final line is
    # the executable. Subsequent cached invocations print only that path.
    string(REPLACE "\n" ";" astra_python_output "${ASTRA_ICON_PYTHON}")
    list(GET astra_python_output -1 ASTRA_ICON_PYTHON)
    string(STRIP "${ASTRA_ICON_PYTHON}" ASTRA_ICON_PYTHON)
else()
    set(ASTRA_ICON_PYTHON "${Python3_EXECUTABLE}")
endif()
execute_process(COMMAND "${ASTRA_ICON_PYTHON}" "${PROJECT_SOURCE_DIR}/scripts/prepare-icons.py"
    --root "${PROJECT_SOURCE_DIR}" --output "${ASTRA_ICON_OUTPUT}"
    COMMAND_ERROR_IS_FATAL ANY)
