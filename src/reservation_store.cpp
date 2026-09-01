// ============================================================================
// Reservation store implementation (atomic check-and-commit under one lock).
// ============================================================================

#include "numafabric/reservation/reservation_store.hpp"

#include <algorithm>

namespace numafabric {
namespace reservation {

Reservation ReservationStore::reserve(const ReserveRequest& req) {
    if (!req.node.is_valid()) throw ReservationError("reservation requires a node");
    if (req.bytes.is_zero() && req.cpu_slots == 0 && !req.accelerator_capacity)
        throw ReservationError("empty reservation (nothing requested)");

    std::lock_guard<std::mutex> lk(mutex_);
    const auto id = ReservationId::from(next_id_++);
    const auto gen = watermark_.next();
    watermark_ = gen;

    if (!req.bytes.is_zero()) {
        const auto already = node_mem_[req.node];
        const auto want = already + req.bytes.value();
        if (node_capacity_) {
            const auto cap = node_capacity_(req.node).value();
            if (cap != 0 && want > cap) {
                throw ReservationError("memory capacity oversubscription on node " + req.node.to_string());
            }
        }
    }
    if (req.accelerator_capacity && req.accelerator.is_valid()) {
        const auto already = accel_mem_[req.accelerator];
        constexpr std::uint64_t kAccelCeiling = 64ULL * 1024 * 1024 * 1024;
        if (already + req.bytes.value() > kAccelCeiling)
            throw ReservationError("accelerator-local capacity ceiling exceeded");
    }

    Reservation r;
    r.id = id;
    r.generation = gen;
    r.node = req.node;
    r.bytes = req.bytes;
    r.cpu_slots = req.cpu_slots;
    r.accelerator = req.accelerator;
    r.accelerator_capacity = req.accelerator_capacity;
    r.owner = req.owner;
    r.worker = req.worker;
    r.active = true;
    r.created_ms = 0;

    if (!req.bytes.is_zero()) node_mem_[req.node] += req.bytes.value();
    if (req.cpu_slots > 0) node_slots_[req.node] += req.cpu_slots;
    if (req.accelerator_capacity && req.accelerator.is_valid())
        accel_mem_[req.accelerator] += req.bytes.value();

    reservations_[id] = r;
    return r;
}

void ReservationStore::release(ReservationId id, std::optional<ReservationGeneration> expected) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = reservations_.find(id);
    if (it == reservations_.end()) throw ReservationError("release of unknown reservation " + id.to_string());
    Reservation& r = it->second;
    if (!r.active) throw ReservationError("duplicate release of reservation " + id.to_string());
    if (expected && *expected != r.generation)
        throw ReservationError("stale release generation for reservation " + id.to_string());

    const auto b = r.bytes.value();
    auto mit = node_mem_.find(r.node);
    if (mit != node_mem_.end()) {
        if (b > mit->second) throw ReservationError("accounting underflow on node " + r.node.to_string());
        mit->second -= b;
        if (mit->second == 0) node_mem_.erase(mit);
    }
    auto sit = node_slots_.find(r.node);
    if (sit != node_slots_.end()) {
        if (r.cpu_slots > sit->second) throw ReservationError("slot accounting underflow");
        sit->second -= r.cpu_slots;
        if (sit->second == 0) node_slots_.erase(sit);
    }
    if (r.accelerator_capacity && r.accelerator.is_valid()) {
        auto ait = accel_mem_.find(r.accelerator);
        if (ait != accel_mem_.end()) {
            if (b > ait->second) throw ReservationError("accelerator accounting underflow");
            ait->second -= b;
            if (ait->second == 0) accel_mem_.erase(ait);
        }
    }
    r.active = false;
}

bool ReservationStore::restore(const Reservation& r) {
    if (!r.id.is_valid()) throw ReservationError("cannot restore invalid reservation id");
    std::lock_guard<std::mutex> lk(mutex_);
    if (reservations_.count(r.id)) throw ReservationError("duplicate reservation id on restore " + r.id.to_string());
    if (!r.active) { reservations_[r.id] = r; return true; }
    if (!r.bytes.is_zero() && node_capacity_) {
        const auto cap = node_capacity_(r.node).value();
        if (cap != 0 && node_mem_[r.node] + r.bytes.value() > cap) return false; // capacity gone
    }
    if (!r.bytes.is_zero()) node_mem_[r.node] += r.bytes.value();
    if (r.cpu_slots > 0) node_slots_[r.node] += r.cpu_slots;
    if (r.accelerator_capacity && r.accelerator.is_valid()) accel_mem_[r.accelerator] += r.bytes.value();
    reservations_[r.id] = r;
    if (r.generation.value() > watermark_.value()) watermark_ = r.generation;
    return true;
}

const Reservation* ReservationStore::get(ReservationId id) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = reservations_.find(id);
    return it == reservations_.end() ? nullptr : &it->second;
}

std::vector<Reservation> ReservationStore::all() const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<Reservation> out;
    out.reserve(reservations_.size());
    for (const auto& [id, r] : reservations_) out.push_back(r);
    return out;
}

std::uint64_t ReservationStore::node_reserved_memory(NumaNodeId id) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = node_mem_.find(id);
    return it == node_mem_.end() ? 0 : it->second;
}

std::uint64_t ReservationStore::total_reserved_memory() const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::uint64_t t = 0;
    for (const auto& [n, b] : node_mem_) t += b;
    return t;
}

std::uint64_t ReservationStore::accelerator_reserved(AcceleratorId id) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = accel_mem_.find(id);
    return it == accel_mem_.end() ? 0 : it->second;
}

bool ReservationStore::accounting_clean() const {
    std::lock_guard<std::mutex> lk(mutex_);
    for (const auto& [id, r] : reservations_) { if (r.active) return false; }
    return node_mem_.empty() && node_slots_.empty() && accel_mem_.empty();
}

bool ReservationStore::has_capacity(NumaNodeId node, Bytes want) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = node_mem_.find(node);
    const auto used = it == node_mem_.end() ? 0 : it->second;
    const auto want_b = want.value();
    std::uint64_t cap = 0;
    if (node_capacity_) cap = node_capacity_(node).value();
    if (cap != 0 && used + want_b > cap) return false;
    return true;
}

} // namespace reservation
} // namespace numafabric
