add_subdirectory(lib)

# CLI library sources
set(CFG_SOURCES
    cli_handler.c
    cli_tree.c
    cli_view.c
    cli_element.c
    cli_param_type.c
    cli_xml_parser.c
    cli_dispatch.c
    cli_history.c
    cfg_main.c
    cfg_cli.c
)

# Create shared library for CLI
add_library(cfg SHARED ${CFG_SOURCES})

# Include directories for CLI library
target_include_directories(cfg PUBLIC
    .
    ${LIBXML2_INCLUDE_DIR}
)

# Link dependencies
target_link_libraries(cfg PUBLIC
    dev
    dev_api
    cli_api
    utils
    ${LIBXML2_LIBRARIES}
    ${GLIB_LIBRARIES}
    Threads::Threads
    util
)

# Set library version
set_target_properties(cfg PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
)
