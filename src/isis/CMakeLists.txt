set(ISIS_SOURCES
    isis_main.c
    isis_cli.c
    isis_db.c
    db/isis_db_common.c
    db/isis_db_instance.c
    db/isis_db_interface.c
    isis_bdr.c
    work/isis_worker.c
    work/isis_cfg_apply.c
    work/isis_route.c
    work/isis_route_sync.c
    work/isis_show.c
    work/isis_neighbor.c
    work/isis_lsp.c
    work/isis_spf.c
)

add_executable(netnexus-isis isis_proc.c ${ISIS_SOURCES})

target_include_directories(netnexus-isis PRIVATE . db work)

target_link_libraries(netnexus-isis PRIVATE
    dev_api
    cli_api
    db_api
    if_api
    route_api
    utils
)

set_target_properties(netnexus-isis PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
)

add_custom_command(TARGET netnexus-isis POST_BUILD
    COMMAND sudo -n setcap cap_net_admin,cap_net_bind_service,cap_net_raw=+ep $<TARGET_FILE:netnexus-isis>
    || echo "提示：如需收发 ISIS 报文，请手动执行：sudo setcap cap_net_admin,cap_net_bind_service,cap_net_raw=+ep $<TARGET_FILE:netnexus-isis>"
    COMMENT "授予 netnexus-isis 原始报文和管理权限"
)

install(TARGETS netnexus-isis RUNTIME DESTINATION bin)
