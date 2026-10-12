# Production target graph; explicit source membership.
function(astra_library name)
    add_library(${name} STATIC ${ARGN})
    target_include_directories(${name} PUBLIC "${PROJECT_SOURCE_DIR}/include")
    target_link_libraries(${name} PRIVATE astra-options)
endfunction()
astra_library(astra-core
    src/core/content_hash.cpp
    src/core/document.cpp
    src/core/rope.cpp
)
target_link_libraries(astra-core PUBLIC astra-options )

astra_library(astra-protocol
    src/protocol/framing.cpp
    src/protocol/rpc.cpp
)
target_link_libraries(astra-protocol PUBLIC astra-options nlohmann_json::nlohmann_json)

astra_library(astra-io
    src/io/file_store.cpp
)
target_link_libraries(astra-io PUBLIC astra-options astra-core)

astra_library(astra-service
    src/service/metadata_clock.cpp
    src/service/application_document_search.cpp
    src/service/application_git.cpp
    src/service/application_language.cpp
    src/service/application_recovery.cpp
    src/service/application_search.cpp
    src/service/application_service.cpp
    src/service/application_syntax.cpp
    src/service/application_watch.cpp
    src/service/editor_service.cpp
    src/service/service_host.cpp
)
target_link_libraries(astra-service PUBLIC astra-options astra-core astra-protocol astra-editing astra-workspace astra-script astra-transport astra-syntax astra-git astra-lsp astra-recovery astra-watch nlohmann_json::nlohmann_json utf8proc::utf8proc)

astra_library(astra-config
    src/config/editor_config.cpp
)
target_link_libraries(astra-config PUBLIC astra-options astra-core)

astra_library(astra-script
    src/script/runtime.cpp
)
target_link_libraries(astra-script PUBLIC astra-options astra-protocol Astra::qjs nlohmann_json::nlohmann_json)

astra_library(astra-transport
    src/transport/tcp_server.cpp
    src/transport/tcp_client.cpp
    src/transport/editor_channel.cpp
)
target_link_libraries(astra-transport PUBLIC astra-options astra-protocol libuv::uv_a)

astra_library(astra-workspace
    src/workspace/search.cpp
    src/workspace/workspace.cpp
)
target_link_libraries(astra-workspace PUBLIC astra-options astra-core astra-io astra-config utf8proc::utf8proc re2::re2)

astra_library(astra-editing
    src/editing/commands.cpp
)
target_link_libraries(astra-editing PUBLIC astra-options astra-core utf8proc::utf8proc)

astra_library(astra-text-source
    src/ui/text_source.cpp
)
target_link_libraries(astra-text-source PUBLIC astra-options astra-core)

astra_library(astra-renderer
    src/ui/renderer.cpp
    src/ui/text_layout.cpp
    src/ui/glyph_cache.cpp
    src/ui/glyph_sequence.cpp
    src/ui/font_signatures.cpp
)
target_link_libraries(astra-renderer PUBLIC astra-options astra-text-source Astra::Graphics SDL3::SDL3 utf8proc::utf8proc)
if(WIN32)
    target_link_libraries(astra-renderer PUBLIC dwmapi)
endif()


add_executable(astra-server
    apps/astra/main.cpp
)
target_link_libraries(astra-server PUBLIC astra-options astra-service astra-transport libuv::uv_a)

astra_library(astra-syntax
    src/language/syntax.cpp
)
target_link_libraries(astra-syntax PUBLIC astra-options astra-core astra-grammar-c astra-grammar-cpp astra-grammar-javascript astra-grammar-typescript astra-grammar-tsx astra-grammar-json astra-grammar-python astra-grammar-markdown astra-grammar-markdown-inline unofficial::tree-sitter::tree-sitter)

astra_library(astra-process
    src/process/process.cpp
)
target_link_libraries(astra-process PUBLIC astra-options astra-core libuv::uv_a)
if(WIN32)
    add_dependencies(astra-process astra-process-host)
endif()

astra_library(astra-lsp
    src/language/lsp.cpp
)
target_link_libraries(astra-lsp PUBLIC astra-options astra-core astra-protocol astra-process)

