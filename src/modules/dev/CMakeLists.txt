# Dev module sources
set(DEV_MODULE_SOURCES
    dev_module.c
)

# Create shared library for dev module
add_library(nn_dev SHARED ${DEV_MODULE_SOURCES})

# Link against core library
target_link_libraries(nn_dev PRIVATE nn_core)

# Include directories
target_include_directories(nn_dev PRIVATE
    ${PROJECT_SOURCE_DIR}/include
        ../cfg
)

# Set library version
set_target_properties(nn_dev PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
)

# Installation
install(TARGETS nn_dev
    LIBRARY DESTINATION lib
)

# Install commands.xml to share/netnexus/modules/dev/
install(FILES commands.xml
    DESTINATION share/netnexus/modules/dev
)
