add_library(fib_api SHARED
    fib_api.c
)

target_link_libraries(fib_api PUBLIC
    dev_api
)

target_include_directories(fib_api PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

set_target_properties(fib_api PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
)

install(TARGETS fib_api LIBRARY DESTINATION lib)
