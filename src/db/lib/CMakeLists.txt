# DB API 模块源码
set(DB_API_SOURCES
    db_api.c
)

# 创建 DB API 共享库
add_library(db_api SHARED ${DB_API_SOURCES})

# 包含目录
target_include_directories(db_api PRIVATE
    .
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# 链接依赖（链接主模块 .so 以获得 db_msg_handler 等符号）
target_link_libraries(db_api PUBLIC
    db
    ipc
    ${GLIB_LIBRARIES}
)

# 设置库版本
set_target_properties(db_api PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
)
