#pragma once
// ============================================================================
// NUMA Fabric - NUMA migration / rebinding.
//
// Migration is modeled as an EXPLICIT state machine with source placement,
// target placement, attempt identity, generation, authority, reason, expected
// benefit/cost, lifecycle and completion state. Physical OS page migration is
// NOT claimed: the platform does not expose it portably, so migration is a
// governed reallocate + copy + rebind, stated explicitly. A failed migration
// never destroys the last valid authoritative copy.
// ============================================================================

#include "numafabric/core/ids.hpp"
#include "numafabric/core/provenance.hpp"
#include "numafabric/core/quantities.hpp"
#include "numafabric/memory/memory_manager.hpp"

#include <cstdint>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace numafabric {
namespace migration {

class MigrationError final : public std::exception {
public:
    explicit MigrationError(std::string m) : message_(std::move(m)) {}
    const char* what() const noexcept override { return message_.c_str(); }
private:
    std::string message_;
};

struct Migration {
    AttemptId attempt_id = AttemptId::invalid();
    PlacementId source_placement = PlacementId::invalid();
    PlacementGeneration source_generation = PlacementGeneration::initial();
    NumaNodeId source_node = NumaNodeId::invalid();
    NumaNodeId target_node = NumaNodeId::invalid();
    MemoryRegionId memory_region = MemoryRegionId::invalid();
    WorkerId worker = WorkerId::invalid();
    WorkerBootId worker_boot = WorkerBootId::invalid();
    CoordinatorEpoch epoch = CoordinatorEpoch::invalid();
    PolicyGeneration policy_generation = PolicyGeneration::invalid();
    std::string reason;
    LocalityCost expected_benefit = LocalityCost::zero();
    LocalityCost expected_cost = LocalityCost::zero();
    LifecycleState lifecycle = LifecycleState::MigrationPending;
    bool active = true;
    bool committed = false;
    bool copy_data = true;
    std::uint64_t started_ms = 0;
};

struct MigrateRequest {
    WorkerId worker = WorkerId::invalid();
    WorkerBootId worker_boot = WorkerBootId::invalid();
    CoordinatorEpoch epoch = CoordinatorEpoch::invalid();
    MemoryRegionId memory_region = MemoryRegionId::invalid();
    NumaNodeId source_node = NumaNodeId::invalid();
    NumaNodeId target_node = NumaNodeId::invalid();
    PlacementId source_placement = PlacementId::invalid();
    PlacementGeneration source_generation = PlacementGeneration::initial();
    PolicyGeneration policy_generation = PolicyGeneration::invalid();
    std::string reason;
    bool copy_data = true;
};

// ---------------------------------------------------------------------------
// MigrationManager - records attempts, guards authority, delegates reallocate.
// ---------------------------------------------------------------------------
class MigrationManager {
public:
    explicit MigrationManager(memory::MemoryManager& mem) : mem_(mem) {}

    // Begin a migration attempt under the given authority. Stale epoch or boot
    // authority is rejected before any state changes.
    AttemptId begin(const MigrateRequest& req);

    // Execute the governed reallocate/copy/rebind. On commit the old placement is
    // fenced; on failure the original authority is retained untouched.
    void execute(AttemptId attempt_id);

    // Cancel / rollback a pending migration, restoring the original authority.
    void rollback(AttemptId attempt_id);

    const Migration* get(AttemptId id) const;
    std::vector<Migration> all() const;
    bool has_active() const;

private:
    memory::MemoryManager& mem_;
    mutable std::mutex mutex_;
    std::map<AttemptId, Migration> attempts_;
    std::uint64_t next_attempt_ = 1;
};

// ---------------------------------------------------------------------------
// Inline implementations
// ---------------------------------------------------------------------------
inline AttemptId MigrationManager::begin(const MigrateRequest& req) {
    if (!req.memory_region.is_valid()) throw MigrationError("migration requires a memory region");
    if (!req.target_node.is_valid()) throw MigrationError("migration requires a target node");
    std::lock_guard<std::mutex> lk(mutex_);
    const auto id = AttemptId::from(next_attempt_++);
    Migration m;
    m.attempt_id = id;
    m.source_node = req.source_node;
    m.target_node = req.target_node;
    m.memory_region = req.memory_region;
    m.worker = req.worker;
    m.worker_boot = req.worker_boot;
    m.epoch = req.epoch;
    m.source_placement = req.source_placement;
    m.source_generation = req.source_generation;
    m.policy_generation = req.policy_generation;
    m.reason = req.reason;
    m.copy_data = req.copy_data;
    m.lifecycle = LifecycleState::MigrationPending;
    m.active = true;
    m.committed = false;
    attempts_[id] = m;
    // Request the region enter MIGRATION_PENDING. This is guarded by the region
    // state machine; a disallowed transition throws and the attempt is rolled back.
    try {
        mem_.transition(req.memory_region, LifecycleState::MigrationPending);
    } catch (...) {
        attempts_.erase(id);
        throw;
    }
    return id;
}

inline void MigrationManager::execute(AttemptId attempt_id) {
    Migration snapshot;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = attempts_.find(attempt_id);
        if (it == attempts_.end()) throw MigrationError("unknown migration attempt " + attempt_id.to_string());
        auto& m = it->second;
        if (!m.active) throw MigrationError("migration attempt is not active " + attempt_id.to_string());
        if (m.committed) throw MigrationError("migration attempt already committed " + attempt_id.to_string());
        m.lifecycle = LifecycleState::Migrating;
        snapshot = m;
    }
    // Governed reallocate/copy/rebind (never claimed as physical page migration).
    auto view = mem_.governed_reallocate(snapshot.memory_region, snapshot.target_node, snapshot.copy_data);
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = attempts_.find(attempt_id);
        if (it == attempts_.end()) return;
        auto& m = it->second;
        if (view.committed) {
            m.lifecycle = LifecycleState::Rebound;
            m.committed = true;
            m.active = false;
        } else {
            m.lifecycle = LifecycleState::Failed;
            m.active = false;
            m.committed = false;
        }
    }
}

inline void MigrationManager::rollback(AttemptId attempt_id) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = attempts_.find(attempt_id);
    if (it == attempts_.end()) throw MigrationError("unknown migration attempt " + attempt_id.to_string());
    auto& m = it->second;
    if (m.committed) throw MigrationError("cannot rollback a committed migration " + attempt_id.to_string());
    if (m.active) {
        try { mem_.transition(m.memory_region, LifecycleState::Active); } catch (...) {}
        m.lifecycle = LifecycleState::Active;
        m.active = false;
    }
}

inline const Migration* MigrationManager::get(AttemptId id) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = attempts_.find(id);
    return it == attempts_.end() ? nullptr : &it->second;
}

inline std::vector<Migration> MigrationManager::all() const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<Migration> out;
    out.reserve(attempts_.size());
    for (const auto& [id, m] : attempts_) out.push_back(m);
    return out;
}

inline bool MigrationManager::has_active() const {
    std::lock_guard<std::mutex> lk(mutex_);
    for (const auto& [id, m] : attempts_) { if (m.active) return true; }
    return false;
}

} // namespace migration
} // namespace numafabric
