// ============================================================================
// Real RTX 5090 / CUDA proof (built only when CUDA is enabled, sm_120).
// ============================================================================

#include "numafabric/numafabric.hpp"
#include "numafabric/cuda/cuda_backend.hpp"
#include "test_util.hpp"

#include <cstdio>

using namespace numafabric;

static void run_all() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("  [cuda] checking availability...\n");
    if (!cuda::is_available()) {
        std::printf("  [cuda] no CUDA device available; skipping proof\n");
        return;
    }

    auto devs = cuda::enumerate_cuda_devices();
    CHECK(!devs.empty());
    for (const auto& d : devs) {
        std::printf("  [cuda] device %llu: %s pci=%s cc=%s locality=%s\n",
                    d.id.value(), d.device.name.c_str(), d.device.pci_bdf.c_str(),
                    d.device.compute_capability.c_str(),
                    numafabric::to_string(d.locality.locality).data());
    }

    auto rt = Runtime::create_windows();
    rt->discover();
    const auto node = rt->topology().nodes.front().id;
    const std::uint64_t bytes = 64u * 1024u * 1024u;
    auto region = rt->allocate_on_node(Bytes::from(bytes), node);
    CHECK(region.backend_result.ptr_valid);
    CHECK(region.backend_result.ptr != nullptr);

    auto res = cuda::run_cuda_proof(region.backend_result.ptr, bytes, 3);
    std::printf("  [cuda] proof: %s\n", res.summary().c_str());
    CHECK(res.ok);
    CHECK(res.verified);
    CHECK(res.device_name.find("RTX 5090") != std::string::npos ||
          res.device_name.find("GeForce") != std::string::npos);
    CHECK(res.compute_capability == "12.0");
    CHECK(res.timings.h2d_ns > 0);
    CHECK(res.timings.kernel_ns > 0);
    CHECK(res.timings.d2h_ns > 0);

    rt->release(region.id);
    CHECK(rt->memory_accounting_clean());
}

TEST_MAIN()
