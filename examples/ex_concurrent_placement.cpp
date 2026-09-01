// Example 12: concurrent placement decisions and reservations.
#include "numafabric/numafabric.hpp"
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>
int main() {
    auto rt = numafabric::Runtime::create_synthetic([] {
        numafabric::backend::SyntheticConfig c; c.node_count = 2; c.processors_per_node = 8; return c; }());
    rt->discover();
    std::atomic<int> errors{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t) ts.emplace_back([&] {
        for (int i = 0; i < 200; ++i) {
            numafabric::placement::PlacementRequest p;
            p.memory_footprint = numafabric::Bytes::from(1024u*1024u);
            auto d = rt->plan(p);
            if (d.kind == numafabric::PlacementDecisionKind::Reject) ++errors;
        }
    });
    for (auto& th : ts) th.join();
    std::printf("concurrent placement completed; errors=%d\n", errors.load());
    return errors.load() == 0 ? 0 : 1;
}
