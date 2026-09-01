#pragma once
// ============================================================================
// NUMA Fabric - worker identities and scoped affinity binding.
//
// A logical worker has a stable WorkerId and a mutable WorkerBootId: a restarted
// worker is a NEW incarnation with a FRESH boot id and never inherits authority
// merely because the WorkerId is unchanged. Worker-specific generation gates are
// scoped to the worker incarnation so a freshly restarted worker is not wrongly
// rejected because a previous incarnation held a higher generation.
// ============================================================================

#include "numafabric/affinity/affinity_set.hpp"
#include "numafabric/backend/backend.hpp"
#include "numafabric/core/ids.hpp"

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace numafabric {
namespace affinity {

class WorkerError final : public std::exception {
public:
    explicit WorkerError(std::string m) : message_(std::move(m)) {}
    const char* what() const noexcept override { return message_.c_str(); }
private:
    std::string message_;
};

struct Worker {
    WorkerId id = WorkerId::invalid();
    WorkerBootId boot_id = WorkerBootId::invalid();   // fresh on every incarnation
    std::uint64_t incarnation = 0;                     // 1-based; increments on restart
    ProcessId process_id = ProcessId::invalid();
    ThreadId thread_id = ThreadId::invalid();
    affinity::AffinitySet requested_affinity;
    affinity::AffinitySet current_affinity;
    bool bound = false;
    NumaNodeId node = NumaNodeId::invalid();
    PlacementId placement_id = PlacementId::invalid();
    PlacementGeneration placement_generation = PlacementGeneration::initial();
    BindingId binding_id = BindingId::invalid();
    BindingGeneration binding_generation = BindingGeneration::initial();
    PolicyId policy_id = PolicyId::invalid();
    PolicyGeneration policy_generation = PolicyGeneration::initial();
    bool alive = true;
    ObservationGeneration last_observation_generation = ObservationGeneration::initial();
    std::uint64_t registered_ms = 0;
};

// ---------------------------------------------------------------------------
// WorkerRegistry - incarnation-aware registration and authority validation.
// ---------------------------------------------------------------------------
class WorkerRegistry {
public:
    // Register a worker (first incarnation). Returns the freshly issued boot id.
    Worker register_worker(WorkerId id, ProcessId pid);

    // Restart the SAME logical worker with a fresh boot id. The previous
    // incarnation is invalidated; worker-specific generations are scoped to the
    // new incarnation (older generation numbers never fence the new worker).
    Worker restart_worker(WorkerId id, ProcessId pid);

    void mark_dead(WorkerId id, WorkerBootId boot);

    // Recover a persisted worker preserving its exact boot id + incarnation.
    // Returns false if a live worker with the same id already exists.
    bool restore(const Worker& w);

    const Worker* find(WorkerId id) const;
    Worker* find_mut(WorkerId id);

    // Validate that a mutable operation's claimed boot id is the current
    // incarnation; otherwise the authority is stale.
    bool validate_boot(WorkerId id, WorkerBootId claimed_boot) const;
    std::uint64_t incarnation_of(WorkerId id) const;

    std::vector<Worker> all() const;
    std::size_t live_count() const;

private:
    mutable std::mutex mutex_;
    std::map<WorkerId, Worker> workers_;
    std::uint64_t next_boot_ = 1;
};

// ---------------------------------------------------------------------------
// ScopedThreadBinding - RAII temporary thread affinity that ALWAYS restores.
// ---------------------------------------------------------------------------
class ScopedThreadBinding {
public:
    ScopedThreadBinding(backend::Backend& backend, const affinity::AffinitySet& target)
        : backend_(backend), original_(backend_.current_thread_affinity()), active_(true) {
        if (!backend_.apply_thread_affinity(target)) {
            active_ = false;
            throw WorkerError("failed to apply scoped thread affinity");
        }
    }

    // Movable, non-copyable.
    ScopedThreadBinding(ScopedThreadBinding&& other) noexcept = default;
    ScopedThreadBinding& operator=(ScopedThreadBinding&& other) = delete;
    ScopedThreadBinding(const ScopedThreadBinding&) = delete;
    ScopedThreadBinding& operator=(const ScopedThreadBinding&) = delete;

    ~ScopedThreadBinding() {
        if (active_) { backend_.apply_thread_affinity(original_); }
    }

    void restore_now() {
        if (active_) { backend_.apply_thread_affinity(original_); active_ = false; }
    }

private:
    backend::Backend& backend_;
    affinity::AffinitySet original_;
    bool active_;
};

} // namespace affinity
} // namespace numafabric
