# BGP module sources
set(BGP_MODULE_SOURCES
    bgp_module.c
)

# Create shared library for BGP module
add_library(nn_bgp SHARED ${BGP_MODULE_SOURCES})

# Link against core library
target_link_libraries(nn_bgp PRIVATE nn_core)

# Include directories
target_include_directories(nn_bgp PRIVATE
    ${PROJECT_SOURCE_DIR}/include
        ../cfg
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
