#pragma once
// ============================================================================
// NUMA Fabric - governed memory region.
//
// Every governed region records what was REQUESTED, what was GRANTED, which
// node was PREFERRED/REQUIRED, what locality was actually OBSERVED or DERIVED,
// its provenance, its lifecycle state, and whether it is still current.
// Placement is never claimed merely because an API accepted a preferred-node
// hint: requested vs. actual locality are distinct fields with distinct truth
// values.
// ============================================================================

#include "numafabric/backend/backend.hpp"
#include "numafabric/core/enums.hpp"
#include "numafabric/core/ids.hpp"
#include "numafabric/core/provenance.hpp"
#include "numafabric/core/quantities.hpp"

#include <cstdint>
#include <ostream>
#include <string>

namespace numafabric {
namespace memory {

class InvalidTransitionError final : public std::exception {
public:
    explicit InvalidTransitionError(std::string m) : message_(std::move(m)) {}
    const char* what() const noexcept override { return message_.c_str(); }
private:
    std::string message_;
};

struct MemoryRegion {
    MemoryRegionId id = MemoryRegionId::invalid();
    MemoryGeneration generation = MemoryGeneration::invalid();
    AllocationKind kind = AllocationKind::Host;
    PlacementMode mode = PlacementMode::Any;
    Bytes requested_bytes = Bytes::zero();
    Bytes granted_bytes = Bytes::zero();
    Alignment alignment = Alignment::zero();
    NumaNodeId preferred_node = NumaNodeId::invalid();
    NumaNodeId required_node = NumaNodeId::invalid();
    NumaNodeId intended_node = NumaNodeId::invalid();   // what the decision wanted
    NumaNodeId actual_node = NumaNodeId::invalid();     // observed/derived/measured
    bool locality_known = false;
    bool locality_verified = false;   // pages were conclusively shown to live on actual_node
    bool locality_current = false;    // still current under the current topology/generation
    ProcessId owning_process = ProcessId::invalid();
    WorkerId associated_worker = WorkerId::invalid();
    AcceleratorId associated_accelerator = AcceleratorId::invalid();
    std::string workload_id;
    std::string tag;
    Provenance provenance = Provenance::unknown();
    LifecycleState state = LifecycleState::Created;
    bool touched = false;             // pages committed / written
    std::uint64_t created_ms = 0;
    std::uint64_t updated_ms = 0;
    backend::AllocationResult backend_result;

    [[nodiscard]] bool is_live() const noexcept {
        return state == LifecycleState::Allocated || state == LifecycleState::Active ||
               state == LifecycleState::MigrationPending || state == LifecycleState::Migrating ||
               state == LifecycleState::Rebound;
    }
    [[nodiscard]] bool is_released() const noexcept {
        return state == LifecycleState::Released || state == LifecycleState::Invalidated ||
               state == LifecycleState::Failed;
    }
};

// Guarded deterministic transition set. Every transition is explicit; a
// disallowed transition throws InvalidTransitionError. Double release, stale
// release, stale migration, duplicate reservation and generation rollback are
// all rejected by construction.
inline bool can_transition(LifecycleState from, LifecycleState to) {
    switch (from) {
        case LifecycleState::Created:
            return to == LifecycleState::Reserved || to == LifecycleState::Invalidated;
        case LifecycleState::Reserved:
            return to == LifecycleState::Allocated || to == LifecycleState::Released ||
                   to == LifecycleState::Invalidated;
        case LifecycleState::Allocated:
            return to == LifecycleState::Active || to == LifecycleState::Failed ||
                   to == LifecycleState::Invalidated;
        case LifecycleState::Active:
            return to == LifecycleState::MigrationPending || to == LifecycleState::ReleasePending ||
                   to == LifecycleState::Invalidated;
        case LifecycleState::MigrationPending:
            return to == LifecycleState::Migrating || to == LifecycleState::Active ||
                   to == LifecycleState::Failed || to == LifecycleState::Invalidated;
        case LifecycleState::Migrating:
            return to == LifecycleState::Rebound || to == LifecycleState::Failed ||
                   to == LifecycleState::Invalidated;
        case LifecycleState::Rebound:
            return to == LifecycleState::Active || to == LifecycleState::ReleasePending ||
                   to == LifecycleState::Invalidated;
        case LifecycleState::ReleasePending:
            return to == LifecycleState::Released || to == LifecycleState::Failed;
        case LifecycleState::Released:
            return to == LifecycleState::Invalidated;
        case LifecycleState::Failed:
            return to == LifecycleState::Invalidated;
        case LifecycleState::Invalidated:
            return false; // terminal
    }
    return false;
}

inline const char* lifecycle_name(LifecycleState s) { return to_string(s).data(); }

} // namespace memory
} // namespace numafabric
