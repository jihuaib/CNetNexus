set(VRF_SOURCES
    vrf_main.c
    vrf_cli.c
)

add_library(vrf SHARED ${VRF_SOURCES})

target_include_directories(vrf PRIVATE .)

target_link_libraries(vrf PRIVATE
    dev_api
    cli_api
    utils
)

set_target_properties(vrf PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
)

add_subdirectory(lib)
