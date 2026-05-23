set(BGP_SOURCES
    bgp_main.c
    bgp_cli.c
    bgp_bmp_cli.c
    bgp_bmp_db.c
    bgp_db.c
    db/bgp_db_common.c
    db/bgp_db_protocol.c
    db/bgp_db_session.c
    db/bgp_db_neighbor.c
    db/bgp_db_vrf.c
    db/bgp_db_instance.c
    db/bgp_db_qp_route.c
    bgp_bdr.c
    work/bgp_cfg_apply.c
    work/bgp_adj_rib_in.c
    work/bgp_if_cache.c
    work/bgp_peer.c
    work/bgp_conn.c
    work/bgp_pkt.c
    work/bgp_pkt_build.c
    work/bgp_pkt_build_ipv4uc.c
    work/bgp_pkt_build_ipv6uc.c
    work/bgp_pkt_build_labeled.c
    work/bgp_pkt_build_ipv4qp.c
    work/bgp_pkt_build_ipv6qp.c
    work/bgp_calc.c
    work/bgp_fsm.c
    work/bgp_session.c
    work/bgp_instance.c
    work/bgp_vrf.c
    work/bgp_protocol.c
    work/bgp_rd.c
    work/bgp_relay.c
    work/bgp_attr_intern.c
    work/bgp_ext_community.c
    work/bgp_rib.c
    work/bgp_update_group.c
    work/bgp_adj_rib_out.c
    work/bgp_show.c
    work/bgp_worker.c
    work/bgp_cmd.c
    work/bgp_import_route.c
    work/bgp_import_rib.c
    work/bgp_route_flush.c
    work/bgp_apply_vrf.c
    bmp/bgp_bmp.c
    bmp/bgp_bmp_cfg_apply.c
    bmp/bgp_bmp_pkt.c
    bmp/bgp_bmp_thread.c
)

add_subdirectory(lib)

add_executable(netnexus-bgp bgp_proc.c ${BGP_SOURCES})

target_include_directories(netnexus-bgp PRIVATE . db work bmp)

target_link_libraries(netnexus-bgp PRIVATE
    dev_api
    cli_api
    db_api
    if_api
    vrf_api
    route_api
    tunnel_api
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
