// Example 2: inspect NUMA nodes and their processor membership.
#include "numafabric/numafabric.hpp"
#include <cstdio>
int main() {
    auto rt = numafabric::Runtime::create_windows();
    rt->discover();
    for (const auto& n : rt->topology().nodes) {
        std::printf("node %llu: %zu processors, capacity=%llu\n",
                    n.id.value(), n.processor_count(), n.memory_capacity.value());
        for (const auto& pid : n.processors) {
            const auto* p = rt->topology().find_processor(pid);
            if (p) std::printf("  processor %llu group=%llu\n", pid.value(), p->group.value());
        }
    }
    return 0;
}
