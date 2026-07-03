set(SNMP_SOURCES
    snmp_agent.c
    snmp_bdr.c
    snmp_cli.c
    snmp_db.c
    snmp_main.c
)

add_executable(netnexus-snmp snmp_proc.c ${SNMP_SOURCES})

target_include_directories(netnexus-snmp PRIVATE .)

target_link_libraries(netnexus-snmp PRIVATE
    dev_api
    cli_api
    db_api
    utils
    ${NETSNMP_AGENT_LIBRARIES}
    ${GLIB_LIBRARIES}
    Threads::Threads
)

target_compile_options(netnexus-snmp PRIVATE ${NETSNMP_AGENT_CFLAGS_OTHER})
target_link_directories(netnexus-snmp PRIVATE ${NETSNMP_AGENT_LIBRARY_DIRS})

set_target_properties(netnexus-snmp PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
)

add_custom_command(TARGET netnexus-snmp POST_BUILD
    COMMAND sudo -n setcap cap_net_bind_service=+ep $<TARGET_FILE:netnexus-snmp>
    || echo "提示：如需绑定 SNMP 161 端口，请手动执行：sudo setcap cap_net_bind_service=+ep $<TARGET_FILE:netnexus-snmp>"
    COMMENT "授予 netnexus-snmp 端口绑定权限"
)

install(TARGETS netnexus-snmp RUNTIME DESTINATION bin)
