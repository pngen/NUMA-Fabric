// ============================================================================
// nf - NUMA Fabric command line.
//
// Exercises real library APIs (never duplicates model logic). Default output is
// deterministic text; --json emits JSON. Reports only what the backend observed.
// ============================================================================

#include "numafabric/numafabric.hpp"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace numafabric;

static void usage() {
    std::printf("nf - NUMA Fabric\n"
                "usage: nf [--backend windows|synthetic] [--nodes N] [--procs M] [--json] <command>\n"
                "commands:\n"
                "  discover  nodes  processors  devices  summary  allocations\n"
                "  workers  placements  reservations  snapshot  diff\n"
                "  save <path>  load <path>  validate  bench  mask\n");
}

int main(int argc, char** argv) {
    std::string backend = "windows";
    std::uint32_t nodes = 2;
    std::uint32_t procs = 8;
    bool json = false;
    std::string command;
    std::string path;

    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--backend" && i + 1 < args.size()) backend = args[++i];
        else if (args[i] == "--nodes" && i + 1 < args.size()) nodes = std::stoul(args[++i]);
        else if (args[i] == "--procs" && i + 1 < args.size()) procs = std::stoul(args[++i]);
        else if (args[i] == "--json") json = true;
        else if (command.empty()) command = args[i];
        else if (path.empty()) path = args[i];
    }
    if (command.empty()) command = "summary";

    std::unique_ptr<Runtime> rt;
    try {
        if (backend == "synthetic") {
            backend::SyntheticConfig cfg;
            cfg.node_count = nodes;
            cfg.processors_per_node = procs;
            cfg.group_capacity = 64;
            rt = Runtime::create_synthetic(cfg);
        } else {
            rt = Runtime::create_windows();
        }
        rt->discover();
        rt->refresh_accelerators();
    } catch (const std::exception& e) {
        std::printf("error: %s\n", e.what());
        return 2;
    }

    try {
        if (command == "discover" || command == "summary" || command == "snapshot") {
            if (json) { std::printf("%s\n", rt->summary_json().c_str()); }
            else { std::printf("%s", rt->summary_text().c_str()); }
            if (command == "snapshot") {
                auto snap = rt->snapshot();
                std::printf("snapshot digest: %s\n", snap.digest_hex().c_str());
            }
        } else if (command == "nodes") {
            for (const auto& n : rt->topology().nodes) {
                std::printf("node %llu: processors=%zu capacity=%llu", n.id.value(), n.processor_count(), n.memory_capacity.value());
                if (n.memory_info_available) std::printf(" free=%llu", n.free_memory.value());
                std::printf(" provenance=%s\n", numafabric::to_string(n.provenance.source).data());
            }
        } else if (command == "processors") {
            for (const auto& g : rt->topology().groups) {
                std::printf("group %llu (index=%u): %zu processors\n", g.id.value(), g.index.value(), g.processor_count());
            }
            for (const auto& p : rt->topology().processors_) {
                std::printf("  processor %llu group=%llu index=%u node=%llu online=%d\n",
                            p.id.value(), p.group.value(), p.index_in_group.value(), p.node.value(), p.online ? 1 : 0);
            }
        } else if (command == "devices") {
            for (const auto& a : rt->accelerators()) {
                std::printf("accelerator %llu: vendor=%s name=%s locality=%s node=%llu certain=%d cuda=%d\n",
                            a.id.value(), a.device.vendor.c_str(), a.device.name.c_str(),
                            numafabric::to_string(a.locality.locality).data(),
                            a.locality.local_node.value(), a.locality.node_is_certain ? 1 : 0, a.is_cuda() ? 1 : 0);
            }
            if (rt->accelerators().empty()) std::printf("no accelerators enumerated by the active backend\n");
        } else if (command == "allocations") {
            const auto node = rt->topology().nodes.front().id;
            auto region = rt->allocate_on_node(Bytes::from(16 * 1024 * 1024), node);
            std::printf("allocated region %llu bytes=%llu kind=%s actual_node=%llu locality_known=%d state=%s\n",
                        region.id.value(), region.granted_bytes.value(),
                        numafabric::to_string(region.kind).data(), region.actual_node.value(),
                        region.locality_known ? 1 : 0, numafabric::to_string(region.state).data());
            rt->release(region.id);
            std::printf("released region %llu; allocated_bytes=%llu clean=%d\n",
                        region.id.value(), rt->allocated_bytes(), rt->memory_accounting_clean() ? 1 : 0);
        } else if (command == "workers") {
            auto w = rt->register_worker(WorkerId::from(1), ProcessId::from(1234));
            std::printf("registered worker %llu boot=%llu incarnation=%llu\n", w.id.value(), w.boot_id.value(), w.incarnation);
            const auto node = rt->topology().nodes.front().id;
            auto b = rt->bind_worker(w.id, w.boot_id, node, false);
            std::printf("bound worker %llu to node %llu binding=%llu gen=%llu applied=%d\n",
                        w.id.value(), node.value(), b.binding_id.value(), b.generation.value(), b.applied ? 1 : 0);
            std::printf("restart -> new boot=%llu\n", rt->restart_worker(w.id, ProcessId::from(1234)).boot_id.value());
        } else if (command == "placements") {
            placement::PlacementRequest req;
            req.memory_footprint = Bytes::from(64 * 1024 * 1024);
            req.workload_id = "nf-cli";
            if (rt->topology().nodes.size() >= 2) req.preferred_node = rt->topology().nodes[1].id;
            auto decision = rt->plan(req);
            std::printf("%s", rt->explanation_text(decision).c_str());
        } else if (command == "reservations") {
            const auto node = rt->topology().nodes.front().id;
            reservation::ReserveRequest rr;
            rr.node = node;
            rr.bytes = Bytes::from(32 * 1024 * 1024);
            rr.cpu_slots = 2;
            auto res = rt->reserve(rr);
            std::printf("reserved %llu bytes node=%llu id=%llu gen=%llu total=%llu\n",
                        rr.bytes.value(), node.value(), res.id.value(), res.generation.value(), rt->reserved_bytes());
            rt->release_reservation(res.id);
            std::printf("released; reserved_bytes=%llu clean=%d\n", rt->reserved_bytes(), rt->reservations_clean() ? 1 : 0);
        } else if (command == "diff") {
            auto a = rt->snapshot();
            auto b = rt->snapshot();
            std::printf("snapshot a digest=%s\nsnapshot b digest=%s\nidentical=%d\n",
                        a.digest_hex().c_str(), b.digest_hex().c_str(), a.fingerprints_match(b.host) ? 1 : 0);
        } else if (command == "save") {
            if (path.empty()) path = "numafabric.state";
            rt->persist(path);
            std::printf("saved authoritative state to %s\n", path.c_str());
        } else if (command == "load") {
            if (path.empty()) path = "numafabric.state";
            rt->recover(path);
            std::printf("recovered from %s; locality is marked stale until revalidated\n", path.c_str());
        } else if (command == "validate") {
            rt->topology().validate();
            std::printf("topology validated: %zu nodes, %zu groups, %zu processors\n",
                        rt->topology().numa_node_count(), rt->topology().groups.size(), rt->topology().processor_count());
        } else if (command == "bench") {
            const auto t0 = Clock::now_ns();
            for (int i = 0; i < 1000; ++i) { auto snap = rt->snapshot(); static_cast<void>(snap.digest); }
            const auto t1 = Clock::now_ns();
            std::printf("1000 snapshot creations in %llu ns (%.0f ns/op)\n", t1 - t0, static_cast<double>(t1 - t0) / 1000.0);
        } else if (command == "mask") {
            affinity::AffinitySet s;
            s.add_processor(ProcessorGroupId::from(1), 0);
            s.add_processor(ProcessorGroupId::from(1), 1);
            std::printf("affinity set: %s (count=%zu)\n", s.to_string().c_str(), s.count());
        } else if (command == "cuda-proof") {
            std::printf("error: cuda-proof requires a CUDA-enabled build\n");
        } else {
            usage();
            return 1;
        }
    } catch (const std::exception& e) {
        std::printf("error: %s\n", e.what());
        return 3;
    }
    return 0;
}