astra_library(astra-ui
    src/ui/editor_window.cpp
    src/ui/icons.cpp
    src/ui/document_viewport.cpp
    src/ui/layout_jobs.cpp
    src/ui/text_surface.cpp
    src/ui/text_input_layout.cpp
    src/ui/text_labels.cpp
    src/ui/text_field.cpp
    src/ui/tab_bar.cpp
    src/ui/view_rows.cpp
    src/ui/workbench_chrome.cpp
    src/ui/dialog_metrics.cpp
    src/ui/input_events.cpp
)
target_link_libraries(astra-ui PUBLIC astra-options astra-renderer astra-workbench astra-io astra-clipboard nlohmann_json::nlohmann_json libuv::uv_a unofficial::skia::modules::svg)
if(WIN32)
    target_link_libraries(astra-ui PUBLIC user32)
endif()

astra_library(astra-icon-assets
    src/ui/icon_resources.cpp
    "${ASTRA_ICON_OUTPUT}/icon_data.cpp"
)
target_link_libraries(astra-icon-assets PUBLIC astra-options)
astra_library(astra-workbench
    src/ui/editor_tooltip.cpp
    src/ui/workbench_style.cpp
    src/ui/tab_index.cpp
    src/ui/view_rows_layout.cpp
    src/ui/document_metadata.cpp
    src/ui/chrome_state.cpp
    src/ui/file_icons.cpp
    src/ui/workbench.cpp
    src/ui/input_queue.cpp
    src/ui/file_tree_index.cpp
    src/ui/workbench_layout.cpp
    src/ui/text_line_index.cpp
    src/ui/text_panel.cpp
    src/ui/picker_index.cpp
    src/ui/decoration_indexes.cpp
    src/ui/inline_form.cpp
    src/ui/form_registry.cpp
    src/ui/view_index.cpp
    src/ui/view_page.cpp
    src/ui/view_store.cpp
    src/ui/view_retirement.cpp
    src/ui/viewport_index.cpp
)
target_link_libraries(astra-workbench PUBLIC astra-options astra-script astra-core astra-editing astra-icon-assets)

add_executable(astra
    apps/astra/main.cpp
)
target_link_libraries(astra PUBLIC astra-options astra-service astra-transport astra-ui libuv::uv_a Astra::Graphics SDL3::SDL3)
target_compile_definitions(astra PRIVATE ASTRA_WITH_GUI)

astra_library(astra-git
    src/git/repository.cpp
)
target_link_libraries(astra-git PUBLIC astra-options astra-core astra-io astra-process)

if(WIN32)
add_executable(astra-process-host
    apps/process/host.cpp
)
target_link_libraries(astra-process-host PUBLIC astra-options )
endif()

astra_library(astra-clipboard
    src/ui/clipboard_client.cpp
)
target_link_libraries(astra-clipboard PUBLIC astra-options astra-process astra-protocol astra-core)

add_executable(astra-clipboard-host
    apps/clipboard/host.cpp
    src/ui/clipboard_text.cpp
)
target_link_libraries(astra-clipboard-host PUBLIC astra-options astra-protocol astra-core SDL3::SDL3 libuv::uv_a)
if(WIN32)
    target_link_libraries(astra-clipboard-host PUBLIC user32)
endif()

astra_library(astra-recovery
    src/recovery/store.cpp
)
target_link_libraries(astra-recovery PUBLIC astra-options astra-core astra-io nlohmann_json::nlohmann_json)

astra_library(astra-watch
    src/watch/file_monitor.cpp
)
target_link_libraries(astra-watch PUBLIC astra-options libuv::uv_a)

if(WIN32)
    target_link_libraries(astra-core PUBLIC bcrypt)
endif()

if(ASTRA_BUILD_BENCHMARKS)
    add_executable(astra-core-benchmark benchmarks/core.cpp)
    target_link_libraries(astra-core-benchmark PRIVATE astra-core astra-options)
    add_executable(astra-view-benchmark benchmarks/view.cpp)
    target_link_libraries(astra-view-benchmark PRIVATE astra-workbench astra-text-source astra-options)
endif()

# Individually built consumers also need their production sibling hosts.
add_dependencies(astra-clipboard astra-clipboard-host)
