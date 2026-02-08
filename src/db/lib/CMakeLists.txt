# DB 客户端库（IPC方式访问DB）
set(DB_CLIENT_SOURCES
    db_client.c
    db_serialize.c
)

# 创建共享库
add_library(db_client SHARED ${DB_CLIENT_SOURCES})

# 包含目录
target_include_directories(db_client PUBLIC
    .
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# 链接依赖
target_link_libraries(db_client PUBLIC
    ipc
    ${GLIB_LIBRARIES}
)

# 设置库版本
set_target_properties(db_client PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
)
