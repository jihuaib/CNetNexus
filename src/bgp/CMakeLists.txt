# BGP 独立进程
set(BGP_SOURCES
    bgp_process.c
    bgp_main.c
    bgp_cli.c
    bgp_api.c
)

# 构建为可执行文件
add_executable(bgp ${BGP_SOURCES})

# 包含目录
target_include_directories(bgp PRIVATE
    .
)

# 链接依赖
target_link_libraries(bgp PRIVATE
    db_client
    cli_param
    ipc
    utils
    ${GLIB_LIBRARIES}
    Threads::Threads
)

# 设置 RPATH
set_target_properties(bgp PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
