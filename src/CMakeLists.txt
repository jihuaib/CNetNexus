cmake_minimum_required(VERSION 3.16)

project(netnexus C)

set(CMAKE_C_STANDARD 11)

# =========================
# Modules
# =========================
add_subdirectory(utils)
add_subdirectory(dev)

# plugin modules
add_subdirectory(cfg)
add_subdirectory(db)
add_subdirectory(if)
add_subdirectory(route)
add_subdirectory(bgp)
add_subdirectory(vrf)
add_subdirectory(sbmp)

# =========================
# Main executable
# =========================
add_executable(netnexus
        main.c
)

# =========================
# Link libraries
# =========================
#
# 注意：
# cfg/db/if/route/bgp/vrf 实际运行时通过 dlopen 加载
# 这里链接仅用于：
#   1. IDE 符号索引
#   2. dev 模块引用符号
#
target_link_libraries(netnexus PRIVATE
    utils
    dev

    # plugin libs (IDE symbol resolution)
    cfg
    db
    if
    route
    bgp
    vrf
    sbmp

    dl
    Threads::Threads
)

# 允许 unresolved symbols（插件架构）
target_link_options(netnexus PRIVATE
    -Wl,--unresolved-symbols=ignore-in-shared-libs
)

# =========================
# RPATH
# =========================
set_target_properties(netnexus PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
)

# =========================
# Auto setcap (dev only)
# =========================
add_custom_command(TARGET netnexus POST_BUILD
    COMMAND sudo -n setcap cap_net_admin,cap_net_bind_service=+ep $<TARGET_FILE:netnexus>
    || echo "提示：如需绑定 179 端口，请手动执行：sudo setcap cap_net_admin,cap_net_bind_service=+ep $<TARGET_FILE:netnexus>"
    COMMENT "授予 cap_net_admin + cap_net_bind_service"
)

# =========================
# Install
# =========================
install(TARGETS netnexus
    RUNTIME DESTINATION bin
)