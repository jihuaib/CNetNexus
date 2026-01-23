# Build order: core first, then cfg, then other modules
add_subdirectory(core)
add_subdirectory(modules/cfg)
add_subdirectory(modules)

# Create main executable
add_executable(netnexus main.c)

# Link against libraries
# Use --no-as-needed to ensure module constructor functions are called
target_link_libraries(netnexus PRIVATE
    nn_core
    -Wl,--no-as-needed
    nn_cfg
    nn_bgp
    nn_dev
    -Wl,--as-needed
    Threads::Threads
)

# Set RPATH to find libraries in lib/ directory at runtime
set_target_properties(netnexus PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
)

# Installation
install(TARGETS netnexus
    RUNTIME DESTINATION bin
)
