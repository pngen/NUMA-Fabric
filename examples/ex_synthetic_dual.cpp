// Example 8: synthetic dual-NUMA placement (multi-socket model).
#include "numafabric/numafabric.hpp"
#include <cstdio>
int main() {
    auto rt = numafabric::Runtime::create_synthetic([] {
        numafabric::backend::SyntheticConfig c; c.node_count = 2; c.processors_per_node = 8; return c; }());
    rt->discover();
    std::printf("%s", rt->summary_text().c_str());
    numafabric::placement::PlacementRequest p;
    p.memory_footprint = numafabric::Bytes::from(32u*1024u*1024u);
    auto d = rt->plan(p);
    std::printf("synthetic decision=%s node=%llu\n", numafabric::to_string(d.kind).data(), d.selected_node.value());
    return 0;
}
