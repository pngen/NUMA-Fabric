// ============================================================================
// Runtime core support: monotonic clock + version.
// ============================================================================

#include "numafabric/core/freshness.hpp"

#include <chrono>

namespace numafabric {

std::uint64_t Clock::now_ns() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

} // namespace numafabric
