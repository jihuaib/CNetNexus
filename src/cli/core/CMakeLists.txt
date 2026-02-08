# CLI 库(合并 core + parser + engine 到单个 .so)
set(CLI_SOURCES
    # Core files
    cli_tree.c
    cli_element.c
    cli_view.c
    cli_history.c
    cfg_template_renderer.c
    cfg_cli.c
    cfg_show_config.c
    cfg_api.c
    cfg_registry.c
    # Parser files
    cli_xml_parser.c
    config_template.c
    # Engine files
    cli_engine.c
    cli_handler.c
    cli_dispatch.c
)

add_library(cli SHARED ${CLI_SOURCES})

target_include_directories(cli PUBLIC
    .
    ${CMAKE_SOURCE_DIR}/src/cli/param
    ${CMAKE_SOURCE_DIR}/src/db/lib
    ${CMAKE_SOURCE_DIR}/include
    ${LIBXML2_INCLUDE_DIR}
)

target_link_libraries(cli PUBLIC
    cli_param          # 参数解析库
    db_client          # DB 客户端库
    ipc                # IPC 库
    utils              # 工具库
    ${LIBXML2_LIBRARIES}
    ${GLIB_LIBRARIES}
    ${SQLite3_LIBRARIES}
    Threads::Threads
)

set_target_properties(cli PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)
