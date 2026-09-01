#pragma once
// ============================================================================
// NUMA Fabric - NUMA-local reservations and accounting.
//
// A reservation atomically and simultaneously claims memory capacity, worker /
// execution slots, and optional accelerator-associated locality capacity on a
// node. If any required dimension fails the whole reservation is refused
// (no partial claim is left behind). Oversubscription, double reservation,
// duplicate release, stale release and generation rollback are rejected.
// ============================================================================

#include "numafabric/core/ids.hpp"
#include "numafabric/core/provenance.hpp"
#include "numafabric/core/quantities.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace numafabric {
namespace reservation {

class ReservationError final : public std::exception {
public:
    explicit ReservationError(std::string m) : message_(std::move(m)) {}
    const char* what() const noexcept override { return message_.c_str(); }
private:
    std::string message_;
};

struct Reservation {
    ReservationId id = ReservationId::invalid();
    NumaNodeId node = NumaNodeId::invalid();
    Bytes bytes = Bytes::zero();
    std::uint32_t cpu_slots = 0;
    AcceleratorId accelerator = AcceleratorId::invalid();
    bool accelerator_capacity = false;
    ReservationGeneration generation = ReservationGeneration::initial();
    ProcessId owner = ProcessId::invalid();
    WorkerId worker = WorkerId::invalid();
    bool active = true;
    std::uint64_t created_ms = 0;
};

struct ReserveRequest {
    NumaNodeId node = NumaNodeId::invalid();
    Bytes bytes = Bytes::zero();
    std::uint32_t cpu_slots = 0;
    AcceleratorId accelerator = AcceleratorId::invalid();
    bool accelerator_capacity = false;
    ProcessId owner = ProcessId::invalid();
    WorkerId worker = WorkerId::invalid();
    std::optional<ReservationGeneration> expected_generation = std::nullopt; // for renew
};

class ReservationStore {
public:
    // node_capacity: returns per-node host-memory capacity; used to enforce
    // oversubscription against physical/declared limits.
    explicit ReservationStore(std::function<Capacity(NumaNodeId)> node_capacity = nullptr)
        : node_capacity_(std::move(node_capacity)) {}

    Reservation reserve(const ReserveRequest& req);
    void release(ReservationId id, std::optional<ReservationGeneration> expected = std::nullopt);

    // Re-admit a previously persisted reservation preserving identity + generation
    // (used by recovery). Returns false with no mutation if capacity no longer
    // permits it.
    bool restore(const Reservation& r);

    const Reservation* get(ReservationId id) const;
    std::vector<Reservation> all() const;

    std::uint64_t node_reserved_memory(NumaNodeId id) const;
    std::uint64_t total_reserved_memory() const;
    std::uint64_t accelerator_reserved(AcceleratorId id) const;

    // Aggregate host accounting: closes exactly to zero when no reservations live.
    bool accounting_clean() const;

    // Capacity check helper used by placement / memory admission.
    bool has_capacity(NumaNodeId node, Bytes want) const;

private:
    std::function<Capacity(NumaNodeId)> node_capacity_;
    mutable std::mutex mutex_;
    std::map<ReservationId, Reservation> reservations_;
    std::map<NumaNodeId, std::uint64_t> node_mem_;
    std::map<NumaNodeId, std::uint32_t> node_slots_;
    std::map<AcceleratorId, std::uint64_t> accel_mem_;
    std::uint64_t next_id_ = 1;
    ReservationGeneration watermark_ = ReservationGeneration::initial();
};

} // namespace reservation
} // namespace numafabric
