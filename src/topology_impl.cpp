// ============================================================================
// Topology implementation: canonical ordering + digest.
// ============================================================================
#include "numafabric/topology/topology.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace numafabric {
namespace topo {

void HostTopology::sort_canonical() {
    // Sort the flat processor register by id.
    std::sort(processors_.begin(), processors_.end(),
              [](const Processor& a, const Processor& b) { return a.id < b.id; });

    // Rebuild per-node and per-group membership from the register.
    // Collect into sorted containers so membership is canonical regardless of
    // the order in which backends filled the register.
    std::map<ProcessorGroupId, std::vector<ProcessorId>> group_members;
    std::map<NumaNodeId, std::vector<ProcessorId>> node_members;
    for (const auto& p : processors_) {
        group_members[p.group].push_back(p.id);
        node_members[p.node].push_back(p.id);
    }
    for (auto& [gid, list] : group_members) { std::sort(list.begin(), list.end()); }
    for (auto& [nid, list] : node_members) { std::sort(list.begin(), list.end()); }

    std::sort(nodes.begin(), nodes.end(), [](const NumaNode& a, const NumaNode& b) { return a.id < b.id; });
    std::sort(groups.begin(), groups.end(), [](const ProcessorGroupInfo& a, const ProcessorGroupInfo& b) { return a.id < b.id; });

    for (auto& n : nodes) {
        auto it = node_members.find(n.id);
        n.processors = (it != node_members.end()) ? it->second : std::vector<ProcessorId>{};
    }
    for (auto& g : groups) {
        auto it = group_members.find(g.id);
        g.processors = (it != group_members.end()) ? it->second : std::vector<ProcessorId>{};
    }
}

SemanticDigest HostTopology::semantic_digest() const {
    SemanticDigest d;
    d.field("host_id").u64(host_id.value());
    d.field("host_gen").u64(generation.value());
    d.field("page_size").u64(system_page_size.value());
    d.field("granularity").u64(allocation_granularity.value());
    d.field("node_count_certain").boolean(numa_node_count_certain);

    // Sorted processor identities first (register already canonical).
    d.field("processors");
    d.u64(processors_.size());
    for (const auto& p : processors_) {
        d.u64(p.id.value());
        d.u64(p.group.value());
        d.u64(p.node.value());
        d.u64(p.index_in_group.value());
        d.boolean(p.online);
    }
    // Nodes in canonical order.
    d.field("nodes");
    d.u64(nodes.size());
    for (const auto& n : nodes) {
        d.u64(n.id.value());
        d.u64(n.generation.value());
        d.u64(n.memory_capacity.value());
        d.boolean(n.memory_info_available);
        d.boolean(n.page_size_known);
        d.field("");
        // node member set
        d.u64(n.processors.size());
        for (const auto& pid : n.processors) { d.u64(pid.value()); }
    }
    // Groups in canonical order.
    d.field("groups");
    d.u64(groups.size());
    for (const auto& g : groups) {
        d.u64(g.id.value());
        d.u64(g.index.value());
        d.u64(g.processors.size());
        for (const auto& pid : g.processors) { d.u64(pid.value()); }
    }
    return d;
}

std::string TopologySnapshot::digest_hex() const {
    return SemanticDigest(digest).hex();
}

void HostTopology::validate() const {
    // No processor may be claimed by two nodes.
    std::map<ProcessorId, NumaNodeId> owner;
    for (const auto& p : processors_) {
        auto [it, inserted] = owner.emplace(p.id, p.node);
        if (!inserted && it->second != p.node) {
            throw UnsupportedTopologyError("processor claimed by multiple nodes");
        }
        if (find_node(p.node) == nullptr) {
            throw UnsupportedTopologyError("processor references missing node");
        }
        if (find_group(p.group) == nullptr) {
            throw UnsupportedTopologyError("processor references missing group");
        }
    }
    // Duplicate / invalid node ids.
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (!nodes[i].id.is_valid()) { throw UnsupportedTopologyError("invalid node id"); }
        for (std::size_t j = i + 1; j < nodes.size(); ++j) {
            if (nodes[i].id == nodes[j].id) { throw UnsupportedTopologyError("duplicate node id"); }
        }
    }
}

} // namespace topo
} // namespace numafabric
