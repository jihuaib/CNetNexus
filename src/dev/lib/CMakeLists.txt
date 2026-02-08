# IPC 模块源码
set(IPC_SOURCES
    ipc_context.c
    ipc_frame.c
    ipc_connection.c
    ipc_query.c
    ipc_config.c
    ipc_message.c
)

# 创建 IPC 共享库
add_library(ipc SHARED ${IPC_SOURCES})

# 包含目录
target_include_directories(ipc PRIVATE
    .
)

# 链接依赖
target_link_libraries(ipc PUBLIC
    utils
    ${GLIB_LIBRARIES}
    Threads::Threads
)

# 设置库版本
set_target_properties(ipc PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
)
