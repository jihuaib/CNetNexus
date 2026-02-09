# DB module sources
set(NN_DB_SOURCES
    nn_db_main.c
    nn_db_registry.c
    nn_db_schema.c
    nn_db_api.c
    nn_db_cli.c
)

# Create shared library for DB module
add_library(nn_db SHARED ${NN_DB_SOURCES})

# Include directories
target_include_directories(nn_db PRIVATE
    .
    ${SQLite3_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(nn_db PRIVATE
    ${GLIB_LIBRARIES}
    ${SQLite3_LIBRARIES}
    Threads::Threads
)

# Set library version
set_target_properties(nn_db PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
)