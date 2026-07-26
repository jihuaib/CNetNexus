set(OSPF_SOURCES
    ospf_proc.c
    ospf_main.c
    ospf_cli.c
    ospf_db.c
    ospf_bdr.c
    work/ospf_worker.c
    work/ospf_packet.c
    work/ospf_lsa.c
    work/ospf_spf.c
    work/ospf_route_sync.c
    work/ospf_show.c
)

add_executable(netnexus-ospf ${OSPF_SOURCES})

target_include_directories(netnexus-ospf PRIVATE . work)

target_link_libraries(netnexus-ospf PRIVATE
    dev_api
    cli_api
    db_api
    if_api
    route_api
    utils
)

set_target_properties(netnexus-ospf PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
)

add_custom_command(TARGET netnexus-ospf POST_BUILD
    COMMAND sudo -n setcap cap_net_admin,cap_net_bind_service,cap_net_raw=+ep $<TARGET_FILE:netnexus-ospf>
    || echo "Hint: grant cap_net_raw to netnexus-ospf to exchange OSPF packets"
    COMMENT "Granting raw-socket capability to netnexus-ospf"
)

install(TARGETS netnexus-ospf RUNTIME DESTINATION bin)
