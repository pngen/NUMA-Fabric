// ============================================================================
// NUMA Fabric benchmarks.
// ============================================================================

#include "numafabric/numafabric.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace numafabric;

static double ms_per(std::uint64_t count, std::uint64_t ns) {
    return static_cast<double>(ns) / static_cast<double>(count) / 1e6;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    auto rt = Runtime::create_synthetic([] {
        backend::SyntheticConfig c; c.node_count = 2; c.processors_per_node = 8; return c; }());
    rt->discover();
    const std::uint64_t N = 20000;

    {
        const auto t0 = Clock::now_ns();
        std::uint64_t acc = 0;
        for (std::uint64_t i = 0; i < N; ++i) { auto s = rt->snapshot(); acc += s.digest; }
        const auto t1 = Clock::now_ns();
        std::printf("snapshot_creation      %10.3f ms/op  (consumed=%llu)\n", ms_per(N, t1 - t0), (unsigned long long)acc);
    }
    {
        const auto t0 = Clock::now_ns();
        std::size_t acc = 0;
        for (std::uint64_t i = 0; i < N; ++i) { acc += rt->topology().numa_node_count(); }
        const auto t1 = Clock::now_ns();
        std::printf("node_membership_lookup %10.3f ms/op  (consumed=%zu)\n", ms_per(N, t1 - t0), acc);
    }
    {
        const auto t0 = Clock::now_ns();
        std::uint64_t acc = 0;
        for (std::uint64_t i = 0; i < N; ++i) {
            placement::PlacementRequest p;
            p.memory_footprint = Bytes::from(8u * 1024u * 1024u);
            auto d = rt->plan(p);
            acc += d.selected_node.value();
        }
        const auto t1 = Clock::now_ns();
        std::printf("placement_evaluation    %10.3f ms/op  (consumed=%llu)\n", ms_per(N, t1 - t0), (unsigned long long)acc);
    }
    {
        const auto t0 = Clock::now_ns();
        std::uint64_t acc = 0;
        for (std::uint64_t i = 0; i < N; ++i) {
            reservation::ReserveRequest rr;
            rr.node = NumaNodeId::from(1 + (i % 2));
            rr.bytes = Bytes::from(64u * 1024u);
            auto r = rt->reserve(rr);
            acc += r.id.value();
            rt->release_reservation(r.id);
        }
        const auto t1 = Clock::now_ns();
        std::printf("reserve+release         %10.3f ms/op  (consumed=%llu)\n", ms_per(N, t1 - t0), (unsigned long long)acc);
    }
    {
        persist::PersistedState st;
        persist::PersistedWorker pw;
        pw.id = WorkerId::from(1); pw.boot = WorkerBootId::from(1); pw.alive = true;
        for (int i = 0; i < 64; ++i) st.workers.push_back(pw);
        std::vector<std::uint8_t> blob;
        const auto t0 = Clock::now_ns();
        for (std::uint64_t i = 0; i < N; ++i) { blob = persist::serialize(st); }
        const auto t1 = Clock::now_ns();
        std::uint64_t acc = 0;
        const auto t2 = Clock::now_ns();
        for (std::uint64_t i = 0; i < N; ++i) { auto b = persist::deserialize(blob); acc += b.workers.size(); }
        const auto t3 = Clock::now_ns();
        std::printf("serialize               %10.3f ms/op  (blob=%zu)\n", ms_per(N, t1 - t0), blob.size());
        std::printf("deserialize             %10.3f ms/op  (consumed=%llu)\n", ms_per(N, t3 - t2), (unsigned long long)acc);
    }
    {
        auto rw = Runtime::create_windows();
        rw->discover();
        const auto node = rw->topology().nodes.front().id;
        const std::uint64_t M = 2000;
        const auto t0 = Clock::now_ns();
        std::uint64_t acc = 0;
        for (std::uint64_t i = 0; i < M; ++i) {
            auto region = rw->allocate_on_node(Bytes::from(64u * 1024u), node);
            acc += static_cast<std::uint64_t>(region.granted_bytes.value());
            rw->release(region.id);
        }
        const auto t1 = Clock::now_ns();
        std::printf("os_alloc+release        %10.3f ms/op  (consumed=%llu)\n", ms_per(M, t1 - t0), (unsigned long long)acc);
    }
    std::printf("units: ms/op (wall clock). In-memory decision ops, OS allocation and persistence costs are measured on this host.\n");
    return 0;
}
