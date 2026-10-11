# Initial parity graph; test source partition follows in the next increment.
if(BUILD_TESTING)
add_executable(astra-tests
    tests/content_hash_tests.cpp
    tests/text_source_tests.cpp
    tests/tab_index_tests.cpp
    tests/file_icons_tests.cpp
    tests/main.cpp
    tests/sum_tree_tests.cpp
    tests/rope_tests.cpp
    tests/document_tests.cpp
    tests/framing_tests.cpp
    tests/file_store_tests.cpp
    tests/rpc_tests.cpp
    tests/service_tests.cpp
    tests/editor_config_tests.cpp
    tests/script_tests.cpp
    tests/tcp_tests.cpp
    tests/workspace_tests.cpp
    tests/editing_tests.cpp
    tests/application_tests.cpp
    tests/application_language_tests.cpp
    tests/application_recovery_tests.cpp
    tests/tcp_client_tests.cpp
    tests/syntax_tests.cpp
    tests/process_tests.cpp
    tests/lsp_tests.cpp
    tests/git_tests.cpp
    tests/recovery_tests.cpp
    tests/file_monitor_tests.cpp
    tests/application_watch_tests.cpp
    tests/application_search_tests.cpp
    tests/workbench_tests.cpp
    tests/view_page_tests.cpp
    tests/presentation_tests.cpp
    tests/viewport_index_tests.cpp
)
target_link_libraries(astra-tests PRIVATE astra-service astra-workbench astra-text-source doctest::doctest astra-options)
add_test(NAME behavior COMMAND astra-tests --no-version)
set_tests_properties(behavior PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS "unit;integration" TIMEOUT 600)
if(WIN32)
    add_test(NAME cli COMMAND pwsh -NoProfile -File "${PROJECT_SOURCE_DIR}/tests/cli_smoke.ps1"
        -ServerExecutable "$<TARGET_FILE:astra-server>")
    set_tests_properties(cli PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS integration TIMEOUT 180)
