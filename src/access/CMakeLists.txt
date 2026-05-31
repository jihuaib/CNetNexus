set(ACCESS_SOURCES
    access_main.c
    access_line.c
    access_history.c
    access_telnet.c
    access_console.c
    access_db.c
)

add_executable(netnexus-access access_proc.c ${ACCESS_SOURCES})

target_include_directories(netnexus-access PUBLIC
    .
)

target_link_libraries(netnexus-access PRIVATE
    dev_api
    db_api
    cli_api
    utils
    ${GLIB_LIBRARIES}
    Threads::Threads
    util
)

set_target_properties(netnexus-access PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
)

# telnet vty 默认端口 23（<1024 特权端口），需 cap_net_bind_service 才能 bind
add_custom_command(TARGET netnexus-access POST_BUILD
    COMMAND sudo -n setcap cap_net_bind_service=+ep $<TARGET_FILE:netnexus-access>
    || echo "提示：如需绑定 telnet 23 端口，请手动执行：sudo setcap cap_net_bind_service=+ep $<TARGET_FILE:netnexus-access>"
    COMMENT "授予 netnexus-access 端口绑定权限")

install(TARGETS netnexus-access RUNTIME DESTINATION bin)

# console 客户端：把本地终端桥接到 ACCESS 的 console unix socket（telnet 连不了 unix socket）
add_executable(netnexus-console netnexus_console.c)
target_include_directories(netnexus-console PUBLIC .)
install(TARGETS netnexus-console RUNTIME DESTINATION bin)
