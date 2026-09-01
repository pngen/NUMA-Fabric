// ============================================================================
// Worker registry implementation.
// ============================================================================

#include "numafabric/affinity/worker.hpp"

#include <algorithm>

namespace numafabric {
namespace affinity {

Worker WorkerRegistry::register_worker(WorkerId id, ProcessId pid) {
    if (!id.is_valid()) throw WorkerError("register_worker requires a valid worker id");
    std::lock_guard<std::mutex> lk(mutex_);
    if (workers_.count(id)) throw WorkerError("worker already registered: " + id.to_string());

    Worker w;
    w.id = id;
    w.boot_id = WorkerBootId::from(next_boot_++);
    w.incarnation = 1;
    w.process_id = pid;
    w.alive = true;
    w.bound = false;
    w.placement_generation = PlacementGeneration::initial();
    w.binding_generation = BindingGeneration::initial();
    w.policy_generation = PolicyGeneration::initial();
    w.last_observation_generation = ObservationGeneration::initial();
    workers_[id] = w;
    return w;
}

Worker WorkerRegistry::restart_worker(WorkerId id, ProcessId pid) {
    if (!id.is_valid()) throw WorkerError("restart_worker requires a valid worker id");
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = workers_.find(id);
    if (it == workers_.end())
        throw WorkerError("cannot restart unknown worker " + id.to_string());

    // A restarted worker is a NEW incarnation: fresh boot id, and worker-specific
    // generation gates reset for the new incarnation. The old incarnation's higher
    // generations must NOT fence the fresh incarnation.
    Worker& w = it->second;
    w.boot_id = WorkerBootId::from(next_boot_++);
    ++w.incarnation;
    w.process_id = pid;
    w.alive = true;
    w.bound = false;
    w.requested_affinity = affinity::AffinitySet{};
    w.current_affinity = affinity::AffinitySet{};
    w.node = NumaNodeId::invalid();
    w.placement_id = PlacementId::invalid();
    w.placement_generation = PlacementGeneration::initial();
    w.binding_id = BindingId::invalid();
    w.binding_generation = BindingGeneration::initial();
    w.policy_id = PolicyId::invalid();
    w.policy_generation = PolicyGeneration::initial();
    w.last_observation_generation = ObservationGeneration::initial();
    return w;
}

bool WorkerRegistry::restore(const Worker& w) {
    if (!w.id.is_valid() || !w.boot_id.is_valid()) throw WorkerError("cannot restore invalid worker identity");
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = workers_.find(w.id);
    if (it != workers_.end() && it->second.alive) return false;
    if (w.boot_id.value() >= next_boot_) next_boot_ = w.boot_id.value() + 1;
    workers_[w.id] = w;
    return true;
}

void WorkerRegistry::mark_dead(WorkerId id, WorkerBootId boot) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = workers_.find(id);
    if (it == workers_.end()) return;
    if (it->second.boot_id == boot) it->second.alive = false;
}

const Worker* WorkerRegistry::find(WorkerId id) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = workers_.find(id);
    return it == workers_.end() ? nullptr : &it->second;
}

Worker* WorkerRegistry::find_mut(WorkerId id) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = workers_.find(id);
    return it == workers_.end() ? nullptr : &it->second;
}

bool WorkerRegistry::validate_boot(WorkerId id, WorkerBootId claimed_boot) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = workers_.find(id);
    if (it == workers_.end()) return false;
    return it->second.alive && it->second.boot_id == claimed_boot;
}

std::uint64_t WorkerRegistry::incarnation_of(WorkerId id) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = workers_.find(id);
    return it == workers_.end() ? 0 : it->second.incarnation;
}

std::vector<Worker> WorkerRegistry::all() const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<Worker> out;
    out.reserve(workers_.size());
    for (const auto& [id, w] : workers_) out.push_back(w);
    return out;
}

std::size_t WorkerRegistry::live_count() const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::size_t n = 0;
    for (const auto& [id, w] : workers_) { if (w.alive) ++n; }
    return n;
}

} // namespace affinity
} // namespace numafabric
