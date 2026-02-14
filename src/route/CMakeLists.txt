# Route 独立进程
set(ROUTE_SOURCES
        route_main.c
    route_cli.c
    route_api.c
)

add_library(route ${ROUTE_SOURCES})

target_include_directories(route PRIVATE
    .
)

# Link libraries
target_link_libraries(route PRIVATE
        ipc
        )

# Set library version
set_target_properties(route PROPERTIES
        VERSION 1.0.0
        SOVERSION 1
        )
