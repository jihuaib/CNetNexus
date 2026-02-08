# DB 模块 - 添加 lib 子目录（构建 db_client.so）
add_subdirectory(lib)

# DB 守护进程源文件
set(DB_SOURCES
    db_main.c
    db_registry.c
    db_schema.c
    db_api.c
    db_cli.c
    db_ipc_handler.c
    db_ipc_registry_handler.c
    db_process.c
)

# 构建 DB 守护进程
add_executable(db_proc ${DB_SOURCES})

target_include_directories(db_proc PRIVATE
    .
    ${CMAKE_CURRENT_SOURCE_DIR}/lib
    ${CMAKE_SOURCE_DIR}/src/cli/param
    ${SQLite3_INCLUDE_DIRS}
)

# 链接依赖（只依赖工具库）
target_link_libraries(db_proc PRIVATE
    cli_param
    ipc
    utils
    db_client
    ${GLIB_LIBRARIES}
    ${SQLite3_LIBRARIES}
    Threads::Threads
)

set_target_properties(db_proc PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    OUTPUT_NAME "db"
)
