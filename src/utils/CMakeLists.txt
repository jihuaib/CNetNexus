# Utils library - Common utilities
add_library(nn_utils SHARED
    nn_path_utils.c
)

# Link dependencies
target_link_libraries(nn_utils PRIVATE
    ${GLIB_LIBRARIES}
)

# Include directories
target_include_directories(nn_utils PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${GLIB_INCLUDE_DIRS}
)

# Installation
install(TARGETS nn_utils
    LIBRARY DESTINATION lib
)
