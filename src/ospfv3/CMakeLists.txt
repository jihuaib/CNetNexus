set(OSPFV3_SOURCES
    ospfv3_proc.c
    ospfv3_main.c
    ospfv3_cli.c
    ospfv3_db.c
    ospfv3_bdr.c
    work/ospfv3_worker.c
    work/ospfv3_packet.c
    work/ospfv3_lsa.c
    work/ospfv3_spf.c
    work/ospfv3_route_sync.c
    work/ospfv3_show.c
)

add_executable(netnexus-ospfv3 ${OSPFV3_SOURCES})

target_include_directories(netnexus-ospfv3 PRIVATE . work)

target_link_libraries(netnexus-ospfv3 PRIVATE
    dev_api
    cli_api
    db_api
    if_api
    route_api
    utils
)

set_target_properties(netnexus-ospfv3 PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
)

add_custom_command(TARGET netnexus-ospfv3 POST_BUILD
    COMMAND sudo -n setcap cap_net_admin,cap_net_bind_service,cap_net_raw=+ep $<TARGET_FILE:netnexus-ospfv3>
    || echo "Hint: grant cap_net_raw to netnexus-ospfv3 to exchange OSPFV3 packets"
    COMMENT "Granting raw-socket capability to netnexus-ospfv3"
)

install(TARGETS netnexus-ospfv3 RUNTIME DESTINATION bin)
