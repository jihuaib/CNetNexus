# CLI 参数解析库
set(CLI_PARAM_SOURCES
    cli_param_type.c
    cli_payload.c
)

add_library(cli_param SHARED ${CLI_PARAM_SOURCES})

target_include_directories(cli_param PUBLIC
    .
)

target_link_libraries(cli_param PUBLIC
    ${GLIB_LIBRARIES}
)

set_target_properties(cli_param PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)
