#pragma once
// ============================================================================
// NUMA Fabric - canonical domain enums.
//
// Every enum is validated on parse (invalid values are rejected rather than
// silently defaulted) and has an explicit string form for deterministic text
// and JSON output.
// ============================================================================

#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace numafabric {

class InvalidEnumError final : public std::exception {
public:
    explicit InvalidEnumError(std::string m) : message_(std::move(m)) {}
    const char* what() const noexcept override { return message_.c_str(); }
private:
    std::string message_;
};

// ---------------------------------------------------------------------------
// Memory placement mode
// ---------------------------------------------------------------------------
enum class PlacementMode {
    Any,             // no locality preference
    Local,           // prefer the node-local memory
    PreferredNode,   // prefer a specific node, fallback allowed
    RequiredNode,    // require a specific node, no fallback
    Interleaved,     // interleave across a set of nodes
    Synthetic        // controlled synthetic placement (validation only)
};

constexpr PlacementMode placement_mode_any() { return PlacementMode::Any; }
constexpr PlacementMode placement_mode_local() { return PlacementMode::Local; }
constexpr PlacementMode placement_mode_preferred_node() { return PlacementMode::PreferredNode; }
constexpr PlacementMode placement_mode_required_node() { return PlacementMode::RequiredNode; }
constexpr PlacementMode placement_mode_interleaved() { return PlacementMode::Interleaved; }
constexpr PlacementMode placement_mode_synthetic() { return PlacementMode::Synthetic; }

// ---------------------------------------------------------------------------
// Memory region lifecycle
// ---------------------------------------------------------------------------
enum class LifecycleState {
    Created,
    Reserved,
    Allocated,
    Active,
    MigrationPending,
    Migrating,
    Rebound,
    ReleasePending,
    Released,
    Failed,
    Invalidated
};

// ---------------------------------------------------------------------------
// Locality class (accelerator/device <-> host domain relationship)
// ---------------------------------------------------------------------------
enum class LocalityClass {
    SameNumaNode,
    SameSocket,
    SameHostRemoteNuma,
    Unknown,
    Synthetic
};

// ---------------------------------------------------------------------------
// Placement decision outcome
// ---------------------------------------------------------------------------
enum class PlacementDecisionKind {
    Place,
    PlaceWithPenalty,
    Rebind,
    Migrate,
    Defer,
    Reject,
    RevalidationRequired
};

// ---------------------------------------------------------------------------
// Freshness
// ---------------------------------------------------------------------------
enum class FreshnessState {
    Current,
    Stale,
    RevalidationRequired,
    Unknown
};

// ---------------------------------------------------------------------------
// Provenance source
// ---------------------------------------------------------------------------
enum class ProvenanceSource {
    Measured,
    Reported,
    Derived,
    Estimated,
    Synthetic,
    Unknown
};

// ---------------------------------------------------------------------------
// Authority / fencing acknowledgement
// ---------------------------------------------------------------------------
enum class AuthorityVerification {
    Accepted,
    StaleEpoch,
    StaleBoot,
    StalePlacementGeneration,
    StaleBindingGeneration,
    StaleMemoryGeneration,
    StaleObservationGeneration,
    StaleReservation,
    StaleAttempt,
    RejectedUnknown
};

// ---------------------------------------------------------------------------
// Allocation kinds (governed host memory)
// ---------------------------------------------------------------------------
enum class AllocationKind {
    Host,     // ordinary OS host allocation
    NumaHost, // governed NUMA-aware host allocation
    Pinned,   // pinned host allocation (page-locked) where supported
    Synthetic // synthetic only - no underlying OS allocation
};

