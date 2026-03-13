# BGP API 库
set(BGP_API_SOURCES
    bgp_api.c
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

# BGP UPDATE 解析库（独立，供 bgp 模块和 sbmp 模块复用）
set(BGP_PARSE_SOURCES
    bgp_parse.c
    bgp_parse_af.c
    bgp_parse_attr.c
    bgp_parse_common.c
    bgp_parse_ipv4uc.c
    bgp_parse_ipv6uc.c
    bgp_parse_labeled.c
    bgp_parse_vpn.c
    bgp_parse_evpn.c
    bgp_parse_flowspec.c
    bgp_parse_open.c
    bgp_parse_notif.c
)

add_library(bgp_parse SHARED ${BGP_PARSE_SOURCES})

# 包含：当前目录（私有头文件）+ include/（公共头文件）
target_include_directories(bgp_parse
    PRIVATE  .
    PUBLIC   ${CMAKE_SOURCE_DIR}/include
)

# 无外部依赖（纯 C 解析库，仅依赖标准库）

set_target_properties(bgp_parse PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
)

# BGP RIB 内存库（供 bgp / sbmp / 测试复用）
set(BGP_RIB_SOURCES
    ${CMAKE_SOURCE_DIR}/src/bgp/bgp_rib.c
)

add_library(bgp_rib SHARED ${BGP_RIB_SOURCES})

target_include_directories(bgp_rib
    PUBLIC ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(bgp_rib PUBLIC
    ${GLIB_LIBRARIES}
)

set_target_properties(bgp_rib PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
)

install(TARGETS bgp_parse bgp_rib LIBRARY DESTINATION lib)
