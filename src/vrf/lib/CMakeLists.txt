set(VRF_API_SOURCES
    vrf_api.c
)

add_library(vrf_api SHARED ${VRF_API_SOURCES})

target_include_directories(vrf_api PRIVATE
    .
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(vrf_api PUBLIC
    vrf
    dev_api
    ${GLIB_LIBRARIES}
)

set_target_properties(vrf_api PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
)
