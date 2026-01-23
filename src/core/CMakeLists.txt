# Core library sources (module registry only)
set(CORE_SOURCES
    nn_module_registry.c
    nn_message_queue.c
)

# Create shared library for core
add_library(nn_core SHARED ${CORE_SOURCES})

# Include directories for core library
target_include_directories(nn_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include
)

# Link GLib
target_link_libraries(nn_core PUBLIC ${GLIB_LIBRARIES})

# Set library version
set_target_properties(nn_core PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
)

# Installation
install(TARGETS nn_core
    LIBRARY DESTINATION lib
)
