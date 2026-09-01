// Example 7: reserve and release NUMA-local capacity.
#include "numafabric/numafabric.hpp"
#include <cstdio>
int main() {
    auto rt = numafabric::Runtime::create_synthetic([] {
        numafabric::backend::SyntheticConfig c; c.node_count = 2; c.processors_per_node = 8; return c; }());
    rt->discover();
    numafabric::reservation::ReserveRequest rr;
    rr.node = rt->topology().nodes[0].id;
    rr.bytes = numafabric::Bytes::from(16u*1024u*1024u);
    rr.cpu_slots = 2;
    auto res = rt->reserve(rr);
    std::printf("reserved id=%llu gen=%llu bytes=%llu total=%llu\n",
                res.id.value(), res.generation.value(), (unsigned long long)rr.bytes.value(),
                (unsigned long long)rt->reserved_bytes());
    rt->release_reservation(res.id);
    std::printf("released; reserved_bytes=%llu clean=%d\n", (unsigned long long)rt->reserved_bytes(),
                rt->reservations_clean() ? 1 : 0);
    return 0;
}
