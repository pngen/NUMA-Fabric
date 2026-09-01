// Example 10: stale authority is fenced (old boot cannot bind after restart).
#include "numafabric/numafabric.hpp"
#include <cstdio>
int main() {
    auto rt = numafabric::Runtime::create_synthetic([] {
        numafabric::backend::SyntheticConfig c; c.node_count = 2; c.processors_per_node = 8; return c; }());
    rt->discover();
    const auto node = rt->topology().nodes.front().id;
    auto w = rt->register_worker(numafabric::WorkerId::from(1), numafabric::ProcessId::from(1));
    auto oldBoot = w.boot_id;
    rt->restart_worker(numafabric::WorkerId::from(1), numafabric::ProcessId::from(2));
    auto w2 = rt->worker(numafabric::WorkerId::from(1));
    std::printf("old boot=%llu current=%llu fresh=%d\n",
                oldBoot.value(), w2->boot_id.value(),
                (w2->boot_id.value() != oldBoot.value()) ? 1 : 0);
    bool staleOk = rt->workers_registry().validate_boot(numafabric::WorkerId::from(1), oldBoot);
    std::printf("stale boot accepted=%d (must be 0)\n", staleOk ? 1 : 0);
    try { rt->bind_worker(numafabric::WorkerId::from(1), oldBoot, node, false); std::printf("unexpected: stale bind allowed\n"); }
    catch (...) { std::printf("stale bind correctly rejected\n"); }
    return 0;
}
