// Example 3: allocate governed host memory with an explicit preferred node.
#include "numafabric/numafabric.hpp"
#include <cstdio>
int main() {
    auto rt = numafabric::Runtime::create_windows();
    rt->discover();
    const auto node = rt->topology().nodes.front().id;
    auto region = rt->allocate_on_node(numafabric::Bytes::from(32u*1024u*1024u), node);
    std::printf("requested_bytes=%llu granted=%llu mode=%s actual_node=%llu locality_known=%d state=%s\n",
                (unsigned long long)region.requested_bytes.value(), (unsigned long long)region.granted_bytes.value(),
                numafabric::to_string(region.mode).data(), region.actual_node.value(),
                region.locality_known ? 1 : 0, numafabric::to_string(region.state).data());
    rt->release(region.id);
    return 0;
}
