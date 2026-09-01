#pragma once
// ============================================================================
// NUMA Fabric - versioned binary persistence.
//
// Persists LOGICAL state separately from physical freshness. The container is
// magic-prefixed + versioned + CRC-32 protected, encoded deterministically, and
// written atomically (temp -> flush -> close -> rename). Decoding is bounded and
// strict: truncation, duplicate IDs, impossible counts, malformed enums, invalid
// generations and trailing garbage are all rejected. Recovery never claims the
// current worker boot, process/thread affinity, physical page locality, device
// health or topology freshness without revalidation.
// ============================================================================

#include "numafabric/core/ids.hpp"
#include "numafabric/core/provenance.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace numafabric {
namespace persist {

class PersistenceError final : public std::exception {
public:
    explicit PersistenceError(std::string m) : message_(std::move(m)) {}
    const char* what() const noexcept override { return message_.c_str(); }
private:
    std::string message_;
};

inline constexpr std::uint8_t kContainerMagic[12] = {'N','U','M','A','F','A','B','R','I','C','P','1'};
inline constexpr std::uint32_t kContainerVersion = 1;
inline constexpr std::uint64_t kMaxPersistedRecords = 1u << 20;

struct PersistedWorker {
    WorkerId id = WorkerId::invalid();
    WorkerBootId boot = WorkerBootId::invalid();
    std::uint64_t incarnation = 0;
    bool alive = false;
    NumaNodeId node = NumaNodeId::invalid();
    PlacementGeneration placement_generation = PlacementGeneration::initial();
    BindingGeneration binding_generation = BindingGeneration::initial();
    PolicyGeneration policy_generation = PolicyGeneration::initial();
    std::uint64_t live_generation = 0;
};

struct PersistedReservation {
    ReservationId id = ReservationId::invalid();
    NumaNodeId node = NumaNodeId::invalid();
    std::uint64_t bytes = 0;
    std::uint32_t cpu_slots = 0;
    AcceleratorId accelerator = AcceleratorId::invalid();
    bool accelerator_capacity = false;
    ReservationGeneration generation = ReservationGeneration::initial();
    bool active = false;
};

struct PersistedPlacement {
    PlacementId id = PlacementId::invalid();
    PlacementGeneration generation = PlacementGeneration::initial();
    WorkerId worker = WorkerId::invalid();
    WorkerBootId worker_boot = WorkerBootId::invalid();
    NumaNodeId node = NumaNodeId::invalid();
    ProvenanceSource source = ProvenanceSource::Unknown;
    std::uint64_t committed_generation = 0;
};

struct PersistedObservation {
    ObservationId id = ObservationId::invalid();
    ObservationGeneration generation = ObservationGeneration::initial();
    WorkerId worker = WorkerId::invalid();
    WorkerBootId boot = WorkerBootId::invalid();
    NumaNodeId node = NumaNodeId::invalid();
    ProvenanceSource source = ProvenanceSource::Unknown;
    bool stale = false;
};

// Authoritative logical state captured for durability.
struct PersistedState {
    std::uint64_t coordinator_epoch = 0;
    std::uint64_t host_generation = 0;
    std::uint64_t policy_generation = 0;
    std::vector<PersistedWorker> workers;
    std::vector<PersistedReservation> reservations;
    std::vector<PersistedPlacement> placements;
    std::vector<PersistedObservation> observations;
};

// Deterministic binary serialization of a PersistedState.
std::vector<std::uint8_t> serialize(const PersistedState& state);
PersistedState deserialize(const std::vector<std::uint8_t>& data);

// Atomic save / strict load of the container to/from a path.
void save(const PersistedState& state, const std::string& path);
PersistedState load(const std::string& path);

} // namespace persist
} // namespace numafabric
