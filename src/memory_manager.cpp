// ============================================================================
// Governed memory manager implementation.
// ============================================================================

#include "numafabric/memory/memory_manager.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>

namespace numafabric {
namespace memory {

void MemoryManager::add_node_usage(NumaNodeId node, std::uint64_t bytes) {
    node_usage_[node] += bytes;
}

void MemoryManager::sub_node_usage(NumaNodeId node, std::uint64_t bytes, bool allow_underflow_error) {
    auto it = node_usage_.find(node);
    if (it == node_usage_.end()) { it = node_usage_.emplace(node, 0).first; }
    if (!allow_underflow_error && bytes > it->second) {
        throw InvalidTransitionError("accounting underflow: release exceeds node allocation");
    }
    it->second -= std::min(it->second, bytes);
    if (it->second == 0) node_usage_.erase(it);
}

NumaNodeId MemoryManager::choose_observed_node(const MemoryRegion& r) const {
    if (r.locality_known && r.actual_node.is_valid()) return r.actual_node;
    if (r.actual_node.is_valid()) return r.actual_node;
    return r.intended_node;
}

MemoryRegion MemoryManager::allocate(const AllocateRequest& req) {
    if (req.bytes.is_zero()) {
        throw InvalidTransitionError("cannot allocate zero bytes");
    }

    backend::AllocationSpec spec;
    spec.bytes = req.bytes;
    spec.alignment = req.alignment;
    spec.mode = req.mode;
    spec.preferred_node = req.preferred_node;
    spec.required_node = req.required_node;
    spec.interleave_nodes = req.interleave_nodes;
    spec.kind = req.kind;
    spec.touch = req.touch;
    spec.tag = req.tag;

    backend::AllocationResult result;
    bool backend_failed = false;
    std::string backend_error;
    {
        std::lock_guard<std::mutex> bk(backend_call_mutex_);
        try {
            result = backend_.allocate(spec);
        } catch (const backend::BackendError& e) {
            backend_failed = true;
            backend_error = e.what();
        }
    }

    std::lock_guard<std::mutex> lk(mutex_);

    const auto id = MemoryRegionId::from(next_id_++);
    MemoryRegion r;
    r.id = id;
    r.generation = watermark_.next();
    watermark_ = r.generation;
    r.kind = req.kind;
    r.mode = req.mode;
    r.requested_bytes = req.bytes;
    r.alignment = req.alignment;
    r.preferred_node = req.preferred_node;
    r.required_node = req.required_node;
    r.intended_node = req.intended_node;
    r.owning_process = req.owning_process;
    r.associated_worker = req.associated_worker;
    r.associated_accelerator = req.associated_accelerator;
    r.workload_id = req.workload_id;
    r.tag = req.tag;
    r.provenance = req.provenance;
    r.created_ms = Clock::now_ns();
    r.updated_ms = r.created_ms;

    if (backend_failed) {
        r.state = LifecycleState::Failed;
        r.locality_current = false;
        regions_[id] = r;
        return r;
    }

    r.kind = result.kind;
    r.granted_bytes = result.granted_bytes;
    r.actual_node = result.observed_node;
    r.locality_known = result.locality_known;
    r.locality_verified = false;
    r.locality_current = true;
    r.touched = result.touched;
    r.backend_result = result;
    r.state = req.touch ? LifecycleState::Active : LifecycleState::Allocated;

    const auto node = choose_observed_node(r);
    if (node.is_valid()) add_node_usage(node, r.granted_bytes.value());
    else if (req.intended_node.is_valid()) add_node_usage(req.intended_node, r.granted_bytes.value());

    regions_[id] = r;
    (void)backend_error;
    return r;
}

void MemoryManager::release(MemoryRegionId id, std::optional<MemoryGeneration> expected) {
    backend::AllocationResult to_free;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = regions_.find(id);
        if (it == regions_.end()) {
            throw InvalidTransitionError("release of unknown memory region " + id.to_string());
        }
        MemoryRegion& r = it->second;
        if (expected && *expected != r.generation) {
            throw InvalidTransitionError("stale release generation for region " + id.to_string());
        }
        if (r.is_released()) {
            throw InvalidTransitionError("double release of region " + id.to_string());
        }
        if (!can_transition(r.state, LifecycleState::Released) &&
            !can_transition(r.state, LifecycleState::Invalidated)) {
            throw InvalidTransitionError("release not permitted from state " +
                                          std::string(lifecycle_name(r.state)));
        }
        const auto node = choose_observed_node(r);
        if (node.is_valid()) sub_node_usage(node, r.granted_bytes.value(), false);
        r.state = LifecycleState::Released;
        r.updated_ms = Clock::now_ns();
        r.locality_current = false;
        to_free = r.backend_result;
    }
    {
        std::lock_guard<std::mutex> bk(backend_call_mutex_);
        backend_.free(to_free);
    }
}

void MemoryManager::transition(MemoryRegionId id, LifecycleState to) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = regions_.find(id);
    if (it == regions_.end()) {
        throw InvalidTransitionError("transition of unknown region " + id.to_string());
    }
    auto& r = it->second;
    if (!can_transition(r.state, to)) {
        throw InvalidTransitionError("transition from " + std::string(lifecycle_name(r.state)) +
                                     " to " + std::string(lifecycle_name(to)) + " is not guarded");
    }
    r.state = to;
    r.updated_ms = Clock::now_ns();
}

