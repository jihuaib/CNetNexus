set(VRF_SOURCES
    vrf_main.c
    vrf_cli.c
)

add_executable(netnexus-vrf vrf_proc.c ${VRF_SOURCES})

target_include_directories(netnexus-vrf PRIVATE .)

target_link_libraries(netnexus-vrf PRIVATE
    dev_api
    cli_api
    utils
)

set_target_properties(netnexus-vrf PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
)

install(TARGETS netnexus-vrf RUNTIME DESTINATION bin)

add_subdirectory(lib)
