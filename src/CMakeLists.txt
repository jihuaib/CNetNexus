# Build order: utils first, then db, then cfg, then other modules
add_subdirectory(utils)
add_subdirectory(db)
add_subdirectory(cfg)
add_subdirectory(dev)
add_subdirectory(bgp)

# Create main executable
add_executable(netnexus 
    main.c
)

# Link against libraries
# Use --no-as-needed to ensure module constructor functions are called
target_link_libraries(netnexus PRIVATE
    -Wl,--no-as-needed
    nn_utils
    nn_db
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
