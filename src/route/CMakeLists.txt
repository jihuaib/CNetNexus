set(ROUTE_SOURCES
    route_main.c
    route_cli.c
)

add_executable(netnexus-route route_proc.c ${ROUTE_SOURCES})

target_include_directories(netnexus-route PRIVATE .)

target_link_libraries(netnexus-route PRIVATE
    dev_api
    cli_api
    db_api
)

set_target_properties(netnexus-route PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
)

install(TARGETS netnexus-route RUNTIME DESTINATION bin)

add_subdirectory(lib)
