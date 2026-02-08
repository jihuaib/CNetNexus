# IF 独立进程
set(IF_SOURCES
    if_process.c
    if_main.c
    if_cli.c
    if.c
    if_map.c
)

add_executable(if ${IF_SOURCES})

target_include_directories(if PRIVATE
    .
)

target_link_libraries(if PRIVATE
    db_client
    cli_param
    ipc
    utils
    ${GLIB_LIBRARIES}
    Threads::Threads
)

set_target_properties(if PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
