set(TUNNEL_SOURCES
    tunnel_main.c
    tunnel_cli.c
    work/tunnel_worker.c
    work/tunnel_show.c
    work/tunnel_rib.c
)

add_subdirectory(lib)

add_executable(netnexus-tunnel tunnel_proc.c ${TUNNEL_SOURCES})

target_include_directories(netnexus-tunnel PRIVATE . work)

target_link_libraries(netnexus-tunnel PRIVATE
    dev_api
    cli_api
    tunnel_api
    utils
)

set_target_properties(netnexus-tunnel PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
)

install(TARGETS netnexus-tunnel RUNTIME DESTINATION bin)
