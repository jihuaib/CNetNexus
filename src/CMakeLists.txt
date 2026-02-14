# Build order: utils first, then dev/ipc, then db, then cfg
add_subdirectory(utils)
add_subdirectory(dev)
add_subdirectory(db)
add_subdirectory(cfg)
add_subdirectory(if)
add_subdirectory(route)
add_subdirectory(bgp)

# Create main executable
add_executable(netnexus 
    main.c
)

# Link against libraries
# Use --no-as-needed to ensure module constructor functions are called
target_link_libraries(netnexus PRIVATE
    -Wl,--no-as-needed
    utils
    dev
    db
    cfg
    if
    bgp
    -Wl,--as-needed
    Threads::Threads
)

# Set RPATH to find libraries in lib/ directory at runtime
set_target_properties(netnexus PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
)

# Automated setcap for development
# Note: This requires sudo permissions without password for 'setcap' or the user running make must be root.
add_custom_command(TARGET netnexus POST_BUILD
    COMMAND sudo setcap cap_net_admin+ep $<TARGET_FILE:netnexus> || echo "Warning: Failed to setcap on netnexus. You may need to run 'sudo setcap cap_net_admin+ep ./build/bin/netnexus' manually."
    COMMENT "Setting CAP_NET_ADMIN on netnexus executable"
)

# Installation
install(TARGETS netnexus
    RUNTIME DESTINATION bin
)
