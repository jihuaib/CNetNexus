# BGP API 模块源码
set(BGP_API_SOURCES
    bgp_api.c
)

# 创建 BGP API 共享库
add_library(bgp_api SHARED ${BGP_API_SOURCES})

# 包含目录
target_include_directories(bgp_api PRIVATE
    .
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# 链接依赖
target_link_libraries(bgp_api PUBLIC
    bgp
    dev_api
    ${GLIB_LIBRARIES}
)

# 设置库版本
set_target_properties(bgp_api PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
)
