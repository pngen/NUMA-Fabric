// Example 1: discover NUMA topology using real OS APIs (Windows backend).
#include "numafabric/numafabric.hpp"
#include <cstdio>
int main() {
    auto rt = numafabric::Runtime::create_windows();
    rt->discover();
    std::printf("%s", rt->summary_text().c_str());
    std::printf("digest=%s\n", rt->snapshot().digest_hex().c_str());
    return 0;
}
