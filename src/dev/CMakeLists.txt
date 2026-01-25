# Dev module sources
set(NN_DEV_SOURCES
    nn_dev_main.c
        nn_dev_module.c
        nn_dev_mq.c
        nn_dev_pubsub.c
)

# Create shared library for dev module
add_library(nn_dev SHARED ${NN_DEV_SOURCES})

# Include directories
target_include_directories(nn_dev PRIVATE
    ${PROJECT_SOURCE_DIR}/include
        ../cfg
)

# Link libraries
target_link_libraries(nn_dev PRIVATE
    ${GLIB_LIBRARIES}
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
