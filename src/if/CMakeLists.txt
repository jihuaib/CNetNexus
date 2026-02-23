# If module sources
set(IF_SOURCES
    if.c
    if_map.c
    if_main.c
    if_cli.c
)

# Create shared library for if module
add_library(if SHARED ${IF_SOURCES})

# Include directories
target_include_directories(if PRIVATE
    .
)

# Link libraries
target_link_libraries(if PRIVATE
    dev_api
    ${GLIB_LIBRARIES}
)

# Set library version
set_target_properties(if PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
)

add_subdirectory(lib)
