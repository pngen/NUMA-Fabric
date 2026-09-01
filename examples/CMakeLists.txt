# NUMA Fabric runnable examples. Compile as part of normal validation and
# exercise the real public library APIs.

function(nf_add_example name)
    add_executable(ex_${name} ex_${name}.cpp)
    target_link_libraries(ex_${name} PRIVATE numafabric numafabric_warnings)
endfunction()

nf_add_example(discover)
nf_add_example(nodes)
nf_add_example(preferred_alloc)
nf_add_example(bind_worker)
nf_add_example(placement)
nf_add_example(explain)
nf_add_example(reservations)
nf_add_example(synthetic_dual)
nf_add_example(fallback_penalty)
nf_add_example(topology_rollover)
nf_add_example(stale_authority)
nf_add_example(persistence)
nf_add_example(concurrent_placement)

if(NUMAFABRIC_ENABLE_CUDA)
    add_executable(ex_cuda ex_cuda.cpp)
    target_link_libraries(ex_cuda PRIVATE numafabric numafabric_cuda numafabric_warnings)
endif()
