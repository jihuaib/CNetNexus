add_subdirectory(utils)
add_subdirectory(dev)
add_subdirectory(db)
add_subdirectory(cli)          # CLI 模块(包含 param/core/engine 子目录)
add_subdirectory(access)
# 删除: add_subdirectory(cfg)        # CFG 模块已合并到 cli/core
# 删除: add_subdirectory(cli_engine) # 已移至 cli/engine
add_subdirectory(bgp)
add_subdirectory(if)
add_subdirectory(route)

# 进程管理器（不再链接模块 .so，仅需 utils 获取路径工具）
add_executable(netnexus
    main.c
)

target_link_libraries(netnexus PRIVATE
    utils
)

# Set RPATH to find libraries in lib/ directory at runtime
set_target_properties(netnexus PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
)

# Installation
install(TARGETS netnexus
    RUNTIME DESTINATION bin
)
