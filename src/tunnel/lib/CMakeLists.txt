add_library(tunnel_api SHARED tunnel_api.c)

target_link_libraries(tunnel_api PUBLIC
    dev_api
    utils
)

target_include_directories(tunnel_api PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

set_target_properties(tunnel_api PROPERTIES
    OUTPUT_NAME tunnel_api
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
)

install(TARGETS tunnel_api LIBRARY DESTINATION lib)
