#pragma once
// ============================================================================
// NUMA Fabric - freshness / revalidation model.
//
// Physical locality evidence can become stale after worker restart, coordinator
// epoch change, device generation change, topology refresh, processor-group
// change, policy change, process restart, reservation change, or recovery from
// persistence. A recovered observation is NEVER treated as CURRENT merely
// because its timestamp is recent: the runtime demands an explicit
// REVALIDATION_REQUIRED state and does not infer freshness from wall-clock age.
// ============================================================================

#include "numafabric/core/enums.hpp"
#include "numafabric/core/quantities.hpp"

#include <cstdint>
#include <ostream>

namespace numafabric {

class Clock {
public:
    // Monotonic relative timestamp in nanoseconds. Not a wall-clock value; used
    // only to reason about staleness ordering, never as a guarantee of
    // physical freshness.
    static Nanoseconds monotonic_now() {
        return Nanoseconds(static_cast<std::uint64_t>(now_ns()));
    }
    static std::uint64_t now_ns();
};

struct Freshness {
    FreshnessState state = FreshnessState::Unknown;
    Nanoseconds measured_at = Nanoseconds::zero();

    static Freshness current() { Freshness f; f.state = FreshnessState::Current; f.measured_at = Clock::monotonic_now(); return f; }
    static Freshness stale() { Freshness f; f.state = FreshnessState::Stale; f.measured_at = Clock::monotonic_now(); return f; }
    static Freshness revalidation_required() {
        Freshness f; f.state = FreshnessState::RevalidationRequired; f.measured_at = Clock::monotonic_now(); return f;
    }
    static Freshness unknown() { Freshness f; f.state = FreshnessState::Unknown; return f; }
};

inline std::ostream& operator<<(std::ostream& os, const Freshness& f) {
    os << numafabric::to_string(f.state);
    return os;
}

} // namespace numafabric
