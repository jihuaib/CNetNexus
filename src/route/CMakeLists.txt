set(ROUTE_SOURCES
    route_main.c
    route_cli.c
    route_bdr.c
    route_db.c
    work/route_rib.c
    work/route_calc.c
    work/route_pub.c
    work/route_relay.c
    work/route_static.c
    work/route_os.c
    work/route_show.c
    work/route_work.c
    work/route_worker.c
    work/route_cfg_apply.c
)

add_executable(netnexus-route route_proc.c ${ROUTE_SOURCES})

target_include_directories(netnexus-route PRIVATE . work)

target_link_libraries(netnexus-route PRIVATE
    dev_api
    cli_api
    db_api
    if_api
)

set_target_properties(netnexus-route PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
)

add_custom_command(TARGET netnexus-route POST_BUILD
    COMMAND sudo -n setcap cap_net_admin,cap_net_bind_service=+ep $<TARGET_FILE:netnexus-route>
    || echo "提示：如需修改内核路由表，请手动执行：sudo setcap cap_net_admin,cap_net_bind_service=+ep $<TARGET_FILE:netnexus-route>"
    COMMENT "授予 cap_net_admin + cap_net_bind_service"
)

install(TARGETS netnexus-route RUNTIME DESTINATION bin)

add_subdirectory(lib)
