# NUMA Fabric benchmark with reported units.
add_executable(numafabric_bench bench.cpp)
target_link_libraries(numafabric_bench PRIVATE numafabric numafabric_warnings)
