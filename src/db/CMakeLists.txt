set(DB_SOURCES
    db_main.c
    db_schema.c
    db_api.c
    db_cli.c
    db_serialize.c
    db_ipc_handler.c
)

add_executable(netnexus-db db_proc.c ${DB_SOURCES})

target_include_directories(netnexus-db PRIVATE
    .
    ${SQLite3_INCLUDE_DIRS}
)

target_link_libraries(netnexus-db PRIVATE
    dev_api
    cli_api
    ${GLIB_LIBRARIES}
    ${SQLite3_LIBRARIES}
    Threads::Threads
)

set_target_properties(netnexus-db PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
)

install(TARGETS netnexus-db RUNTIME DESTINATION bin)

add_subdirectory(lib)
