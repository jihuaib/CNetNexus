add_executable(bgp_parse_tests
    bgp_parse_tests.c
)

target_include_directories(bgp_parse_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(bgp_parse_tests PRIVATE
    bgp_api
)

set_target_properties(bgp_parse_tests PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
)

add_test(NAME bgp_parse_tests COMMAND bgp_parse_tests)

add_executable(bgp_rib_tests
    bgp_rib_tests.c
    ${CMAKE_SOURCE_DIR}/src/bgp/bgp_rib.c
)

target_include_directories(bgp_rib_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/bgp
)

target_link_libraries(bgp_rib_tests PRIVATE
    bgp_api
    utils
)

set_target_properties(bgp_rib_tests PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
)

add_test(NAME bgp_rib_tests COMMAND bgp_rib_tests)