endif()
add_executable(astra-process-fixture tests/process_fixture.cpp)
target_link_libraries(astra-process-fixture PRIVATE astra-service astra-options)
add_dependencies(astra-tests astra-process-fixture)
add_executable(lsp-fixture tests/fixtures/lsp_fixture.cpp)
target_link_libraries(lsp-fixture PRIVATE astra-service astra-options)
add_dependencies(astra-tests lsp-fixture)
add_executable(astra-recovery-fixture tests/recovery_fixture.cpp)
target_link_libraries(astra-recovery-fixture PRIVATE astra-service astra-options)
add_dependencies(astra-tests astra-recovery-fixture)
endif()
if(ASTRA_BUILD_GUI_TESTS)
add_executable(graphics-smoke
    tests/graphics_smoke.cpp
)
target_link_libraries(graphics-smoke PRIVATE astra-ui astra-service astra-options)
add_test(NAME graphics-smoke COMMAND graphics-smoke)
set_tests_properties(graphics-smoke PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS "gui" RUN_SERIAL TRUE TIMEOUT 180)
endif()
if(ASTRA_BUILD_GUI_TESTS)
add_executable(renderer-smoke
    tests/renderer_smoke.cpp
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
    tests/text_layout_tests.cpp
)
target_link_libraries(text-layout-tests PRIVATE astra-ui astra-service astra-options)
add_test(NAME text-layout-tests COMMAND text-layout-tests)
set_tests_properties(text-layout-tests PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS "gui" RUN_SERIAL TRUE TIMEOUT 180)
endif()
if(ASTRA_BUILD_GUI_TESTS)
add_executable(viewport-layout-tests
    tests/viewport_layout_tests.cpp
)
target_link_libraries(viewport-layout-tests PRIVATE astra-ui astra-service astra-options)
add_test(NAME viewport-layout-tests COMMAND viewport-layout-tests)
set_tests_properties(viewport-layout-tests PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS "gui" RUN_SERIAL TRUE TIMEOUT 180)
set_tests_properties(text-layout-tests viewport-layout-tests PROPERTIES TIMEOUT 600)
endif()
if(ASTRA_BUILD_GUI_TESTS)
add_executable(gui-smoke
    tests/gui_smoke.cpp
)
target_link_libraries(gui-smoke PRIVATE astra-ui astra-service astra-options)
add_test(NAME gui-smoke COMMAND gui-smoke)
set_tests_properties(gui-smoke PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS "gui" RUN_SERIAL TRUE TIMEOUT 180)
endif()
if(ASTRA_BUILD_GUI_TESTS)
add_executable(gui-services-smoke
    tests/gui_services_smoke.cpp
)
target_link_libraries(gui-services-smoke PRIVATE astra-ui astra-service astra-options)
add_test(NAME gui-services-smoke COMMAND gui-services-smoke)
set_tests_properties(gui-services-smoke PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS "gui" RUN_SERIAL TRUE TIMEOUT 180)
endif()
if(ASTRA_BUILD_GUI_TESTS)
add_executable(gui-recovery-smoke
    tests/gui_recovery_smoke.cpp
)
target_link_libraries(gui-recovery-smoke PRIVATE astra-ui astra-service astra-options)
add_test(NAME gui-recovery-smoke COMMAND gui-recovery-smoke)
set_tests_properties(gui-recovery-smoke PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS "gui" RUN_SERIAL TRUE TIMEOUT 180)
endif()
if(ASTRA_BUILD_GUI_TESTS)
add_executable(gui-workspace-smoke
    tests/gui_workspace_smoke.cpp
)
target_link_libraries(gui-workspace-smoke PRIVATE astra-ui astra-service astra-options)
add_test(NAME gui-workspace-smoke COMMAND gui-workspace-smoke)
set_tests_properties(gui-workspace-smoke PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS "gui" RUN_SERIAL TRUE TIMEOUT 180)
endif()
if(ASTRA_BUILD_GUI_TESTS)
add_executable(gui-presentation-smoke
    tests/gui_presentation_smoke.cpp
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
    tests/gui_visual_smoke.cpp
)
target_link_libraries(gui-visual-smoke PRIVATE astra-ui astra-service astra-options)
add_test(NAME gui-visual-smoke COMMAND gui-visual-smoke)
set_tests_properties(gui-visual-smoke PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS "gui" RUN_SERIAL TRUE TIMEOUT 180)
endif()
if(ASTRA_BUILD_ENVIRONMENT_TESTS AND WIN32)
add_executable(gui-clipboard-async-smoke
    tests/gui_clipboard_async_smoke.cpp
)
target_link_libraries(gui-clipboard-async-smoke PRIVATE astra-ui astra-service astra-options)
add_test(NAME gui-clipboard-async-smoke COMMAND gui-clipboard-async-smoke)
set_tests_properties(gui-clipboard-async-smoke PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS "environment" RUN_SERIAL TRUE TIMEOUT 180)
endif()
if(ASTRA_BUILD_GUI_TESTS)
add_executable(gui-clipboard-smoke
    tests/gui_clipboard_smoke.cpp
)
target_link_libraries(gui-clipboard-smoke PRIVATE astra-ui astra-service astra-options)
add_test(NAME gui-clipboard-smoke COMMAND gui-clipboard-smoke)
set_tests_properties(gui-clipboard-smoke PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS "gui" RUN_SERIAL TRUE TIMEOUT 180)
endif()
if(ASTRA_BUILD_ENVIRONMENT_TESTS AND WIN32)
add_executable(gui-ime-smoke
    tests/gui_ime_smoke.cpp
)
target_link_libraries(gui-ime-smoke PRIVATE astra-ui astra-service astra-options)
target_link_libraries(gui-ime-smoke PRIVATE imm32 ole32 oleaut32 uuid comctl32 user32 gdi32)
add_test(NAME gui-ime-smoke COMMAND gui-ime-smoke)
set_tests_properties(gui-ime-smoke PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS "environment" RUN_SERIAL TRUE TIMEOUT 180)
endif()
if(ASTRA_BUILD_GUI_TESTS)
add_executable(gui-body-smoke
    tests/gui_body_smoke.cpp
)
target_link_libraries(gui-body-smoke PRIVATE astra-ui astra-service astra-options)
add_test(NAME gui-body-smoke COMMAND gui-body-smoke)
set_tests_properties(gui-body-smoke PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS "gui" RUN_SERIAL TRUE TIMEOUT 180)
endif()
if(ASTRA_BUILD_GUI_TESTS)
    add_test(NAME gui-tcp COMMAND gui-smoke --tcp)
    set_tests_properties(gui-tcp PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS gui RUN_SERIAL TRUE TIMEOUT 180)
endif()
