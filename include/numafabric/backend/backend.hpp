#pragma once
// ============================================================================
// NUMA Fabric - vendor-neutral backend boundary.
//
// The public runtime model never depends on a specific OS. A Backend is the
// single integration point for host-locality facts: NUMA discovery, group-aware
// affinity, governed host allocation and accelerator enumeration. Currently two
// backends exist (Windows, Synthetic); a future Linux backend slots in behind
// this same interface without redesigning the public runtime model.
// ============================================================================

#include "numafabric/accelerator/accelerator.hpp"
#include "numafabric/affinity/affinity_set.hpp"
#include "numafabric/core/enums.hpp"
#include "numafabric/core/freshness.hpp"
#include "numafabric/core/ids.hpp"
#include "numafabric/core/provenance.hpp"
#include "numafabric/core/quantities.hpp"
#include "numafabric/topology/topology.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace numafabric {
namespace backend {

enum class BackendKind { Windows, Synthetic };

constexpr std::string_view to_string(BackendKind v) {
    switch (v) {
        case BackendKind::Windows: return "Windows";
        case BackendKind::Synthetic: return "Synthetic";
    }
    return "Unknown";
}
inline std::ostream& operator<<(std::ostream& os, BackendKind v) { return os << to_string(v); }

class BackendError final : public std::exception {
public:
    explicit BackendError(std::string m) : message_(std::move(m)) {}
    const char* what() const noexcept override { return message_.c_str(); }
private:
    std::string message_;
};

// ---------------------------------------------------------------------------
// Allocation spec (request) and result
// ---------------------------------------------------------------------------
struct AllocationSpec {
    Bytes bytes = Bytes::zero();
    Alignment alignment = Alignment::zero();
    PlacementMode mode = PlacementMode::Any;
    NumaNodeId preferred_node = NumaNodeId::invalid();
    NumaNodeId required_node = NumaNodeId::invalid();
    std::vector<NumaNodeId> interleave_nodes;
    AllocationKind kind = AllocationKind::Host;
    bool touch = true;                 // commit/touch pages immediately
    bool persistent = false;           // keep the allocation after call returns
    std::string tag;                   // caller-supplied label

    void validate() const {
        if (bytes.value() == 0) throw BackendError("allocation of zero bytes");
        if (mode == PlacementMode::RequiredNode && !required_node.is_valid())
            throw BackendError("REQUIRED_NODE without a node");
        if (mode == PlacementMode::PreferredNode && !preferred_node.is_valid())
            throw BackendError("PREFERRED_NODE without a node");
    }
};

struct AllocationResult {
    MemoryRegionId region_id = MemoryRegionId::invalid();
    void* ptr = nullptr;
    Bytes granted_bytes = Bytes::zero();
    bool ptr_valid = false;
    bool locality_known = false;        // conclusively observed actual locality
    NumaNodeId observed_node = NumaNodeId::invalid(); // requested/derived unless measured
    AllocationKind kind = AllocationKind::Host;
    bool touched = false;
    PlacementMode requested_mode = PlacementMode::Any;

    [[nodiscard]] bool ok() const noexcept {
        if (granted_bytes.value() == 0) return false;
        return kind == AllocationKind::Synthetic || ptr_valid;
    }
};

// ---------------------------------------------------------------------------
// Backend interface
// ---------------------------------------------------------------------------
class Backend {
public:
    virtual ~Backend() = default;

    virtual BackendKind kind() const noexcept = 0;
    virtual std::string kind_name() const = 0;
    virtual std::string describe() const = 0;

    // Discovery (can be expensive; not called while holding a runtime lock).
    virtual topo::HostTopology discover_host() = 0;

    // Governed allocation with explicit requested-vs-actual semantics.
    virtual AllocationResult allocate(const AllocationSpec& spec) = 0;
    virtual void free(const AllocationResult& alloc) = 0;

    // Conclusive actual-locality probe where the platform can expose it.
    virtual std::optional<NumaNodeId> probe_allocation_node(const AllocationResult& alloc) = 0;

    // Group-aware thread affinity (capture + apply + restore).
    virtual affinity::AffinitySet current_thread_affinity() = 0;
    virtual bool apply_thread_affinity(const affinity::AffinitySet& set) = 0;

    // Accelerator enumeration.
    virtual std::vector<accel::AcceleratorInfo> enumerate_accelerators() = 0;

    // Optional: does this backend provide genuinely measured locality?
    virtual bool provides_measured_locality() const noexcept { return false; }
};

// ---------------------------------------------------------------------------
// Synthetic backend configuration.
//
// Deterministically models multi-socket / multi-NUMA / multi-accelerator
// systems even when the physical validation host exposes only one node. The
// synthetic backend runs the same production decision paths as the hardware
// backends; it is never presented as physical validation.
// ---------------------------------------------------------------------------
struct SyntheticConfig {
    std::uint32_t node_count = 2;
    std::uint32_t processors_per_node = 8;
    std::uint32_t group_capacity = 64;      // processors per processor group
    bool expose_memory = true;

    // Per-node memory capacity (bytes). Empty => uniform default of 16 GiB each.
    std::vector<Capacity> node_capacities;

    // Symmetric inter-node distance/cost matrix (node_count x node_count).
    // Empty => uniform: 0 on diagonal, 100 otherwise.
    std::vector<std::vector<LocalityCost>> distances;

    // Agent-specified placement: which node the synthetic backend's "current
    // thread" is logically owned by (for LOCAL placement).
    std::uint32_t local_thread_node = 0;

    // Accelerators: count and per-accelerator locality class + node.
    struct AcceleratorPlacement { LocalityClass locality; NumaNodeId node; };
    std::vector<AcceleratorPlacement> accelerators;
    std::string name = "synthetic";

    LocalityCost distance(NumaNodeId a, NumaNodeId b) const;
};

// Factory (see synthetic_backend.cpp / windows_backend.cpp for concrete types).
std::unique_ptr<Backend> make_windows_backend();
std::unique_ptr<Backend> make_synthetic_backend(const SyntheticConfig& cfg);

} // namespace backend
} // namespace numafabric
