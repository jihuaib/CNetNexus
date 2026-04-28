# IPC 子库
add_subdirectory(lib)

# Dev module sources
set(DEV_SOURCES
    dev_main.c
    dev_cli.c
    dev_module.c
    dev_conf_parser.c
    dev_db.c
    dev_bdr.c
)

# Create shared library for dev module
add_library(dev SHARED ${DEV_SOURCES})

# Include directories
target_include_directories(dev PRIVATE
    .
)

# Link libraries
target_link_libraries(dev PRIVATE
    dev_api
    cli_api
    db_api
    dl
    ${GLIB_LIBRARIES}
)

# Set library version
set_target_properties(dev PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
)
