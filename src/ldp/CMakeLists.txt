set(LDP_SOURCES
    ldp_main.c
    ldp_cli.c
    ldp_db.c
    ldp_bdr.c
    db/ldp_db_common.c
    db/ldp_db_protocol.c
    db/ldp_db_interface.c
    work/ldp_worker.c
    work/ldp_pkt.c
    work/ldp_discovery.c
    work/ldp_session.c
    work/ldp_lib.c
    work/ldp_route_sync.c
    work/ldp_show.c
)

add_executable(netnexus-ldp ldp_proc.c ${LDP_SOURCES})

target_include_directories(netnexus-ldp PRIVATE . db work)

target_link_libraries(netnexus-ldp PRIVATE
    dev_api
    cli_api
    db_api
    if_api
    route_api
    tunnel_api
    utils
)

set_target_properties(netnexus-ldp PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
)

add_custom_command(TARGET netnexus-ldp POST_BUILD
    COMMAND sudo -n setcap cap_net_admin,cap_net_bind_service,cap_net_raw=+ep $<TARGET_FILE:netnexus-ldp>
    || echo "提示：如需收发 LDP 报文，请手动执行：sudo setcap cap_net_admin,cap_net_bind_service,cap_net_raw=+ep $<TARGET_FILE:netnexus-ldp>"
    COMMENT "授予 netnexus-ldp 原始报文和管理权限"
)

install(TARGETS netnexus-ldp RUNTIME DESTINATION bin)
