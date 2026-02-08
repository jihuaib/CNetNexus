# Telnet接入模块
set(TELNET_ACCESS_SOURCES
    telnet_access.c
)

add_library(telnet_access SHARED ${TELNET_ACCESS_SOURCES})

target_include_directories(telnet_access PUBLIC
    .
    ${CMAKE_SOURCE_DIR}/src/access
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(telnet_access PUBLIC
    access
    cli
    ${GLIB_LIBRARIES}
    Threads::Threads
)

set_target_properties(telnet_access PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)
