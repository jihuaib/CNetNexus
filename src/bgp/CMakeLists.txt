set(BGP_SOURCES
    bgp_main.c
    bgp_cli.c
    bgp_cfg_apply.c
    bgp_db.c
    bgp_bdr.c
    bgp_peer.c
    bgp_conn.c
    bgp_pkt.c
    bgp_pkt_build.c
    bgp_pkt_build_ipv4uc.c
    bgp_pkt_build_ipv6uc.c
    bgp_pub.c
    bgp_calc.c
    bgp_session.c
    bgp_instance.c
    bgp_vrf.c
    bgp_protocol.c
    bgp_rib.c
)

add_subdirectory(lib)

add_executable(netnexus-bgp bgp_proc.c ${BGP_SOURCES})

target_include_directories(netnexus-bgp PRIVATE .)

target_link_libraries(netnexus-bgp PRIVATE
    dev_api
    cli_api
    db_api
    utils
    bgp_api
)

set_target_properties(netnexus-bgp PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
)

add_custom_command(TARGET netnexus-bgp POST_BUILD
    COMMAND sudo -n setcap cap_net_admin,cap_net_bind_service=+ep $<TARGET_FILE:netnexus-bgp>
    || echo "提示：如需绑定 179 端口，请手动执行：sudo setcap cap_net_admin,cap_net_bind_service=+ep $<TARGET_FILE:netnexus-bgp>"
    COMMENT "授予 netnexus-bgp 端口绑定权限"
)

install(TARGETS netnexus-bgp RUNTIME DESTINATION bin)
