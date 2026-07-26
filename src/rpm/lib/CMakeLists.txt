add_library(rpm_api SHARED rpm_api.c)

target_include_directories(rpm_api PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(rpm_api PUBLIC
    dev_api
    utils
    ${GLIB_LIBRARIES}
)

set_target_properties(rpm_api PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
)

install(TARGETS rpm_api LIBRARY DESTINATION lib)
