#pragma once
// ============================================================================
// NUMA Fabric - canonical topology / locality inventory.
//
// NUMA Fabric consumes (or independently derives) the relevant host-locality
// facts: which NUMA nodes exist, which processors belong to them, and where
// memory and execution can be placed. Windows processor-group correctness is
// explicit: a single 64-bit mask is NEVER assumed to represent the whole
// machine. Unavailable facts are represented as unavailable/unknown rather
// than invented.
// ============================================================================

#include "numafabric/core/digest.hpp"
#include "numafabric/core/enums.hpp"
#include "numafabric/core/ids.hpp"
#include "numafabric/core/provenance.hpp"
#include "numafabric/core/quantities.hpp"

#include <algorithm>
#include <cstdint>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace numafabric {
namespace topo {

class UnsupportedTopologyError final : public std::exception {
public:
    explicit UnsupportedTopologyError(std::string m) : message_(std::move(m)) {}
    const char* what() const noexcept override { return message_.c_str(); }
private:
    std::string message_;
};

// ---------------------------------------------------------------------------
// Processor
// ---------------------------------------------------------------------------
struct Processor {
    ProcessorId id = ProcessorId::invalid();
    ProcessorGroupId group = ProcessorGroupId::invalid();
    LogicalProcessorIndex index_in_group = LogicalProcessorIndex::zero();
    NumaNodeId node = NumaNodeId::invalid();
    bool online = false;
    bool id_certain = false;   // true when isolated from real OS enumeration

    bool operator==(const Processor& o) const {
        return id == o.id && group == o.group && index_in_group == o.index_in_group &&
               node == o.node && online == o.online;
    }
};

// ---------------------------------------------------------------------------
// ProcessorGroup
// ---------------------------------------------------------------------------
struct ProcessorGroupInfo {
    ProcessorGroupId id = ProcessorGroupId::invalid();
    ProcessorGroupIndex index = ProcessorGroupIndex::zero();
    std::vector<ProcessorId> processors; // sorted ascending by ProcessorId

    [[nodiscard]] std::size_t processor_count() const noexcept { return processors.size(); }
};

// ---------------------------------------------------------------------------
// NumaNode
// ---------------------------------------------------------------------------
struct NumaNode {
    NumaNodeId id = NumaNodeId::invalid();
    NumaNodeGeneration generation = NumaNodeGeneration::initial();
    std::vector<ProcessorId> processors; // sorted ascending
    Capacity memory_capacity = Capacity::zero();
    AvailableMemory free_memory = AvailableMemory::zero();
    bool memory_info_available = false;  // free/available memory only set when genuinely measured
    PageSize page_size = PageSize::zero();
    bool page_size_known = false;
    Provenance provenance = Provenance::unknown();

    [[nodiscard]] std::size_t processor_count() const noexcept { return processors.size(); }
    [[nodiscard]] bool contains(ProcessorId pid) const noexcept {
        return std::binary_search(processors.begin(), processors.end(), pid);
    }
    [[nodiscard]] Capacity capacity() const noexcept { return memory_capacity; }
};

// ---------------------------------------------------------------------------
// Host topology
// ---------------------------------------------------------------------------
class HostTopology {
public:
    HostTopology() = default;

    HostId host_id = HostId::invalid();
    HostGeneration generation = HostGeneration::initial();
    std::vector<ProcessorGroupInfo> groups; // ordered by group index
    std::vector<NumaNode> nodes;            // ordered by node id
    std::vector<Processor> processors_;     // flat processor register
    PageSize system_page_size = PageSize::zero();
    Alignment allocation_granularity = Alignment::zero();
    bool numa_node_count_certain = false;   // highest-node number authoritative
    Provenance provenance = Provenance::unknown();

    [[nodiscard]] bool is_single_node() const noexcept { return nodes.size() == 1; }
    [[nodiscard]] std::size_t numa_node_count() const noexcept { return nodes.size(); }
    [[nodiscard]] std::size_t processor_count() const noexcept { return processors_.size(); }

    const NumaNode* find_node(NumaNodeId id) const noexcept {
        for (const auto& n : nodes) { if (n.id == id) return &n; }
        return nullptr;
    }
    NumaNode* find_node(NumaNodeId id) noexcept {
        for (auto& n : nodes) { if (n.id == id) return &n; }
        return nullptr;
    }
    const Processor* find_processor(ProcessorId pid) const noexcept {
        for (const auto& p : processors_) { if (p.id == pid) return &p; }
        return nullptr;
    }
    const ProcessorGroupInfo* find_group(ProcessorGroupId gid) const noexcept {
        for (const auto& g : groups) { if (g.id == gid) return &g; }
        return nullptr;
    }

    // Rebuild nodes.processors / groups.processors from the flat register and
    // sort everything into canonical (deterministic) order.
    void sort_canonical();

    // Stable semantic digest: independent of discovery order, never depends on
    // process addresses or pointer identity.
    SemanticDigest semantic_digest() const;

    // Validation: a single node's membership must not be self-contradictory.
    void validate() const;
};

// ---------------------------------------------------------------------------
// TopologySnapshot - immutable semantic snapshot of the host-locality facts.
// ---------------------------------------------------------------------------
class TopologySnapshot {
public:
    SnapshotId snapshot_id = SnapshotId::invalid();
    HostTopology host;
    SemanticDigest::value_type digest = 0;
    std::uint64_t created_ms = 0;

    [[nodiscard]] bool is_single_node() const noexcept { return host.is_single_node(); }
    [[nodiscard]] std::size_t numa_node_count() const noexcept { return host.numa_node_count(); }
    [[nodiscard]] const NumaNode* find_node(NumaNodeId id) const noexcept { return host.find_node(id); }
    [[nodiscard]] SemanticDigest::value_type semantic_digest() const noexcept { return digest; }
    [[nodiscard]] bool fingerprints_match(const HostTopology& other) const noexcept {
        return digest == other.semantic_digest().value();
    }
    [[nodiscard]] std::string digest_hex() const;
};

} // namespace topo
} // namespace numafabric
