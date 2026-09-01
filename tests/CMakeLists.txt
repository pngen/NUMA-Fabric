# NUMA Fabric tests. Each is an ordinary executable returning 0 / non-zero.
# No CTest TIMEOUT is set: a genuine hang is a defect to diagnose, never
# converted into a timer-based failure.

function(nf_add_test name)
    add_executable(test_${name} ${ARGN})
    target_link_libraries(test_${name} PRIVATE numafabric numafabric_warnings)
    add_test(NAME ${name} COMMAND test_${name})
endfunction()

nf_add_test(core          test_core.cpp)
nf_add_test(runtime       test_runtime.cpp)
nf_add_test(protocol      test_protocol.cpp)
nf_add_test(synthetic     test_synthetic.cpp)
nf_add_test(property      test_property.cpp)
nf_add_test(concurrency   test_concurrency.cpp)

if(NUMAFABRIC_ENABLE_CUDA)
    add_executable(test_cuda test_cuda.cpp)
    target_link_libraries(test_cuda PRIVATE numafabric numafabric_cuda numafabric_warnings)
    add_test(NAME cuda COMMAND test_cuda)
endif()

if(NUMAFABRIC_BUILD_DISTRIBUTED)
    add_test(NAME multiprocess_proof COMMAND nf-proof)
    set_tests_properties(multiprocess_proof PROPERTIES WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()
