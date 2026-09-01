// ============================================================================
// Synthetic NUMA backend.
//
// Deterministically models multi-socket / multi-NUMA / multi-accelerator
// systems so cross-node placement, migration, distance and policy paths can be
// exercised even when the physical validation host exposes a single node. It
// runs the same production decision paths as the Windows backend; provenance is
// SYNTHETIC and is never presented as physical validation.
// ============================================================================

#include "numafabric/backend/backend.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace numafabric {
namespace backend {

LocalityCost SyntheticConfig::distance(NumaNodeId a, NumaNodeId b) const {
    if (a == b) return LocalityCost::zero();
    if (!distances.empty()) {
        const auto an = a.value();
        const auto bn = b.value();
        if (an <= distances.size() && bn <= distances.size()) {
            return distances[an - 1][bn - 1];
        }
    }
    return LocalityCost::from(100);
}

namespace {

constexpr std::uint64_t kDefaultNodeCapacity = 16ULL * 1024 * 1024 * 1024; // 16 GiB

class SyntheticBackend final : public Backend {
public:
    explicit SyntheticBackend(SyntheticConfig cfg)
        : cfg_(std::move(cfg)), next_region_(1) {}

    BackendKind kind() const noexcept override { return BackendKind::Synthetic; }
    std::string kind_name() const override { return "synthetic"; }
    std::string describe() const override {
        return "Synthetic backend (deterministic " + std::to_string(cfg_.node_count) +
               "-node model; provenance=SYNTHETIC)";
    }

    topo::HostTopology discover_host() override {
        topo::HostTopology host;
        host.host_id = HostId::from(1);
        host.generation = HostGeneration::from(1);
        host.system_page_size = PageSize::from(4096);
        host.allocation_granularity = Alignment::from(4096);
        host.numa_node_count_certain = true;
        host.provenance = Provenance::synthetic("synthetic multi-node model");

        const auto per_node = cfg_.processors_per_node;
        const auto group_cap = std::max<std::uint32_t>(1, cfg_.group_capacity);
        // Nodes (ids 1..N).
        for (std::uint32_t n = 0; n < cfg_.node_count; ++n) {
            topo::NumaNode node;
            node.id = NumaNodeId::from(static_cast<std::uint64_t>(n) + 1);
            node.generation = NumaNodeGeneration::initial();
            node.provenance = Provenance::synthetic("synthetic node " + std::to_string(n + 1));
            node.page_size = PageSize::from(4096);
            node.page_size_known = true;
            if (cfg_.node_capacities.empty()) {
                node.memory_capacity = Capacity::from(kDefaultNodeCapacity);
            } else if (n < cfg_.node_capacities.size()) {
                node.memory_capacity = cfg_.node_capacities[n];
            } else {
                node.memory_capacity = Capacity::from(kDefaultNodeCapacity);
            }
            // Synthetic "available" = capacity (0 used at discovery).
            node.free_memory = AvailableMemory::from(node.memory_capacity.value());
            node.memory_info_available = cfg_.expose_memory;
            host.nodes.push_back(std::move(node));
        }

        // Processor groups + processors.
        // Group index = global_processor_index / group_cap (deterministic).
        std::map<std::uint32_t, std::vector<ProcessorId>> group_register;
        for (std::uint32_t n = 0; n < cfg_.node_count; ++n) {
            for (std::uint32_t lp = 0; lp < per_node; ++lp) {
                const auto global = static_cast<std::uint64_t>(n) * per_node + lp;
                const auto group_idx = static_cast<std::uint32_t>(global / group_cap);
                const auto in_group = static_cast<std::uint32_t>(global % group_cap);
                topo::Processor p;
                p.id = ProcessorId::from(static_cast<std::uint64_t>(global) + 1);
                p.group = ProcessorGroupId::from(static_cast<std::uint64_t>(group_idx) + 1);
                p.index_in_group = LogicalProcessorIndex::from(in_group);
                p.node = NumaNodeId::from(static_cast<std::uint64_t>(n) + 1);
                p.online = true;
                host.processors_.push_back(std::move(p));
                group_register[group_idx].push_back(p.id);
            }
        }

        host.sort_canonical();

        // Geometry to know the group order: group ids correspond to group_idx+1.
        for (auto& [idx, pids] : group_register) {
            topo::ProcessorGroupInfo gi;
            gi.index = ProcessorGroupIndex::from(static_cast<std::uint16_t>(idx + 1));
            gi.id = ProcessorGroupId::from(static_cast<std::uint64_t>(idx) + 1);
            gi.processors = std::move(pids);
            host.groups.push_back(std::move(gi));
        }
        // Re-sort groups by id (sort_canonical sorts groups by id already, but
        // rebuild membership to include group list).
        std::sort(host.groups.begin(), host.groups.end(),
                  [](const topo::ProcessorGroupInfo& a, const topo::ProcessorGroupInfo& b) { return a.id < b.id; });

        host.validate();
        return host;
    }

