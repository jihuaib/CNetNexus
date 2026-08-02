# BGP API 库
set(BGP_API_SOURCES
    bgp_api.c
    bgp_parse.c
    bgp_parse_af.c
    bgp_parse_attr.c
    bgp_prefix_sid.c
    bgp_parse_common.c
    bgp_parse_ipv4uc.c
    bgp_parse_ipv6uc.c
    bgp_parse_ipv4qp.c
    bgp_parse_ipv6qp.c
    bgp_parse_labeled.c
    bgp_parse_vpn.c
    bgp_parse_evpn.c
    bgp_parse_flowspec.c
    bgp_parse_open.c
    bgp_parse_notif.c
)

add_library(bgp_api SHARED ${BGP_API_SOURCES})

target_include_directories(bgp_api PRIVATE
    .
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(bgp_api PUBLIC
    dev_api
    ${GLIB_LIBRARIES}
)

set_target_properties(bgp_api PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
)

install(TARGETS bgp_api LIBRARY DESTINATION lib)
