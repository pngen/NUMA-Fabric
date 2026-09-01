// ============================================================================
// CUDA integration / proof backend (RTX 5090, sm_120) - CUDA source.
// Compiled by nvcc only when NUMAFABRIC_ENABLE_CUDA is ON. Host code is
// warning-clean under the /W4 /WX host compiler; device code is a simple,
// bounded saxpy that never overflows.
// ============================================================================

#include "numafabric/cuda/cuda_backend.hpp"

#include <cuda_runtime.h>
#include <chrono>
#include <cstring>
#include <sstream>

namespace numafabric {
namespace cuda {

__global__ void saxpy_kernel(float* data, int n) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) data[i] = 2.0f * data[i] + 1.0f;
}

static std::uint64_t now_ns() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

static void fill_pattern(float* data, std::uint64_t count) {
    for (std::uint64_t i = 0; i < count; ++i) data[i] = static_cast<float>(i % 1000);
}

bool is_available() {
    int count = 0;
    return cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

std::vector<accel::AcceleratorInfo> enumerate_cuda_devices() {
    std::vector<accel::AcceleratorInfo> out;
    int count = 0;
    if (cudaGetDeviceCount(&count) != cudaSuccess || count <= 0) return out;
    for (int d = 0; d < count; ++d) {
        cudaDeviceProp prop{};
        if (cudaGetDeviceProperties(&prop, d) != cudaSuccess) continue;
        char bus[64] = {0};
        cudaDeviceGetPCIBusId(bus, sizeof(bus), d);
        accel::AcceleratorInfo info;
        info.id = AcceleratorId::from(static_cast<std::uint64_t>(d) + 1);
        info.generation = AcceleratorGeneration::initial();
        info.enumerated = true;
        info.device.vendor = "NVIDIA";
        info.device.name = prop.name;
        info.device.pci_bdf = bus;
        info.device.driver_api_ordinal = d;
        info.device.compute_capability = std::to_string(prop.major) + "." + std::to_string(prop.minor);
        info.locality.locality = LocalityClass::Unknown;
        info.locality.node_is_certain = false;
        info.locality.provenance = Provenance::derived("no OS GPU->NUMA mapping exposed");
        info.provenance = Provenance::measured("CUDA device enumeration");
        out.push_back(std::move(info));
    }
    return out;
}

std::string ProofResult::summary() const {
    std::ostringstream os;
    os << (ok ? "ok" : "FAILED") << " device=" << device_name
       << " pci=" << pci_bdf << " cc=" << compute_capability
       << " bytes=" << bytes << " iters=" << iterations
       << " verified=" << (verified ? "true" : "false")
       << " h2d_ns=" << timings.h2d_ns << " kernel_ns=" << timings.kernel_ns
       << " d2h_ns=" << timings.d2h_ns << " pinned_h2d_ns=" << timings.pinned_h2d_ns;
    return os.str();
}

ProofResult run_cuda_proof(void* host_data, std::uint64_t bytes, int iterations) {
    ProofResult r;
    r.bytes = bytes;
    r.iterations = iterations;
    const int n = static_cast<int>(bytes / sizeof(float));
    if (n <= 0) { r.ok = false; return r; }

    cudaDeviceProp prop{};
    if (cudaGetDeviceProperties(&prop, 0) != cudaSuccess) { r.ok = false; return r; }
    r.device_name = prop.name;
    r.compute_capability = std::to_string(prop.major) + "." + std::to_string(prop.minor);
    char bus[64] = {0};
    cudaDeviceGetPCIBusId(bus, sizeof(bus), 0);
    r.pci_bdf = bus;

    auto* host = static_cast<float*>(host_data);
    fill_pattern(host, static_cast<std::uint64_t>(n));

    float* dev = nullptr;
    if (cudaMalloc(&dev, static_cast<std::size_t>(bytes)) != cudaSuccess) { r.ok = false; return r; }

    for (int iter = 0; iter < iterations; ++iter) {
        fill_pattern(host, static_cast<std::uint64_t>(n)); // reset input each iteration
        const auto t0 = now_ns();
        cudaMemcpy(dev, host, static_cast<std::size_t>(bytes), cudaMemcpyHostToDevice);
        const auto t1 = now_ns();
        const int threads = 256;
        const int blocks = (n + threads - 1) / threads;
        saxpy_kernel<<<blocks, threads>>>(dev, n);
        cudaDeviceSynchronize();
        const auto t2 = now_ns();
        cudaMemcpy(host, dev, static_cast<std::size_t>(bytes), cudaMemcpyDeviceToHost);
        const auto t3 = now_ns();
        r.timings.h2d_ns = t1 - t0;
        r.timings.kernel_ns = t2 - t1;
        r.timings.d2h_ns = t3 - t2;
    }

    bool ok = true;
    for (int i = 0; i < n; ++i) {
        const float expected = 2.0f * static_cast<float>(i % 1000) + 1.0f;
        if (host[i] != expected) { ok = false; break; }
    }
    r.verified = ok;
    r.ok = ok;

    void* pinned = nullptr;
    if (cudaMallocHost(&pinned, static_cast<std::size_t>(bytes)) == cudaSuccess) {
        fill_pattern(static_cast<float*>(pinned), static_cast<std::uint64_t>(n));
        const auto p0 = now_ns();
        cudaMemcpy(dev, pinned, static_cast<std::size_t>(bytes), cudaMemcpyHostToDevice);
        cudaDeviceSynchronize();
        const auto p1 = now_ns();
        r.timings.pinned_h2d_ns = p1 - p0;
        cudaFreeHost(pinned);
    }

    cudaFree(dev);
    if (cudaGetLastError() != cudaSuccess) r.ok = false;
    return r;
}

} // namespace cuda
} // namespace numafabric
