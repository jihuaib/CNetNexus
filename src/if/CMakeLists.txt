# If module sources
set(NN_IF_SOURCES
    nn_if.c
    nn_if_map.c
    nn_if_main.c
    nn_if_cli.c
)

# Create shared library for if module
add_library(nn_if SHARED ${NN_IF_SOURCES})

# Include directories
target_include_directories(nn_if PRIVATE
    .
)

# Link libraries
target_link_libraries(nn_if PRIVATE
    ${GLIB_LIBRARIES}
)

# Set library version
set_target_properties(nn_if PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
)