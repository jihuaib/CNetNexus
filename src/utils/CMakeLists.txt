# Utils library - Common utilities
add_library(utils SHARED
    path_utils.c
    log.c
    syslog_report.c
    mpls_config.c
    net_addr.c
)

# Link dependencies
target_link_libraries(utils PRIVATE
    ${GLIB_LIBRARIES}
)

# Include directories
target_include_directories(utils PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${GLIB_INCLUDE_DIRS}
)
