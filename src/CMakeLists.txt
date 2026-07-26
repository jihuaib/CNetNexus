cmake_minimum_required(VERSION 3.16)

project(netnexus C)

set(CMAKE_C_STANDARD 11)

# =========================
# 公共库
# =========================
add_subdirectory(utils)
add_subdirectory(dev)

# =========================
# 模块独立进程（各自构建为可执行文件）
# =========================
add_subdirectory(access)
add_subdirectory(cli)
add_subdirectory(db)
add_subdirectory(fib)
add_subdirectory(route)
add_subdirectory(if)
add_subdirectory(tunnel)
add_subdirectory(bgp)
add_subdirectory(vrf)
add_subdirectory(sbmp)
add_subdirectory(isis)
add_subdirectory(ldp)
add_subdirectory(lldp)
add_subdirectory(snmp)
add_subdirectory(ospf)
add_subdirectory(ospfv3)

# =========================
# DEV 主进程（supervisor）
# =========================
add_executable(netnexus main.c)

target_link_libraries(netnexus PRIVATE
    utils
    dev
    dev_api
    Threads::Threads
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
    COMMAND sudo -n setcap cap_net_admin,cap_net_bind_service,cap_net_raw=+ep $<TARGET_FILE:netnexus>
    || echo "提示：如需 MPLS ping/raw socket，请手动执行：sudo setcap cap_net_admin,cap_net_bind_service,cap_net_raw=+ep $<TARGET_FILE:netnexus>"
    COMMENT "授予 cap_net_admin + cap_net_bind_service + cap_net_raw"
)

# =========================
# Install
# =========================
install(TARGETS netnexus
    RUNTIME DESTINATION bin
)
