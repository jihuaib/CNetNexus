set(SBMP_SOURCES
    sbmp_main.c
    sbmp_bmp.c
    sbmp_cli.c
    sbmp_db.c
    sbmp_bdr.c
    sbmp_rib.c
)

add_executable(netnexus-sbmp sbmp_proc.c ${SBMP_SOURCES})

target_include_directories(netnexus-sbmp PRIVATE
    .
)

target_link_libraries(netnexus-sbmp PRIVATE
    dev_api
    cli_api
    db_api
    utils
    bgp_api
    ${GLIB_LIBRARIES}
)

set_target_properties(netnexus-sbmp PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
)

install(TARGETS netnexus-sbmp RUNTIME DESTINATION bin)
