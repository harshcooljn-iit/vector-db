# The benchmark driver is a separate executable so that benchmarking code never
# ends up linked into the shipped CLI, and so that it can be built with
# different flags when needed.
add_executable(vectordb_bench)
set_target_properties(vectordb_bench PROPERTIES OUTPUT_NAME vectordb-bench)

target_link_libraries(vectordb_bench PRIVATE vectordb vectordb_warnings)
target_include_directories(vectordb_bench PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_sources(vectordb_bench PRIVATE main.cpp)
