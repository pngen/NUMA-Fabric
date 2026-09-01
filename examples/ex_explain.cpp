// Example 6: explain a placement (inspectable cost components, constraints).
#include "numafabric/numafabric.hpp"
#include <cstdio>
int main() {
    auto rt = numafabric::Runtime::create_synthetic([] {
        numafabric::backend::SyntheticConfig c; c.node_count = 2; c.processors_per_node = 8; return c; }());
    rt->discover();
    numafabric::placement::PlacementRequest p;
    p.memory_footprint = numafabric::Bytes::from(32u*1024u*1024u);
    p.has_required_node = true;
    p.required_node = rt->topology().nodes[0].id;
    auto d = rt->plan(p);
    std::printf("%s", rt->explanation_text(d).c_str());
    return 0;
}
