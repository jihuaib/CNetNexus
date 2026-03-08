# Route 独立进程
set(ROUTE_SOURCES
        route_main.c
    route_cli.c
)

add_library(route ${ROUTE_SOURCES})

target_include_directories(route PRIVATE
    .
)

# Link libraries
target_link_libraries(route PRIVATE
        dev_api
        cli_api
        db_api
        )

# Set library version
set_target_properties(route PROPERTIES
        VERSION 1.0.0
        SOVERSION 1
        )

add_subdirectory(lib)
