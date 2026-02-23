# DB module sources
set(DB_SOURCES
    db_main.c
    db_registry.c
    db_schema.c
    db_api.c
    db_cli.c
    db_serialize.c
    db_ipc_handler.c
)

# Create shared library for DB module
add_library(db SHARED ${DB_SOURCES})

# Include directories
target_include_directories(db PRIVATE
    .
    lib
    ${SQLite3_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(db PRIVATE
    dev_api
    ${GLIB_LIBRARIES}
    ${SQLite3_LIBRARIES}
    Threads::Threads
)

# Set library version
set_target_properties(db PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
)

add_subdirectory(lib)
