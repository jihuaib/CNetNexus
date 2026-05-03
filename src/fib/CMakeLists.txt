set(FIB_SOURCES
    fib_main.c
    work/fib_rib.c
    work/fib_os.c
    work/fib_show.c
    work/fib_worker.c
)

add_executable(netnexus-fib fib_proc.c ${FIB_SOURCES})

target_include_directories(netnexus-fib PRIVATE . work)

target_link_libraries(netnexus-fib PRIVATE
    dev_api
    cli_api
)

set_target_properties(netnexus-fib PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
)

add_custom_command(TARGET netnexus-fib POST_BUILD
    COMMAND sudo -n setcap cap_net_admin,cap_net_bind_service=+ep $<TARGET_FILE:netnexus-fib>
    || echo "提示：如需修改内核 FIB，请手动执行：sudo setcap cap_net_admin,cap_net_bind_service=+ep $<TARGET_FILE:netnexus-fib>"
    COMMENT "授予 cap_net_admin + cap_net_bind_service"
)

install(TARGETS netnexus-fib RUNTIME DESTINATION bin)

add_subdirectory(lib)
