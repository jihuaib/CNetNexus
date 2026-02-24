# BGP module sources
set(BGP_SOURCES
    bgp_main.c
    bgp_cli.c
    bgp_db.c
)

# Create shared library for BGP module
add_library(bgp SHARED ${BGP_SOURCES})

# Include directories
target_include_directories(bgp PRIVATE
    .
)

# Link libraries
target_link_libraries(bgp PRIVATE
    dev_api
)

# Set library version
set_target_properties(bgp PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
)

add_subdirectory(lib)