    AllocationResult allocate(const AllocationSpec& spec) override {
        spec.validate();
        // Determine target node and whether fallback is permitted.
        NumaNodeId target = NumaNodeId::invalid();
        bool allow_fallback = false;
        switch (spec.mode) {
            case PlacementMode::RequiredNode:
                target = spec.required_node; allow_fallback = false; break;
            case PlacementMode::PreferredNode:
                target = spec.preferred_node; allow_fallback = true; break;
            case PlacementMode::Local:
                target = NumaNodeId::from(static_cast<std::uint64_t>(cfg_.local_thread_node) + 1);
                allow_fallback = true; break;
            case PlacementMode::Interleaved:
                target = NumaNodeId::invalid(); allow_fallback = false; break;
            case PlacementMode::Any:
            case PlacementMode::Synthetic:
            default:
                target = NumaNodeId::invalid(); allow_fallback = true; break;
        }

        // Determine capacity.
        if (spec.mode == PlacementMode::Interleaved) {
            return allocate_interleaved(spec);
        }

        NumaNodeId chosen = choose_node_for(spec, target, allow_fallback);
        if (!chosen.is_valid()) {
            throw BackendError("no synthetic node has capacity for this allocation");
        }
        const auto bytes = spec.bytes.value();
        const auto remaining = capacity_for(chosen) - used_for(chosen);
        if (bytes > remaining) {
            throw BackendError("required synthetic node capacity exhausted");
        }

        AllocationResult r;
        r.region_id = MemoryRegionId::from(next_region_++);
        r.kind = AllocationKind::Synthetic;
        r.requested_mode = spec.mode;
        r.granted_bytes = spec.bytes;
        r.ptr = nullptr;
        r.ptr_valid = false;          // synthetic memory is never dereferenced
        r.touched = false;
        r.locality_known = true;      // deterministic within the synthetic model
        r.observed_node = chosen;
        // record usage & node for later free.
        region_node_[r.region_id] = chosen;
        used_[chosen] += bytes;
        return r;
    }

    void free(const AllocationResult& alloc) override {
        if (!alloc.region_id.is_valid()) return;
        auto it = region_node_.find(alloc.region_id);
        if (it == region_node_.end()) return;
        const auto node = it->second;
        const auto bytes = alloc.granted_bytes.value();
        auto uit = used_.find(node);
        if (uit != used_.end()) {
            if (bytes >= uit->second) { uit->second = 0; } else { uit->second -= bytes; }
        }
        region_node_.erase(alloc.region_id);
    }

    std::optional<NumaNodeId> probe_allocation_node(const AllocationResult& alloc) override {
        auto it = region_node_.find(alloc.region_id);
        if (it != region_node_.end()) return it->second;
        return alloc.observed_node;
    }

    affinity::AffinitySet current_thread_affinity() override {
        affinity::AffinitySet s;
        // Synthetic thread belongs to the configured local node's first processor.
        const auto node = cfg_.local_thread_node;
        s.add_processor(ProcessorGroupId::from(1), 0);
        (void)node;
        return s;
    }

