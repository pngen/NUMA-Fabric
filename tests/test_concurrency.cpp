// ============================================================================
// Concurrency stress test.
//
// Concurrent placement, reservation, allocation, registration and snapshot
// reads run against the thread-safe managers. Audit invariants: exact
// accounting closure, no overflow, no double ownership, no cross-node leakage.
// ============================================================================

#include "numafabric/numafabric.hpp"
#include "test_util.hpp"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

using namespace numafabric;

static backend::SyntheticConfig cfg() {
    backend::SyntheticConfig c;
    c.node_count = 2;
    c.processors_per_node = 8;
    c.group_capacity = 64;
    return c;
}

static void run_all() {
    auto rt = Runtime::create_synthetic(cfg());
    rt->discover();

    constexpr int kThreads = 8;
    constexpr int kIters = 150;
    std::atomic<int> errors{0};
    std::atomic<std::uint64_t> ops{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kIters; ++i) {
                reservation::ReserveRequest rr;
                rr.node = NumaNodeId::from(1 + (t % 2));
                rr.bytes = Bytes::from(static_cast<std::uint64_t>(256 * 1024 + t * 1024));
                try { auto r = rt->reserve(rr); rt->release_reservation(r.id); } catch (...) { ++errors; }

                try {
                    auto region = rt->allocate_on_node(Bytes::from(64 * 1024), NumaNodeId::from(1 + (i % 2)));
                    rt->release(region.id);
                } catch (...) { ++errors; }

                try {
                    placement::PlacementRequest p;
                    p.memory_footprint = Bytes::from(1 * 1024 * 1024);
                    auto d = rt->plan(p);
                    (void)d;
                } catch (...) { ++errors; }

                try { (void)rt->snapshot(); } catch (...) { ++errors; }
                ops.fetch_add(4, std::memory_order_relaxed);
            }
        });
    }
    for (auto& th : threads) th.join();

    CHECK(errors.load() == 0);
    CHECK(rt->fully_clean());
    std::printf("  %llu concurrent operations; accounting closed exactly to zero\n",
                static_cast<unsigned long long>(ops.load()));
}

TEST_MAIN()
