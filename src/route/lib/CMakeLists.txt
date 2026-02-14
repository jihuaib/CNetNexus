# Route API 模块源码
set(ROUTE_API_SOURCES
    route_api.c
)

# 创建 Route API 共享库
add_library(route_api SHARED ${ROUTE_API_SOURCES})

# 包含目录
target_include_directories(route_api PRIVATE
    .
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# 链接依赖
target_link_libraries(route_api PUBLIC
    route
    ipc
    ${GLIB_LIBRARIES}
)

# 设置库版本
set_target_properties(route_api PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
)
