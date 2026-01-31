# CLI library sources
set(NN_CFG_SOURCES
    nn_cli_handler.c
    nn_cli_tree.c
    nn_cli_view.c
    nn_cli_element.c
    nn_cli_param_type.c
    nn_cli_xml_parser.c
    nn_cli_dispatch.c
    nn_cli_history.c
    nn_cfg_registry.c
    nn_cfg_main.c
    nn_cfg_cli.c
    nn_cfg_api.c
    nn_config_template.c
    nn_cfg_template_renderer.c
    nn_cfg_show_config.c
)

# Create shared library for CLI
add_library(nn_cfg SHARED ${NN_CFG_SOURCES})

# Include directories for CLI library
target_include_directories(nn_cfg PUBLIC
    .
    ${LIBXML2_INCLUDE_DIR}
)

# Link dependencies
target_link_libraries(nn_cfg PUBLIC
    nn_dev
    ${LIBXML2_LIBRARIES}
    ${GLIB_LIBRARIES}
    Threads::Threads
)

# Set library version
set_target_properties(nn_cfg PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
)
