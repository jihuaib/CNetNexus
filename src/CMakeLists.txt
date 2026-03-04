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
# Module libraries (db, cfg, if, route, bgp) are loaded dynamically via dlopen
# --unresolved-symbols=ignore-in-shared-libs: dev 引用了 cfg/db 的符号,
# 这些符号将在运行时通过 dlopen 加载的 .so 中解析
target_link_libraries(netnexus PRIVATE
    utils
    dev
    dl
    Threads::Threads
    -Wl,--unresolved-symbols=ignore-in-shared-libs
)

# Set RPATH to find libraries in lib/ directory at runtime
set_target_properties(netnexus PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
)

# Automated setcap for development
# Note: This requires sudo permissions without password for 'setcap' or the user running make must be root.
add_custom_command(TARGET netnexus POST_BUILD
    COMMAND sudo -n setcap cap_net_admin,cap_net_bind_service=+ep $<TARGET_FILE:netnexus> || echo "提示：如需绑定 179 端口，请手动执行：sudo setcap cap_net_admin,cap_net_bind_service=+ep $<TARGET_FILE:netnexus>"
    COMMENT "授予 cap_net_admin + cap_net_bind_service"
)

# Installation
install(TARGETS netnexus
    RUNTIME DESTINATION bin
)
