set(SRV6_SOURCES
    srv6_main.c
    srv6_cli.c
    srv6_db.c
    srv6_bdr.c
    work/srv6_worker.c
)

add_subdirectory(lib)

add_executable(netnexus-srv6 srv6_proc.c ${SRV6_SOURCES})

target_include_directories(netnexus-srv6 PRIVATE
    .
    work
)

target_link_libraries(netnexus-srv6 PRIVATE
    dev_api
    cli_api
    db_api
    fib_api
    route_api
    vrf_api
    srv6_api
    utils
    ${GLIB_LIBRARIES}
    Threads::Threads
)

set_target_properties(netnexus-srv6 PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
)

install(TARGETS netnexus-srv6 RUNTIME DESTINATION bin)
