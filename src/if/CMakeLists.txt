set(IF_SOURCES
    if.c
    if_map.c
    if_cfg_apply.c
    if_main.c
    if_cli.c
    if_bdr.c
)

add_executable(netnexus-if if_proc.c ${IF_SOURCES})

target_include_directories(netnexus-if PRIVATE .)

target_link_libraries(netnexus-if PRIVATE
    dev_api
    cli_api
    db_api
    ${GLIB_LIBRARIES}
)

set_target_properties(netnexus-if PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
)

install(TARGETS netnexus-if RUNTIME DESTINATION bin)

add_subdirectory(lib)