const MemoryRegion* MemoryManager::get(MemoryRegionId id) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = regions_.find(id);
    return it == regions_.end() ? nullptr : &it->second;
}

MemoryRegion* MemoryManager::get_mut(MemoryRegionId id) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = regions_.find(id);
    return it == regions_.end() ? nullptr : &it->second;
}

void MemoryManager::invalidate_locality_stale() {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& [id, r] : regions_) {
        if (r.is_live()) {
            r.locality_current = false;
            r.locality_verified = false;
            r.updated_ms = Clock::now_ns();
        }
    }
}

std::uint64_t MemoryManager::per_node_allocated(NumaNodeId id) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = node_usage_.find(id);
    return it == node_usage_.end() ? 0 : it->second;
}

std::uint64_t MemoryManager::total_allocated() const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::uint64_t t = 0;
    for (const auto& [node, bytes] : node_usage_) { t += bytes; }
    return t;
}

bool MemoryManager::accounting_clean() const {
    std::lock_guard<std::mutex> lk(mutex_);
    for (const auto& [id, r] : regions_) {
        if (r.is_live()) return false;
    }
    return node_usage_.empty();
}

MemoryManager::MigrationView MemoryManager::governed_reallocate(MemoryRegionId id, NumaNodeId target, bool copy_data) {
    MigrationView out;

    // Allocate the new homed buffer at the target node (a strict placement).
    backend::AllocationSpec spec;
    spec.bytes = Bytes::zero();
    spec.kind = AllocationKind::Host;
    spec.touch = true;
    spec.mode = PlacementMode::RequiredNode;
    spec.required_node = target;
    spec.tag = "migration";

    // Snapshot the region we are migrating so we can copy + fence deterministically.
    MemoryRegion snapshot;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = regions_.find(id);
        if (it == regions_.end()) throw InvalidTransitionError("migrate unknown region " + id.to_string());
        auto& r = it->second;
        if (r.is_released()) throw InvalidTransitionError("cannot migrate a released region " + id.to_string());
        if (!can_transition(r.state, LifecycleState::Migrating))
            throw InvalidTransitionError("migrate not permitted from " + std::string(lifecycle_name(r.state)));
        r.state = LifecycleState::Migrating;
        r.updated_ms = Clock::now_ns();
        snapshot = r;
    }
    spec.bytes = snapshot.granted_bytes;

    backend::AllocationResult new_res;
    bool new_ok = true;
    {
        std::lock_guard<std::mutex> bk(backend_call_mutex_);
        try {
            new_res = backend_.allocate(spec);
        } catch (const backend::BackendError&) {
            new_ok = false;
        }
    }

    if (!new_ok || !new_res.ok()) {
        // Failed to obtain target memory: retain the original authoritative copy.
        {
            std::lock_guard<std::mutex> lk(mutex_);
            auto it = regions_.find(id);
            if (it != regions_.end() && it->second.state == LifecycleState::Migrating)
                it->second.state = LifecycleState::Active;
        }
        return out;
    }

    // Copy the payload out of the authoritative buffer (only for real memory).
    bool copy_ok = true;
    if (copy_data && snapshot.backend_result.ptr_valid && new_res.ptr_valid && snapshot.backend_result.ptr && new_res.ptr) {
        const auto n = std::min(snapshot.granted_bytes.value(), new_res.granted_bytes.value());
        std::memcpy(new_res.ptr, snapshot.backend_result.ptr, static_cast<std::size_t>(n));
    }

    if (!copy_ok) {
        // Clean up the provisional buffer; retain the original authority.
        {
            std::lock_guard<std::mutex> bk(backend_call_mutex_);
            backend_.free(new_res);
        }
        {
            std::lock_guard<std::mutex> lk(mutex_);
            auto it = regions_.find(id);
            if (it != regions_.end() && it->second.state == LifecycleState::Migrating)
                it->second.state = LifecycleState::Active;
        }
        return out;
    }

    // Commit: advance generation, fence the old placement, free the old buffer.
    {
        std::lock_guard<std::mutex> bk(backend_call_mutex_);
        backend_.free(snapshot.backend_result);
    }
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = regions_.find(id);
        if (it == regions_.end()) return out;
        auto& r = it->second;
        // move accounting from old node to new node
        const auto old_node = choose_observed_node(snapshot);
        if (old_node.is_valid()) sub_node_usage(old_node, r.granted_bytes.value(), false);
        r.backend_result = new_res;
        r.actual_node = new_res.observed_node;
        r.intended_node = target;
        r.locality_known = new_res.locality_known;
        r.locality_verified = false;
        r.locality_current = true;
        r.generation = r.generation.next();
        r.state = LifecycleState::Rebound;
        r.updated_ms = Clock::now_ns();
        const auto new_node = choose_observed_node(r);
        if (new_node.is_valid()) add_node_usage(new_node, r.granted_bytes.value());
        out.committed = true;
        out.copy_succeeded = true;
        out.new_node = new_res.observed_node.is_valid() ? new_res.observed_node : target;
        out.generation = r.generation;
    }
    return out;
}

std::vector<MemoryRegion> MemoryManager::all_regions() const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<MemoryRegion> out;
    out.reserve(regions_.size());
    for (const auto& [id, r] : regions_) out.push_back(r);
    return out;
}

} // namespace memory
} // namespace numafabric
