# BGP module sources
set(NN_BGP_SOURCES
    nn_bgp_main.c
    nn_bgp_cfg.c
)

# Create shared library for BGP module
add_library(nn_bgp SHARED ${NN_BGP_SOURCES})

# Include directories
target_include_directories(nn_bgp PRIVATE
    .
    ${PROJECT_SOURCE_DIR}/include
)

# Set library version
set_target_properties(nn_bgp PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
)

# Installation
install(TARGETS nn_bgp
    LIBRARY DESTINATION lib
)

# Install commands.xml to share/netnexus/modules/bgp/
install(FILES commands.xml
    DESTINATION share/netnexus/modules/bgp
)
