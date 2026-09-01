// Example 13: topology generation rollover invalidates prior placement.
#include "numafabric/numafabric.hpp"
#include <cstdio>
int main() {
    auto rt = numafabric::Runtime::create_synthetic([] {
        numafabric::backend::SyntheticConfig c; c.node_count = 2; c.processors_per_node = 8; return c; }());
    rt->discover();
    numafabric::placement::PlacementRequest p;
    p.memory_footprint = numafabric::Bytes::from(16u*1024u*1024u);
    auto d0 = rt->plan(p);
    std::printf("before rollover: %s node=%llu\n", numafabric::to_string(d0.kind).data(), d0.selected_node.value());
    rt->memory_manager().invalidate_locality_stale();
    numafabric::placement::PlacementRequest p2;
    p2.revalidation_pending = true;
    p2.memory_footprint = numafabric::Bytes::from(16u*1024u*1024u);
    auto d1 = rt->plan(p2);
    std::printf("after rollover (stale evidence): %s\n", numafabric::to_string(d1.kind).data());
    return 0;
}
