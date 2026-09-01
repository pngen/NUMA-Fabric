#pragma once
// ============================================================================
// NUMA Fabric - composed runtime facade.
//
// This is the composition root: it owns one backend and wires together the
// topology, memory, affinity, placement, reservation, migration and
// accelerator concerns. The logic lives in the individual managers (no giant
// god object); the Runtime exposes the high-level operations the CLI,
// coordinator, workers, examples and tests drive. Vendor-neutral: it talks to
// the machine only through the Backend interface.
// ============================================================================

#include "numafabric/accelerator/accelerator.hpp"
#include "numafabric/affinity/worker.hpp"
#include "numafabric/backend/backend.hpp"
#include "numafabric/core/enums.hpp"
#include "numafabric/core/ids.hpp"
#include "numafabric/core/provenance.hpp"
#include "numafabric/core/quantities.hpp"
#include "numafabric/memory/memory_manager.hpp"
#include "numafabric/migration/migration.hpp"
#include "numafabric/placement/placement_engine.hpp"
#include "numafabric/reservation/reservation_store.hpp"
#include "numafabric/topology/topology.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace numafabric {

class RuntimeError final : public std::exception {
public:
    explicit RuntimeError(std::string m) : message_(std::move(m)) {}
    const char* what() const noexcept override { return message_.c_str(); }
private:
    std::string message_;
};

// Optional behaviour modifiers for executing a placement.
struct ExecuteOptions {
    bool allocate_memory = false;  // also allocate the memory footprint at the node
    bool bind_worker = false;      // also bind the worker (thread affinity)
    bool verify_memory = false;    // write/verify a pattern (real allocation proof)
    Bytes memory_footprint = Bytes::zero();
    MemoryRegionId existing_region = MemoryRegionId::invalid();
};

// Thread-affinity apply + restore helper used by the runtime's in-process path.
class ScopedAffinityApply {
public:
    ScopedAffinityApply(backend::Backend& b, const affinity::AffinitySet& set);
    ~ScopedAffinityApply();
    ScopedAffinityApply(const ScopedAffinityApply&) = delete;
    ScopedAffinityApply& operator=(const ScopedAffinityApply&) = delete;

private:
    backend::Backend& backend_;
    affinity::AffinitySet original_;
    bool applied_ = false;
};

class Runtime {
public:
    explicit Runtime(std::unique_ptr<backend::Backend> backend);
    static std::unique_ptr<Runtime> create_windows();
    static std::unique_ptr<Runtime> create_synthetic(const backend::SyntheticConfig& cfg);
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    // ---- discovery --------------------------------------------------------
    topo::HostTopology discover();
    topo::TopologySnapshot snapshot() const;
    const topo::HostTopology& topology() const { return topo_; }
    std::uint64_t host_generation() const { return topo_.generation.value(); }

    // ---- accelerators -----------------------------------------------------
    std::vector<accel::AcceleratorInfo> refresh_accelerators();
    void register_accelerators(std::vector<accel::AcceleratorInfo> accelerators);
    const std::vector<accel::AcceleratorInfo>& accelerators() const { return accelerators_; }
    const accel::AcceleratorLocality* accelerator_locality(AcceleratorId id) const;

    // ---- memory allocation -----------------------------------------------
    memory::MemoryRegion allocate(memory::MemoryManager::AllocateRequest req);
    void release(MemoryRegionId id);
    memory::MemoryRegion allocate_on_node(Bytes bytes, NumaNodeId node, AllocationKind kind = AllocationKind::Host);
    std::uint64_t allocated_bytes() const;
    std::uint64_t node_allocated(NumaNodeId id) const;

    // ---- placement --------------------------------------------------------
    placement::PlacementDecision plan(const placement::PlacementRequest& req) const;
    // Execute a placement: optionally allocate memory at the selected node and
    // bind/migrate the worker.
    std::string execute(const placement::PlacementDecision& decision,
                        const ExecuteOptions& opts = ExecuteOptions{});

    // ---- reservations -----------------------------------------------------
    reservation::Reservation reserve(reservation::ReserveRequest req);
    void release_reservation(ReservationId id);
    std::uint64_t reserved_bytes() const;
    bool reservations_clean() const;
    std::uint64_t node_reserved(NumaNodeId id) const;

    // ---- workers & binding -----------------------------------------------
    affinity::Worker register_worker(WorkerId id, ProcessId pid);
    affinity::Worker restart_worker(WorkerId id, ProcessId pid);
    void mark_worker_dead(WorkerId id, WorkerBootId boot);
    const affinity::Worker* worker(WorkerId id) const;
    std::vector<affinity::Worker> workers() const;

    struct BindResult {
        BindingId binding_id = BindingId::invalid();
        BindingGeneration generation = BindingGeneration::initial();
        bool applied = false;
    };
    BindResult bind_worker(WorkerId id, WorkerBootId boot, NumaNodeId node, bool apply_affinity);

    // ---- observations -----------------------------------------------------
    void observe(WorkerId id, WorkerBootId boot, NumaNodeId node, ProvenanceSource source, bool stale);

    // ---- migration / rebinding -------------------------------------------
    // The actual reallocate+copy+rebind is performed by MemoryManager; the
    // migration attempt (authority + reason + lifecycle) is tracked by the
    // migration manager.
    struct Auth { CoordinatorEpoch epoch; WorkerBootId boot; };
    bool migrate(MemoryRegionId region, NumaNodeId target, WorkerId worker,
                 const Auth& auth, std::string reason);

    // ---- authority --------------------------------------------------------
    void advance_epoch();
    CoordinatorEpoch epoch() const { return epoch_; }
    std::uint64_t policy_generation() const { return policy_generation_; }
    void advance_policy_generation();

    // ---- observability ----------------------------------------------------
    std::string summary_text() const;
    std::string summary_json() const;
    std::string explanation_text(const placement::PlacementDecision& d) const;

    // ---- persistence ------------------------------------------------------
    void persist(const std::string& path);
    void recover(const std::string& path);

    // ---- accounting -------------------------------------------------------
    bool memory_accounting_clean() const;
    bool fully_clean() const;

    // ---- access -----------------------------------------------------------
    memory::MemoryManager& memory_manager() { return memory_; }
    reservation::ReservationStore& reservations() { return reservations_; }
    affinity::WorkerRegistry& workers_registry() { return registry_; }

private:
    std::unique_ptr<backend::Backend> backend_;
    topo::HostTopology topo_;
    std::vector<accel::AcceleratorInfo> accelerators_;
    memory::MemoryManager memory_;
    affinity::WorkerRegistry registry_;
    reservation::ReservationStore reservations_;
    placement::PlacementEngine engine_;
    migration::MigrationManager migrator_;
    CoordinatorEpoch epoch_ = CoordinatorEpoch::from(1);
    std::uint64_t policy_generation_ = 1;
    std::uint64_t next_placement_ = 1;
    std::uint64_t next_observation_ = 1;
    mutable std::mutex mutex_;

    std::function<Capacity(NumaNodeId)> node_capacity_provider() const;
    placement::PlacementModel build_placement_model() const;
};

} // namespace numafabric
