add_subdirectory(lib)

set(CLI_SOURCES
    cli_handler.c
    cli_tree.c
    cli_view.c
    cli_element.c
    cli_param_type.c
    cli_xml_parser.c
    cli_dispatch.c
    cli_history.c
    cli_main.c
    cli_cfg.c
)

add_executable(netnexus-cli cli_proc.c ${CLI_SOURCES})

target_include_directories(netnexus-cli PUBLIC
    .
    ${LIBXML2_INCLUDE_DIR}
)

target_link_libraries(netnexus-cli PRIVATE
    dev_api
    cli_api
    utils
    ${LIBXML2_LIBRARIES}
    ${GLIB_LIBRARIES}
    Threads::Threads
    util
)

set_target_properties(netnexus-cli PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
)

install(TARGETS netnexus-cli RUNTIME DESTINATION bin)
