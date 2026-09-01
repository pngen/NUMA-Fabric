// Example 4: register a worker, bind it to a node, restore affinity (scoped).
#include "numafabric/numafabric.hpp"
#include <cstdio>
int main() {
    auto rt = numafabric::Runtime::create_windows();
    rt->discover();
    auto w = rt->register_worker(numafabric::WorkerId::from(1), numafabric::ProcessId::from(1234));
    const auto node = rt->topology().nodes.front().id;
    auto b = rt->bind_worker(w.id, w.boot_id, node, /*apply_affinity=*/true);
    std::printf("worker %llu boot=%llu bound to node %llu binding=%llu gen=%llu applied=%d\n",
                w.id.value(), w.boot_id.value(), node.value(), b.binding_id.value(), b.generation.value(), b.applied ? 1 : 0);
    return 0;
}
