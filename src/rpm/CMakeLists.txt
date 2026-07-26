set(RPM_SOURCES
    rpm_main.c
    rpm_cli.c
    rpm_db.c
    rpm_bdr.c
)

add_subdirectory(lib)

add_executable(netnexus-rpm rpm_proc.c ${RPM_SOURCES})

target_include_directories(netnexus-rpm PRIVATE .)

target_link_libraries(netnexus-rpm PRIVATE
    dev_api
    cli_api
    db_api
    rpm_api
    utils
    ${GLIB_LIBRARIES}
    Threads::Threads
)

set_target_properties(netnexus-rpm PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
)

install(TARGETS netnexus-rpm RUNTIME DESTINATION bin)
