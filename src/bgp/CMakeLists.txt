# BGP module sources
set(NN_BGP_SOURCES
    nn_bgp_main.c
    nn_bgp_cli.c
    nn_bgp_api.c
)

# Create shared library for BGP module
add_library(nn_bgp SHARED ${NN_BGP_SOURCES})

# Include directories
target_include_directories(nn_bgp PRIVATE
    .
)

# Set library version
set_target_properties(nn_bgp PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
)
