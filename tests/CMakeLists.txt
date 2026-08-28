find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

# One test binary keeps link time down and makes `ctest` output readable.
# Suites are separated by directory and by gtest suite name, not by executable.
add_executable(vectordb_tests)

target_link_libraries(vectordb_tests PRIVATE
    vectordb
    vectordb_warnings
    GTest::gtest_main   # transitively brings GTest::gtest
)

target_include_directories(vectordb_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)

add_subdirectory(unit)

gtest_discover_tests(vectordb_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES TIMEOUT 300
)