// ---------------------------------------------------------------------------
// to_string
// ---------------------------------------------------------------------------
inline constexpr std::string_view to_string(PlacementMode v) {
    switch (v) {
        case PlacementMode::Any: return "ANY";
        case PlacementMode::Local: return "LOCAL";
        case PlacementMode::PreferredNode: return "PREFERRED_NODE";
        case PlacementMode::RequiredNode: return "REQUIRED_NODE";
        case PlacementMode::Interleaved: return "INTERLEAVED";
        case PlacementMode::Synthetic: return "SYNTHETIC";
    }
    return "UNKNOWN";
}
inline constexpr std::string_view to_string(LifecycleState v) {
    switch (v) {
        case LifecycleState::Created: return "CREATED";
        case LifecycleState::Reserved: return "RESERVED";
        case LifecycleState::Allocated: return "ALLOCATED";
        case LifecycleState::Active: return "ACTIVE";
        case LifecycleState::MigrationPending: return "MIGRATION_PENDING";
        case LifecycleState::Migrating: return "MIGRATING";
        case LifecycleState::Rebound: return "REBOUND";
        case LifecycleState::ReleasePending: return "RELEASE_PENDING";
        case LifecycleState::Released: return "RELEASED";
        case LifecycleState::Failed: return "FAILED";
        case LifecycleState::Invalidated: return "INVALIDATED";
    }
    return "UNKNOWN";
}
inline constexpr std::string_view to_string(LocalityClass v) {
    switch (v) {
        case LocalityClass::SameNumaNode: return "SAME_NUMA_NODE";
        case LocalityClass::SameSocket: return "SAME_SOCKET";
        case LocalityClass::SameHostRemoteNuma: return "SAME_HOST_REMOTE_NUMA";
        case LocalityClass::Unknown: return "UNKNOWN";
        case LocalityClass::Synthetic: return "SYNTHETIC";
    }
    return "UNKNOWN";
}
inline constexpr std::string_view to_string(PlacementDecisionKind v) {
    switch (v) {
        case PlacementDecisionKind::Place: return "PLACE";
        case PlacementDecisionKind::PlaceWithPenalty: return "PLACE_WITH_PENALTY";
        case PlacementDecisionKind::Rebind: return "REBIND";
        case PlacementDecisionKind::Migrate: return "MIGRATE";
        case PlacementDecisionKind::Defer: return "DEFER";
        case PlacementDecisionKind::Reject: return "REJECT";
        case PlacementDecisionKind::RevalidationRequired: return "REVALIDATION_REQUIRED";
    }
    return "UNKNOWN";
}
inline constexpr std::string_view to_string(FreshnessState v) {
    switch (v) {
        case FreshnessState::Current: return "CURRENT";
        case FreshnessState::Stale: return "STALE";
        case FreshnessState::RevalidationRequired: return "REVALIDATION_REQUIRED";
        case FreshnessState::Unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}
inline constexpr std::string_view to_string(ProvenanceSource v) {
    switch (v) {
        case ProvenanceSource::Measured: return "MEASURED";
        case ProvenanceSource::Reported: return "REPORTED";
        case ProvenanceSource::Derived: return "DERIVED";
        case ProvenanceSource::Estimated: return "ESTIMATED";
        case ProvenanceSource::Synthetic: return "SYNTHETIC";
        case ProvenanceSource::Unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}
inline constexpr std::string_view to_string(AuthorityVerification v) {
    switch (v) {
        case AuthorityVerification::Accepted: return "ACCEPTED";
        case AuthorityVerification::StaleEpoch: return "STALE_EPOCH";
        case AuthorityVerification::StaleBoot: return "STALE_BOOT";
        case AuthorityVerification::StalePlacementGeneration: return "STALE_PLACEMENT_GENERATION";
        case AuthorityVerification::StaleBindingGeneration: return "STALE_BINDING_GENERATION";
        case AuthorityVerification::StaleMemoryGeneration: return "STALE_MEMORY_GENERATION";
        case AuthorityVerification::StaleObservationGeneration: return "STALE_OBSERVATION_GENERATION";
        case AuthorityVerification::StaleReservation: return "STALE_RESERVATION";
        case AuthorityVerification::StaleAttempt: return "STALE_ATTEMPT";
        case AuthorityVerification::RejectedUnknown: return "REJECTED_UNKNOWN";
    }
    return "UNKNOWN";
}
inline constexpr std::string_view to_string(AllocationKind v) {
    switch (v) {
        case AllocationKind::Host: return "HOST";
        case AllocationKind::NumaHost: return "NUMA_HOST";
        case AllocationKind::Pinned: return "PINNED";
        case AllocationKind::Synthetic: return "SYNTHETIC";
    }
    return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// from_string (rejects unknown/malformed values with InvalidEnumError)
// ---------------------------------------------------------------------------
inline PlacementMode placement_mode_from_string(std::string_view s) {
    if (s == "ANY") return PlacementMode::Any;
    if (s == "LOCAL") return PlacementMode::Local;
    if (s == "PREFERRED_NODE") return PlacementMode::PreferredNode;
    if (s == "REQUIRED_NODE") return PlacementMode::RequiredNode;
    if (s == "INTERLEAVED") return PlacementMode::Interleaved;
    if (s == "SYNTHETIC") return PlacementMode::Synthetic;
    throw InvalidEnumError("unknown PlacementMode '" + std::string(s) + "'");
}
inline LifecycleState lifecycle_state_from_string(std::string_view s) {
    if (s == "CREATED") return LifecycleState::Created;
    if (s == "RESERVED") return LifecycleState::Reserved;
    if (s == "ALLOCATED") return LifecycleState::Allocated;
    if (s == "ACTIVE") return LifecycleState::Active;
    if (s == "MIGRATION_PENDING") return LifecycleState::MigrationPending;
    if (s == "MIGRATING") return LifecycleState::Migrating;
    if (s == "REBOUND") return LifecycleState::Rebound;
    if (s == "RELEASE_PENDING") return LifecycleState::ReleasePending;
    if (s == "RELEASED") return LifecycleState::Released;
    if (s == "FAILED") return LifecycleState::Failed;
    if (s == "INVALIDATED") return LifecycleState::Invalidated;
    throw InvalidEnumError("unknown LifecycleState '" + std::string(s) + "'");
}
inline ProvenanceSource provenance_from_string(std::string_view s) {
    if (s == "MEASURED") return ProvenanceSource::Measured;
    if (s == "REPORTED") return ProvenanceSource::Reported;
    if (s == "DERIVED") return ProvenanceSource::Derived;
    if (s == "ESTIMATED") return ProvenanceSource::Estimated;
    if (s == "SYNTHETIC") return ProvenanceSource::Synthetic;
    if (s == "UNKNOWN") return ProvenanceSource::Unknown;
    throw InvalidEnumError("unknown ProvenanceSource '" + std::string(s) + "'");
}
inline PlacementDecisionKind placement_decision_from_string(std::string_view s) {
    if (s == "PLACE") return PlacementDecisionKind::Place;
    if (s == "PLACE_WITH_PENALTY") return PlacementDecisionKind::PlaceWithPenalty;
    if (s == "REBIND") return PlacementDecisionKind::Rebind;
    if (s == "MIGRATE") return PlacementDecisionKind::Migrate;
    if (s == "DEFER") return PlacementDecisionKind::Defer;
    if (s == "REJECT") return PlacementDecisionKind::Reject;
    if (s == "REVALIDATION_REQUIRED") return PlacementDecisionKind::RevalidationRequired;
    throw InvalidEnumError("unknown PlacementDecisionKind '" + std::string(s) + "'");
}
inline LocalityClass locality_class_from_string(std::string_view s) {
    if (s == "SAME_NUMA_NODE") return LocalityClass::SameNumaNode;
    if (s == "SAME_SOCKET") return LocalityClass::SameSocket;
    if (s == "SAME_HOST_REMOTE_NUMA") return LocalityClass::SameHostRemoteNuma;
    if (s == "UNKNOWN") return LocalityClass::Unknown;
    if (s == "SYNTHETIC") return LocalityClass::Synthetic;
    throw InvalidEnumError("unknown LocalityClass '" + std::string(s) + "'");
}

inline std::ostream& operator<<(std::ostream& os, PlacementMode v) { return os << to_string(v); }
inline std::ostream& operator<<(std::ostream& os, LifecycleState v) { return os << to_string(v); }
inline std::ostream& operator<<(std::ostream& os, LocalityClass v) { return os << to_string(v); }
inline std::ostream& operator<<(std::ostream& os, PlacementDecisionKind v) { return os << to_string(v); }
inline std::ostream& operator<<(std::ostream& os, FreshnessState v) { return os << to_string(v); }
inline std::ostream& operator<<(std::ostream& os, ProvenanceSource v) { return os << to_string(v); }
inline std::ostream& operator<<(std::ostream& os, AuthorityVerification v) { return os << to_string(v); }
inline std::ostream& operator<<(std::ostream& os, AllocationKind v) { return os << to_string(v); }

} // namespace numafabric
