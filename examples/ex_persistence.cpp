// Example 11: persist authoritative state, then recover it and require revalidation.
#include "numafabric/numafabric.hpp"
#include <cstdio>
int main() {
    const std::string path = "example_state.bin";
    {
        auto rt = numafabric::Runtime::create_synthetic([] {
            numafabric::backend::SyntheticConfig c; c.node_count = 2; c.processors_per_node = 8; return c; }());
        rt->discover();
        rt->register_worker(numafabric::WorkerId::from(7), numafabric::ProcessId::from(1));
        rt->persist(path);
        std::printf("persisted worker 7 to %s\n", path.c_str());
    }
    {
        auto rt = numafabric::Runtime::create_synthetic([] {
            numafabric::backend::SyntheticConfig c; c.node_count = 2; c.processors_per_node = 8; return c; }());
        rt->discover();
        rt->recover(path);
        const auto* w = rt->worker(numafabric::WorkerId::from(7));
        std::printf("recovered worker 7: boot=%llu alive=%d (locality marked stale until revalidated)\n",
                    w ? w->boot_id.value() : 0, w ? w->alive : 0);
    }
    std::remove(path.c_str());
    return 0;
}
