# CFG API 模块源码
set(CFG_API_SOURCES
    cfg_api.c
)

# 创建 CFG API 共享库
add_library(cfg_api SHARED ${CFG_API_SOURCES})

# 包含目录
target_include_directories(cfg_api PRIVATE
    .
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# 链接依赖
target_link_libraries(cfg_api PUBLIC
    cfg
    dev_api
    ${GLIB_LIBRARIES}
)

# 设置库版本
set_target_properties(cfg_api PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
)
