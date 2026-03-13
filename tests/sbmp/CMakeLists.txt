add_executable(sbmp_bmp_tests
    bmp_parse_tests.c
    ${CMAKE_SOURCE_DIR}/src/sbmp/sbmp_bmp.c
)

target_include_directories(sbmp_bmp_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/sbmp
)

add_test(NAME sbmp_bmp_tests COMMAND sbmp_bmp_tests)
