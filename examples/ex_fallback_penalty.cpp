// Example 9: preferred-node fallback with an explicit locality penalty.
#include "numafabric/numafabric.hpp"
#include <cstdio>
int main() {
    auto rt = numafabric::Runtime::create_synthetic([] {
        numafabric::backend::SyntheticConfig c; c.node_count = 2; c.processors_per_node = 8; return c; }());
    rt->discover();
    numafabric::placement::PlacementRequest p;
    p.preferred_node = rt->topology().nodes[1].id;
    p.memory_owner_node = rt->topology().nodes[1].id;
    p.memory_footprint = numafabric::Bytes::from(32u*1024u*1024u);
    p.allow_fallback = true;
    auto d = rt->plan(p);
    std::printf("decision=%s node=%llu penalty=%llu binding=%s\n",
                numafabric::to_string(d.kind).data(), d.selected_node.value(),
                (unsigned long long)d.expected_penalty.value(), d.binding_constraint.c_str());
    return 0;
}
