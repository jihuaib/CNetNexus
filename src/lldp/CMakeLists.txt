set(LLDP_SOURCES
    lldp_main.c
    lldp_cli.c
    lldp_db.c
    lldp_bdr.c
    db/lldp_db_common.c
    db/lldp_db_protocol.c
    db/lldp_db_interface.c
    work/lldp_worker.c
    work/lldp_packet.c
    work/lldp_show.c
    work/lldp_snmp_report.c
)

add_executable(netnexus-lldp lldp_proc.c ${LLDP_SOURCES})

target_include_directories(netnexus-lldp PRIVATE . db work)

target_link_libraries(netnexus-lldp PRIVATE
    dev_api
    cli_api
    db_api
    if_api
    utils
)

set_target_properties(netnexus-lldp PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
)

add_custom_command(TARGET netnexus-lldp POST_BUILD
    COMMAND sudo -n setcap cap_net_admin,cap_net_raw=+ep $<TARGET_FILE:netnexus-lldp>
    || echo "提示：如需收发 LLDP 报文，请手动执行：sudo setcap cap_net_admin,cap_net_raw=+ep $<TARGET_FILE:netnexus-lldp>"
    COMMENT "授予 netnexus-lldp 原始报文权限"
)

install(TARGETS netnexus-lldp RUNTIME DESTINATION bin)
