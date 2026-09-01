#pragma once
// ============================================================================
// NUMA Fabric - CUDA integration / proof backend (optional, RTX 5090 sm_120).
//
// Discovers real CUDA devices and runs a bounded real H2D / kernel / D2H proof
// over host memory that NUMA Fabric governed. Accelerator-to-NUMA locality is
// reported honestly: the OS here does not expose a conclusive GPU-to-NUMA-node
// mapping, so it is represented as UNKNOWN / DERIVED and never asserted as
// "GPU belongs to node 0" merely because the machine has a single node.
// ============================================================================

#include "numafabric/accelerator/accelerator.hpp"
#include "numafabric/core/quantities.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace numafabric {
namespace cuda {

bool is_available();
std::vector<accel::AcceleratorInfo> enumerate_cuda_devices();

struct ProofTimings {
    std::uint64_t h2d_ns = 0;
    std::uint64_t kernel_ns = 0;
    std::uint64_t d2h_ns = 0;
    std::uint64_t pinned_h2d_ns = 0;
};

struct ProofResult {
    bool ok = false;
    std::uint64_t bytes = 0;
    int iterations = 0;
    bool verified = false;
    std::string device_name;
    std::string pci_bdf;
    std::string compute_capability;
    ProofTimings timings;
    std::string summary() const;
};

// Runs a real bounded CUDA kernel over a caller-provided governed host buffer
// (overwritten with the kernel output: out = 2*in + 1). The caller owns the
// allocation and release.
ProofResult run_cuda_proof(void* host_data, std::uint64_t bytes, int iterations);

} // namespace cuda
} // namespace numafabric
