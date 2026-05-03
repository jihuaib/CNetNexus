set(VRF_SOURCES
    vrf_main.c
    vrf_cli.c
    vrf_db.c
    vrf_bdr.c
    work/vrf_table.c
    work/vrf_pub.c
    work/vrf_show.c
    work/vrf_cfg_apply.c
    work/vrf_os.c
    work/vrf_worker.c
)

add_executable(netnexus-vrf vrf_proc.c ${VRF_SOURCES})

target_include_directories(netnexus-vrf PRIVATE . work)

target_link_libraries(netnexus-vrf PRIVATE
    dev_api
    cli_api
    db_api
    utils
)

set_target_properties(netnexus-vrf PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
)

add_custom_command(TARGET netnexus-vrf POST_BUILD
    COMMAND sudo -n setcap cap_net_admin=+ep $<TARGET_FILE:netnexus-vrf>
    || echo "提示：如需创建 L3VRF 设备，请手动执行：sudo setcap cap_net_admin=+ep $<TARGET_FILE:netnexus-vrf>"
    COMMENT "授予 netnexus-vrf netlink L3VRF 设备管理权限"
)

install(TARGETS netnexus-vrf RUNTIME DESTINATION bin)

add_subdirectory(lib)
