#pragma once
// ============================================================================
// NUMA Fabric - accelerator & device locality model.
//
// NUMA Fabric does not pretend to own accelerator topology. It represents the
// accelerator-to-host locality RELATIONSHIP (SAME_NUMA_NODE, SAME_SOCKET,
// SAME_HOST_REMOTE_NUMA, UNKNOWN, SYNTHETIC) using real CUDA / PCI / OS device
// evidence where available, and explicitly distinguishes DERIVED or UNKNOWN
// mapping from MEASURED mapping. It never asserts "GPU belongs to node 0"
// solely because the machine has one visible node without recording how that
// conclusion was derived.
// ============================================================================

#include "numafabric/core/enums.hpp"
#include "numafabric/core/ids.hpp"
#include "numafabric/core/provenance.hpp"
#include "numafabric/core/quantities.hpp"

#include <ostream>
#include <string>
#include <vector>

namespace numafabric {
namespace accel {

struct DeviceDescriptor {
    DeviceId device_id = DeviceId::invalid();
    DeviceGeneration device_generation = DeviceGeneration::initial();
    std::string vendor;             // e.g. "NVIDIA"
    std::string name;               // e.g. "NVIDIA GeForce RTX 5090"
    std::string pci_bdf;            // e.g. "0000:01:00.0"
    std::int32_t driver_api_ordinal = -1; // CUDA device ordinal -1 => unknown/not CUDA
    std::string compute_capability; // e.g. "12.0" (sm_120)
};

struct AcceleratorLocality {
    AcceleratorId accelerator = AcceleratorId::invalid();
    LocalityClass locality = LocalityClass::Unknown;
    NumaNodeId local_node = NumaNodeId::invalid(); // only set when the relation is conclusive
    bool node_is_certain = false;
    Nanoseconds derived_from = Nanoseconds::zero();
    Provenance provenance = Provenance::unknown();

    bool operator==(const AcceleratorLocality& o) const {
        return accelerator == o.accelerator && locality == o.locality &&
               local_node == o.local_node && node_is_certain == o.node_is_certain;
    }
};

struct AcceleratorInfo {
    AcceleratorId id = AcceleratorId::invalid();
    AcceleratorGeneration generation = AcceleratorGeneration::initial();
    DeviceDescriptor device;
    AcceleratorLocality locality;
    bool enumerated = false; // whether the accelerator was found on real hardware
    Provenance provenance = Provenance::unknown();

    [[nodiscard]] bool is_cuda() const noexcept { return device.driver_api_ordinal >= 0; }
};

} // namespace accel
} // namespace numafabric
