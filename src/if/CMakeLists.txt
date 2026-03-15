set(IF_SOURCES
    if.c
    if_map.c
    if_pub.c
    if_cfg_apply.c
    if_main.c
    if_cli.c
    if_bdr.c
)

add_executable(netnexus-if if_proc.c ${IF_SOURCES})

target_include_directories(netnexus-if PRIVATE .)

target_link_libraries(netnexus-if PRIVATE
    dev_api
    cli_api
    db_api
    route_api
    ${GLIB_LIBRARIES}
)

set_target_properties(netnexus-if PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
)

add_custom_command(TARGET netnexus-if POST_BUILD
    COMMAND sudo -n setcap cap_net_admin,cap_net_bind_service=+ep $<TARGET_FILE:netnexus-if>
    || echo "提示：如需绑定 179 端口，请手动执行：sudo setcap cap_net_admin,cap_net_bind_service=+ep $<TARGET_FILE:netnexus-if>"
    COMMENT "授予 cap_net_admin + cap_net_bind_service"
)

install(TARGETS netnexus-if RUNTIME DESTINATION bin)

add_subdirectory(lib)
