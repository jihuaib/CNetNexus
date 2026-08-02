add_library(srv6_api SHARED srv6_api.c)

target_include_directories(srv6_api PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(srv6_api PUBLIC
    dev_api
    utils
    ${GLIB_LIBRARIES}
)

set_target_properties(srv6_api PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
)

install(TARGETS srv6_api LIBRARY DESTINATION lib)
