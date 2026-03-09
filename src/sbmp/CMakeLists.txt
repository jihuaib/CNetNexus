# SBMP module sources
set(SBMP_SOURCES
    sbmp_main.c
    sbmp_cli.c
    sbmp_db.c
    sbmp_bdr.c
)

# Create shared library for SBMP module
add_library(sbmp SHARED ${SBMP_SOURCES})

# Include directories
target_include_directories(sbmp PRIVATE
    .
)

# Link libraries
target_link_libraries(sbmp PRIVATE
    dev_api
    cli_api
    utils
)

# Set library version
set_target_properties(sbmp PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
)
