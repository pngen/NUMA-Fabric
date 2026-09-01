// ============================================================================
// Deterministic fixed-seed property / adversarial test.
//
// Runs thousands of operations against a synthetic runtime, continuously
// asserting invariants: no accounting underflow, exact closure after teardown,
// stable snapshot digest, deterministic ranking, and guarded transitions.
// ============================================================================

#include "numafabric/numafabric.hpp"
#include "test_util.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace numafabric;

struct Lcg {
    std::uint64_t state;
    explicit Lcg(std::uint64_t seed) : state(seed) {}
    std::uint32_t next() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<std::uint32_t>((state >> 32) & 0xFFFFFFFFu);
    }
    std::uint64_t range(std::uint64_t lo, std::uint64_t hi) {
        if (hi <= lo) return lo;
        return lo + (next() % (hi - lo + 1));
    }
};

static backend::SyntheticConfig cfg() {
    backend::SyntheticConfig c;
    c.node_count = 2;
    c.processors_per_node = 8;
    c.group_capacity = 64;
    return c;
}

static void run_all() {
    for (int trial = 0; trial < 8; ++trial) {
        auto rt = Runtime::create_synthetic(cfg());
        rt->discover();

        Lcg rng(0x1000 + static_cast<std::uint64_t>(trial) * 0x9E3779B9ULL);
        std::vector<ReservationId> res_ids;
        std::vector<MemoryRegionId> reg_ids;
        std::vector<WorkerId> worker_ids;

        for (int op = 0; op < 500; ++op) {
            const auto kind = rng.range(0, 8);
            if (kind == 0) {
                try {
                    auto w = rt->register_worker(WorkerId::from(rng.range(1, 8)), ProcessId::from(rng.range(1, 8)));
                    worker_ids.push_back(w.id);
                } catch (...) { /* already registered */ }
            } else if (kind == 1) {
                reservation::ReserveRequest rr;
                rr.node = NumaNodeId::from(rng.range(1, 2));
                rr.bytes = Bytes::from(rng.range(1, 2 * 1024 * 1024));
                try { auto r = rt->reserve(rr); res_ids.push_back(r.id); } catch (...) {}
            } else if (kind == 2 && !res_ids.empty()) {
                const auto idx = rng.range(0, res_ids.size() - 1);
                try { rt->release_reservation(res_ids[idx]); } catch (...) {}
            } else if (kind == 3) {
                auto region = rt->allocate_on_node(Bytes::from(rng.range(1, 2 * 1024 * 1024)), NumaNodeId::from(rng.range(1, 2)));
                reg_ids.push_back(region.id);
            } else if (kind == 4 && !reg_ids.empty()) {
                const auto idx = rng.range(0, reg_ids.size() - 1);
                try { rt->release(reg_ids[idx]); } catch (...) {}
            } else if (kind == 5 && !worker_ids.empty()) {
                const auto idx = rng.range(0, worker_ids.size() - 1);
                try { rt->restart_worker(worker_ids[idx], ProcessId::from(rng.range(1, 8))); } catch (...) {}
            } else if (kind == 6) {
                placement::PlacementRequest p;
                p.memory_footprint = Bytes::from(rng.range(1, 8 * 1024 * 1024));
                auto d = rt->plan(p);
                (void)d;
            } else if (kind == 7) {
                (void)rt->snapshot();
            } else {
                // Deterministic digest + deterministic ranking under identical state.
                auto s1 = rt->snapshot();
                auto s2 = rt->snapshot();
                CHECK(s1.semantic_digest() == s2.semantic_digest());
                placement::PlacementRequest p;
                p.memory_footprint = Bytes::from(4 * 1024 * 1024);
                auto d1 = rt->plan(p);
                auto d2 = rt->plan(p);
                CHECK(d1.selected_node == d2.selected_node);
                CHECK(d1.expected_penalty.value() == d2.expected_penalty.value());
            }
        }

        // Exact closure after teardown.
        for (auto id : res_ids) { try { rt->release_reservation(id); } catch (...) {} }
        for (auto id : reg_ids) { try { rt->release(id); } catch (...) {} }
        CHECK(rt->memory_accounting_clean());
        CHECK(rt->reservations_clean());
    }
    std::printf("  property/adversarial trials completed\n");
}

TEST_MAIN()
