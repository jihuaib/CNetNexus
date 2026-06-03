# CLI API 模块源码
set(CLI_API_SOURCES
    cli_tlv.c
    cli_stream.c
    cli_cfg_anchor.c
    cli_progress.c
)

# 创建 CLI API 共享库
add_library(cli_api SHARED ${CLI_API_SOURCES})

# 包含目录
target_include_directories(cli_api PRIVATE
    .
)

# 链接依赖
target_link_libraries(cli_api PUBLIC
    dev_api
    ${GLIB_LIBRARIES}
)

# 设置库版本
set_target_properties(cli_api PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
)
