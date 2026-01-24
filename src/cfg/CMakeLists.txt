# CLI library sources
set(NN_CFG_SOURCES
    nn_cli_handler.c
    nn_cli_tree.c
    nn_cli_view.c
    nn_cli_element.c
    nn_cli_xml_parser.c
    nn_cli_dispatch.c
    nn_cfg_main.c
)

# Create shared library for CLI
add_library(nn_cfg SHARED ${NN_CFG_SOURCES})

# Include directories for CLI library
target_include_directories(nn_cfg PUBLIC
    .
    ../../../include
    ${LIBXML2_INCLUDE_DIR}
)

# Link dependencies
target_link_libraries(nn_cfg PUBLIC
    nn_core
    ${LIBXML2_LIBRARIES}
    Threads::Threads
)

# Set library version
set_target_properties(nn_cfg PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
)

# Installation
install(TARGETS nn_cfg
    LIBRARY DESTINATION lib
)
