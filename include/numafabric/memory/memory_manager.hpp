#pragma once
// ============================================================================
// NUMA Fabric - governed memory manager.
//
// Owns the lifecycle of governed host-memory regions: create -> reserve ->
// allocate -> active -> (migrate/rebind/release) -> released -> invalidated.
// Transitions are guarded; double-free, stale release and generation rollback
// are rejected. Per-node accounting closes exactly to zero after teardown.
// ============================================================================

#include "numafabric/backend/backend.hpp"
#include "numafabric/core/ids.hpp"
#include "numafabric/core/provenance.hpp"
#include "numafabric/memory/memory_region.hpp"

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace numafabric {
namespace memory {

class MemoryManager {
public:
    explicit MemoryManager(backend::Backend& backend) : backend_(backend) {}

    struct AllocateRequest {
        Bytes bytes = Bytes::zero();
        Alignment alignment = Alignment::zero();
        PlacementMode mode = PlacementMode::Any;
        NumaNodeId intended_node = NumaNodeId::invalid(); // what the decision wanted
        NumaNodeId preferred_node = NumaNodeId::invalid();
        NumaNodeId required_node = NumaNodeId::invalid();
        std::vector<NumaNodeId> interleave_nodes;
        AllocationKind kind = AllocationKind::Host;
        bool touch = true;
        ProcessId owning_process = ProcessId::invalid();
        WorkerId associated_worker = WorkerId::invalid();
        AcceleratorId associated_accelerator = AcceleratorId::invalid();
        std::string workload_id;
        std::string tag;
        Provenance provenance = Provenance::unknown();
    };

    // Allocate a governed region. On physical backend failure a Failed region is
    // returned (never a silent success); programmer errors throw.
    MemoryRegion allocate(const AllocateRequest& req);

    // Release a live region. Rejects double-free / stale release (a released or
    // invalidated region cannot be released again).
    void release(MemoryRegionId id, std::optional<MemoryGeneration> expected = std::nullopt);

    // Transition a region to a new lifecycle state (guarded).
    void transition(MemoryRegionId id, LifecycleState to);

    const MemoryRegion* get(MemoryRegionId id) const;
    MemoryRegion* get_mut(MemoryRegionId id);

    // Advance generation / mark locality stale after a topology or generation
    // change. Stale decisions can never become current without revalidation.
    void invalidate_locality_stale();

    // Governed reallocate/copy/rebind for migration. The preserved operation is a
    // reallocate + copy + rebind (the platform does NOT expose portable physical
    // page migration) and this is stated explicitly. A failed copy retains the
    // last valid authoritative allocation.
    struct MigrationView {
        bool committed = false;
        bool copy_succeeded = false;
        NumaNodeId new_node = NumaNodeId::invalid();
        MemoryGeneration generation = MemoryGeneration::invalid();
    };
    MigrationView governed_reallocate(MemoryRegionId id, NumaNodeId target, bool copy_data);

    // Per-node accounting.
    std::uint64_t per_node_allocated(NumaNodeId id) const;
    std::uint64_t total_allocated() const;
    bool accounting_clean() const; // no live regions, no residual accounting

    std::vector<MemoryRegion> all_regions() const;

    std::uint64_t generation_watermark() const { return watermark_.value(); }

private:
    backend::Backend& backend_;
    mutable std::mutex mutex_;
    mutable std::mutex backend_call_mutex_;
    std::map<MemoryRegionId, MemoryRegion> regions_;
    std::map<NumaNodeId, std::uint64_t> node_usage_;
    std::uint64_t next_id_ = 1;
    MemoryGeneration watermark_ = MemoryGeneration::initial();

    void add_node_usage(NumaNodeId node, std::uint64_t bytes);
    void sub_node_usage(NumaNodeId node, std::uint64_t bytes, bool allow_underflow_error);
    NumaNodeId choose_observed_node(const MemoryRegion& r) const;
};

} // namespace memory
} // namespace numafabric
