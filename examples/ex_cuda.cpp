// Example 14: accelerator locality + real CUDA proof (built with CUDA).
#include "numafabric/numafabric.hpp"
#include "numafabric/cuda/cuda_backend.hpp"
#include <cstdio>
int main() {
    auto rt = numafabric::Runtime::create_windows();
    rt->discover();
    auto devs = numafabric::cuda::enumerate_cuda_devices();
    rt->register_accelerators(devs);
    std::printf("accelerators=%zu\n", rt->accelerators().size());
    for (const auto& a : rt->accelerators()) {
        std::printf("  %llu %s locality=%s\n", a.id.value(), a.device.name.c_str(),
                    numafabric::to_string(a.locality.locality).data());
    }
    return 0;
}
