# Each behavior source belongs to exactly one executable.
get_property(astra_existing_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
if(BUILD_TESTING)
    add_library(astra-test-main OBJECT tests/support/test_main.cpp)
    target_link_libraries(astra-test-main PRIVATE doctest::doctest astra-options)
    add_executable(astra-unit-tests
        tests/unit/content_hash_tests.cpp
        tests/unit/text_source_tests.cpp
        tests/unit/tab_index_tests.cpp
        tests/unit/file_icons_tests.cpp
        tests/unit/sum_tree_tests.cpp
        tests/unit/rope_tests.cpp
        tests/unit/document_tests.cpp
        tests/unit/framing_tests.cpp
        tests/unit/rpc_tests.cpp
        tests/unit/editor_config_tests.cpp
        tests/unit/editing_tests.cpp
        tests/unit/workbench_tests.cpp
        tests/unit/view_page_tests.cpp
        tests/unit/presentation_tests.cpp
        tests/unit/viewport_index_tests.cpp
    )
    target_link_libraries(astra-unit-tests PRIVATE astra-test-main astra-service astra-workbench astra-text-source doctest::doctest astra-options)
    add_test(NAME unit COMMAND astra-unit-tests --no-version)
    set_tests_properties(unit PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS unit TIMEOUT 600)
    add_executable(astra-integration-tests
        tests/integration/file_store_tests.cpp
        tests/integration/service_tests.cpp
        tests/integration/script_tests.cpp
        tests/integration/tcp_tests.cpp
        tests/integration/workspace_tests.cpp
        tests/integration/application_tests.cpp
        tests/integration/application_language_tests.cpp
        tests/integration/application_recovery_tests.cpp
        tests/integration/tcp_client_tests.cpp
        tests/integration/syntax_tests.cpp
        tests/integration/process_tests.cpp
        tests/integration/lsp_tests.cpp
        tests/integration/git_tests.cpp
        tests/integration/recovery_tests.cpp
        tests/integration/file_monitor_tests.cpp
        tests/integration/application_watch_tests.cpp
        tests/integration/application_search_tests.cpp
    )
    target_link_libraries(astra-integration-tests PRIVATE astra-test-main astra-service astra-workbench astra-text-source doctest::doctest astra-options)
    add_test(NAME integration COMMAND astra-integration-tests --no-version)
    set_tests_properties(integration PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS integration TIMEOUT 600)
    add_dependencies(astra-integration-tests astra-runtime)
    add_custom_target(astra-tests DEPENDS astra-unit-tests astra-integration-tests)
    foreach(fixture IN ITEMS process recovery)
        add_executable(astra-${fixture}-fixture tests/fixtures/${fixture}_fixture.cpp)
        target_link_libraries(astra-${fixture}-fixture PRIVATE astra-service astra-options)
        add_dependencies(astra-integration-tests astra-${fixture}-fixture)
    endforeach()
    add_executable(lsp-fixture tests/fixtures/lsp_fixture.cpp)
    target_link_libraries(lsp-fixture PRIVATE astra-service astra-options)
    add_dependencies(astra-integration-tests lsp-fixture)
    if(WIN32)
        add_test(NAME cli COMMAND pwsh -NoProfile -File "${PROJECT_SOURCE_DIR}/tests/integration/cli_smoke.ps1" -ServerExecutable "$<TARGET_FILE:astra-server>")
        set_tests_properties(cli PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS integration TIMEOUT 180)
        add_test(NAME package-helpers COMMAND pwsh -NoProfile -File "${PROJECT_SOURCE_DIR}/tests/package/package_scripts_tests.ps1")
        set_tests_properties(package-helpers PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS package TIMEOUT 180)
    endif()
endif()
if(ASTRA_BUILD_GUI_TESTS)
add_executable(graphics-smoke
    tests/gui/graphics_smoke.cpp
)
target_link_libraries(graphics-smoke PRIVATE astra-ui astra-service astra-options)
add_test(NAME graphics-smoke COMMAND graphics-smoke)
set_tests_properties(graphics-smoke PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS "gui" RUN_SERIAL TRUE TIMEOUT 180)
endif()
if(ASTRA_BUILD_GUI_TESTS)
add_executable(renderer-smoke
    tests/gui/renderer_smoke.cpp
)
target_link_libraries(renderer-smoke PRIVATE astra-ui astra-service astra-options)
if(WIN32)
    target_sources(renderer-smoke PRIVATE tests/support/window_frame.cpp)
    target_link_libraries(renderer-smoke PRIVATE d3d11 runtimeobject)
endif()
add_test(NAME renderer-smoke COMMAND renderer-smoke)
set_tests_properties(renderer-smoke PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS "gui" RUN_SERIAL TRUE TIMEOUT 180)
endif()
if(ASTRA_BUILD_GUI_TESTS)
add_executable(text-layout-tests
    tests/gui/text_layout_tests.cpp
)
target_link_libraries(text-layout-tests PRIVATE astra-ui astra-service astra-options)
add_test(NAME text-layout-tests COMMAND text-layout-tests)
set_tests_properties(text-layout-tests PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS "gui" RUN_SERIAL TRUE TIMEOUT 600)
endif()
if(ASTRA_BUILD_GUI_TESTS)
add_executable(viewport-layout-tests
    tests/gui/viewport_layout_tests.cpp
)
target_link_libraries(viewport-layout-tests PRIVATE astra-ui astra-service astra-options)
add_test(NAME viewport-layout-tests COMMAND viewport-layout-tests)
set_tests_properties(viewport-layout-tests PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS "gui" RUN_SERIAL TRUE TIMEOUT 600)
endif()
if(ASTRA_BUILD_GUI_TESTS)
add_executable(gui-smoke
    tests/gui/gui_smoke.cpp
)
target_link_libraries(gui-smoke PRIVATE astra-ui astra-service astra-options)
add_test(NAME gui-smoke COMMAND gui-smoke)
set_tests_properties(gui-smoke PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS "gui" RUN_SERIAL TRUE TIMEOUT 180)
endif()
if(ASTRA_BUILD_GUI_TESTS)
add_executable(gui-services-smoke
    tests/gui/gui_services_smoke.cpp
)
target_link_libraries(gui-services-smoke PRIVATE astra-ui astra-service astra-options)
add_test(NAME gui-services-smoke COMMAND gui-services-smoke)
set_tests_properties(gui-services-smoke PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS "gui" RUN_SERIAL TRUE TIMEOUT 180)
endif()
if(ASTRA_BUILD_GUI_TESTS)
add_executable(gui-recovery-smoke
    tests/gui/gui_recovery_smoke.cpp
)
target_link_libraries(gui-recovery-smoke PRIVATE astra-ui astra-service astra-options)
add_test(NAME gui-recovery-smoke COMMAND gui-recovery-smoke)
set_tests_properties(gui-recovery-smoke PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS "gui" RUN_SERIAL TRUE TIMEOUT 180)
endif()
if(ASTRA_BUILD_GUI_TESTS)
add_executable(gui-workspace-smoke
    tests/gui/gui_workspace_smoke.cpp
)
target_link_libraries(gui-workspace-smoke PRIVATE astra-ui astra-service astra-options)
add_test(NAME gui-workspace-smoke COMMAND gui-workspace-smoke)
set_tests_properties(gui-workspace-smoke PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS "gui" RUN_SERIAL TRUE TIMEOUT 180)
endif()
if(ASTRA_BUILD_GUI_TESTS)
add_executable(gui-presentation-smoke
    tests/gui/gui_presentation_smoke.cpp
)
target_link_libraries(gui-presentation-smoke PRIVATE astra-ui astra-service astra-options)
if(WIN32)
    target_sources(gui-presentation-smoke PRIVATE tests/support/window_frame.cpp)
    target_link_libraries(gui-presentation-smoke PRIVATE d3d11 runtimeobject)
endif()
add_test(NAME gui-presentation-smoke COMMAND gui-presentation-smoke)
set_tests_properties(gui-presentation-smoke PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS "gui" RUN_SERIAL TRUE TIMEOUT 600)
endif()
if(ASTRA_BUILD_GUI_TESTS)
add_executable(gui-visual-smoke
    tests/gui/gui_visual_smoke.cpp
)
target_link_libraries(gui-visual-smoke PRIVATE astra-ui astra-service astra-options)
add_test(NAME gui-visual-smoke COMMAND gui-visual-smoke)
set_tests_properties(gui-visual-smoke PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS "gui" RUN_SERIAL TRUE TIMEOUT 180)
endif()
if(ASTRA_BUILD_ENVIRONMENT_TESTS AND WIN32)
add_executable(gui-clipboard-async-smoke
    tests/environment/gui_clipboard_async_smoke.cpp
)
target_link_libraries(gui-clipboard-async-smoke PRIVATE astra-ui astra-service astra-options)
add_test(NAME gui-clipboard-async-smoke COMMAND gui-clipboard-async-smoke)
set_tests_properties(gui-clipboard-async-smoke PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS "environment" RUN_SERIAL TRUE TIMEOUT 180)
endif()
if(ASTRA_BUILD_GUI_TESTS)
add_executable(gui-clipboard-smoke
    tests/gui/gui_clipboard_smoke.cpp
)
target_link_libraries(gui-clipboard-smoke PRIVATE astra-ui astra-service astra-options)
add_test(NAME gui-clipboard-smoke COMMAND gui-clipboard-smoke)
set_tests_properties(gui-clipboard-smoke PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS "gui" RUN_SERIAL TRUE TIMEOUT 180)
endif()
if(ASTRA_BUILD_ENVIRONMENT_TESTS AND WIN32)
add_executable(gui-ime-smoke
    tests/environment/gui_ime_smoke.cpp
)
target_link_libraries(gui-ime-smoke PRIVATE astra-ui astra-service astra-options)
target_link_libraries(gui-ime-smoke PRIVATE imm32 ole32 oleaut32 uuid comctl32 user32 gdi32)
add_test(NAME gui-ime-smoke COMMAND gui-ime-smoke)
set_tests_properties(gui-ime-smoke PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS "environment" RUN_SERIAL TRUE TIMEOUT 180)
endif()
if(ASTRA_BUILD_GUI_TESTS)
add_executable(gui-body-smoke
    tests/gui/gui_body_smoke.cpp
)
target_link_libraries(gui-body-smoke PRIVATE astra-ui astra-service astra-options)
add_test(NAME gui-body-smoke COMMAND gui-body-smoke)
set_tests_properties(gui-body-smoke PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS "gui" RUN_SERIAL TRUE TIMEOUT 180)
endif()
if(ASTRA_BUILD_GUI_TESTS)
    add_test(NAME gui-tcp COMMAND gui-smoke --tcp)
    set_tests_properties(gui-tcp PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS gui RUN_SERIAL TRUE TIMEOUT 180)
endif()

get_property(astra_current_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
list(REMOVE_ITEM astra_current_targets ${astra_existing_targets})
foreach(target IN LISTS astra_current_targets)
    get_target_property(kind ${target} TYPE)
    if(NOT kind STREQUAL "UTILITY")
        target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/tests")
        target_compile_definitions(${target} PRIVATE ASTRA_SOURCE_ROOT="${PROJECT_SOURCE_DIR}")
    endif()
endforeach()
