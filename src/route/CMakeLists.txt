# Route 独立进程
set(ROUTE_SOURCES
    route_process.c
    route_main.c
    route_cli.c
    route_api.c
)

add_executable(route ${ROUTE_SOURCES})

target_include_directories(route PRIVATE
    .
)

target_link_libraries(route PRIVATE
    cfg
    db
    dev
    ipc
    utils
    ${GLIB_LIBRARIES}
    Threads::Threads
)

set_target_properties(route PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