    bool apply_thread_affinity(const affinity::AffinitySet& set) override {
        // Synthetic: no OS thread is actually moved.
        return !set.empty();
    }

    std::vector<accel::AcceleratorInfo> enumerate_accelerators() override {
        std::vector<accel::AcceleratorInfo> out;
        for (std::size_t i = 0; i < cfg_.accelerators.size(); ++i) {
            const auto& ap = cfg_.accelerators[i];
            accel::AcceleratorInfo info;
            info.id = AcceleratorId::from(static_cast<std::uint64_t>(i) + 1);
            info.generation = AcceleratorGeneration::initial();
            info.enumerated = false;
            info.provenance = Provenance::synthetic("synthetic accelerator " + std::to_string(i + 1));
            info.device.vendor = "Synthetic";
            info.device.name = "SyntheticAccelerator" + std::to_string(i + 1);
            info.device.driver_api_ordinal = -1;
            info.locality.locality = ap.locality;
            info.locality.accelerator = info.id;
            info.locality.local_node = ap.node;
            if (ap.locality == LocalityClass::SameNumaNode) { info.locality.node_is_certain = true; }
            out.push_back(std::move(info));
        }
        return out;
    }

private:
    SyntheticConfig cfg_;
    std::uint64_t next_region_;
    std::map<MemoryRegionId, NumaNodeId> region_node_;
    std::map<NumaNodeId, std::uint64_t> used_;

    std::uint64_t capacity_for(NumaNodeId n) const {
        if (cfg_.node_capacities.empty()) return kDefaultNodeCapacity;
        const auto idx = n.value() - 1;
        if (idx < cfg_.node_capacities.size()) return cfg_.node_capacities[idx].value();
        return kDefaultNodeCapacity;
    }
    std::uint64_t used_for(NumaNodeId n) const {
        auto it = used_.find(n);
        return it == used_.end() ? 0 : it->second;
    }
    NumaNodeId choose_node_for(const AllocationSpec& spec, NumaNodeId target, bool allow_fallback) {
        // If target is valid and has capacity, prefer it.
        if (target.is_valid()) {
            const auto bytes = spec.bytes.value();
            if (capacity_for(target) - used_for(target) >= bytes) return target;
            if (!allow_fallback) return NumaNodeId::invalid();
        }
        // fallback: pick any node with capacity; deterministic order (lowest id).
        for (std::uint32_t i = 0; i < cfg_.node_count; ++i) {
            const auto n = NumaNodeId::from(static_cast<std::uint64_t>(i) + 1);
            const auto bytes = spec.bytes.value();
            if (capacity_for(n) - used_for(n) >= bytes) return n;
        }
        return NumaNodeId::invalid();
    }
    AllocationResult allocate_interleaved(const AllocationSpec& spec) {
        // Round-robin across interleave_nodes (or all nodes).
        std::vector<NumaNodeId> nodes = spec.interleave_nodes;
        if (nodes.empty()) {
            for (std::uint32_t i = 0; i < cfg_.node_count; ++i) nodes.push_back(NumaNodeId::from(static_cast<std::uint64_t>(i) + 1));
        }
        const auto chunk = std::max<std::uint64_t>(1, spec.bytes.value() / static_cast<std::uint64_t>(nodes.size()));
        AllocationResult r;
        r.region_id = MemoryRegionId::from(next_region_++);
        r.kind = AllocationKind::Synthetic;
        r.requested_mode = spec.mode;
        r.granted_bytes = spec.bytes;
        r.ptr = nullptr; r.ptr_valid = false;
        r.locality_known = true;
        r.observed_node = nodes.empty() ? NumaNodeId::invalid() : nodes.front();
        for (auto& n : nodes) { used_[n] += chunk; }
        region_node_[r.region_id] = r.observed_node;
        return r;
    }
};

} // namespace

std::unique_ptr<Backend> make_synthetic_backend(const SyntheticConfig& cfg) {
    return std::make_unique<SyntheticBackend>(cfg);
}

} // namespace backend
} // namespace numafabric
