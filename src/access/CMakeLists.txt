# 接入模块库
set(ACCESS_SOURCES
    access_module.c
)

add_library(access SHARED ${ACCESS_SOURCES})

target_include_directories(access PUBLIC
    .
    ${CMAKE_SOURCE_DIR}/src/cli_engine
)

target_link_libraries(access PUBLIC
    ${GLIB_LIBRARIES}
)

set_target_properties(access PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

# Telnet接入模块子目录
add_subdirectory(telnet)

# Access 独立进程
set(ACCESS_PROCESS_SOURCES
    access_process.c
    access_main.c
)

add_executable(access_daemon ${ACCESS_PROCESS_SOURCES})
set_target_properties(access_daemon PROPERTIES OUTPUT_NAME access)

target_include_directories(access_daemon PRIVATE
    .
    ${CMAKE_SOURCE_DIR}/src/cli_engine
)

target_link_libraries(access_daemon PRIVATE
    access
    telnet_access
    ipc
    cli
    ${GLIB_LIBRARIES}
    Threads::Threads
)

set_target_properties(access_daemon PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
