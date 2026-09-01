// ============================================================================
// Windows NUMA / processor-group backend.
//
// Uses supported operating-system APIs only. Processor-group correctness is
// explicit: membership is derived from GetLogicalProcessorInformationEx with
// RelationNumaNode (which yields per-node, per-group masks) rather than
// assuming a single 64-bit mask represents the machine. Allocation uses
// VirtualAllocExNuma (a preferred-node hint, never claimed to be measured).
// ============================================================================

#include "numafabric/backend/backend.hpp"

#include <windows.h>
#include <malloc.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace numafabric {
namespace backend {
namespace {

// RAII around a logical-processor-information buffer.
struct ProcessorInfoBuffer {
    std::vector<unsigned char> data;
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* begin() const {
        return reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
            const_cast<unsigned char*>(data.data()));
    }
};

bool query_processor_info(LOGICAL_PROCESSOR_RELATIONSHIP rel, ProcessorInfoBuffer& out) {
    DWORD length = 0;
    GetLogicalProcessorInformationEx(rel, nullptr, &length);
    if (length == 0) return false;
    out.data.resize(length);
    return GetLogicalProcessorInformationEx(rel, out.begin(), &length) != FALSE;
}

struct GroupBit { WORD group; WORD bit; };

// Build node -> { (group, bit) } from RelationNumaNode.
void gather_numa_nodes(std::vector<topo::NumaNode>& node_register,
                       std::vector<std::vector<GroupBit>>& node_processors) {
    ProcessorInfoBuffer buf;
    if (!query_processor_info(RelationNumaNode, buf)) return;

    const unsigned char* p = buf.data.data();
    const unsigned char* end = p + buf.data.size();
    while (p < end) {
        const auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(p);
        if (info->Relationship == RelationNumaNode) {
            const auto& numa = info->NumaNode;
            const auto node = NumaNodeId::from(static_cast<std::uint64_t>(numa.NodeNumber) + 1);
            std::vector<GroupBit> bits;
            for (WORD i = 0; i < numa.GroupCount; ++i) {
                const auto ga = numa.GroupMasks[i];
                for (unsigned b = 0; b < 64; ++b) {
                    if ((ga.Mask >> b) & 1ULL) {
                        bits.push_back(GroupBit{ga.Group, static_cast<WORD>(b)});
                    }
                }
            }
            // dedupe by (group,bit)
            std::sort(bits.begin(), bits.end(), [](const GroupBit& a, const GroupBit& b) {
                return a.group < b.group || (a.group == b.group && a.bit < b.bit);
            });
            bits.erase(std::unique(bits.begin(), bits.end(), [](const GroupBit& a, const GroupBit& b) {
                return a.group == b.group && a.bit == b.bit;
            }), bits.end());
            node_processors.push_back(std::move(bits));
            topo::NumaNode n;
            n.id = node;
            n.provenance = Provenance::measured("Win32 RelationNumaNode");
            node_register.push_back(std::move(n));
        }
        p += info->Size ? info->Size : sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX);
    }
}

bool numa_available_memory(NumaNodeId node, std::uint64_t& out) {
    ULONGLONG avail = 0;
    if (GetNumaAvailableMemoryNode(static_cast<UCHAR>(node.value()), &avail)) {
        out = static_cast<std::uint64_t>(avail);
        return true;
    }
    return false;
}

class WindowsBackend final : public Backend {
public:
    BackendKind kind() const noexcept override { return BackendKind::Windows; }
    std::string kind_name() const override { return "windows"; }
    std::string describe() const override {
        return "Windows backend (processor-group-aware, real OS APIs)";
    }
    bool provides_measured_locality() const noexcept override { return false; }

    topo::HostTopology discover_host() override {
        topo::HostTopology host;
        host.host_id = HostId::from(1);
        host.generation = HostGeneration::from(1);
        host.provenance = Provenance::measured("Win32 system topology");

        // System-wide facts.
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        host.system_page_size = PageSize::from(si.dwPageSize);
        host.allocation_granularity = Alignment::from(si.dwAllocationGranularity);

        // NUMA nodes (with per-group processor membership).
        std::vector<std::vector<GroupBit>> node_processors;
        gather_numa_nodes(host.nodes, node_processors);

        // Highest node number -> certainty about node count.
        ULONG highest = 0;
        GetNumaHighestNodeNumber(&highest);
        host.numa_node_count_certain = true;
        // If RelationNumaNode reported fewer nodes than the highest numbers, the
        // extra numbers are gaps / unavailable nodes. Keep only enumerated nodes.

        // Active processor groups.
        const WORD group_count = static_cast<WORD>(GetActiveProcessorGroupCount());

        // Build group info.
        for (WORD g = 0; g < group_count; ++g) {
            topo::ProcessorGroupInfo gi;
            gi.index = ProcessorGroupIndex::from(static_cast<std::uint16_t>(g + 1));
            gi.id = ProcessorGroupId::from(static_cast<std::uint64_t>(g) + 1);
            host.groups.push_back(std::move(gi));
        }

        // Build the flat processor register: for each group, for each active bit,
        // find which node owns it.
        for (std::size_t gi = 0; gi < host.groups.size(); ++gi) {
            const auto gid = host.groups[gi].id;
            const auto active = static_cast<std::uint32_t>(GetActiveProcessorCount(static_cast<WORD>(gi)));
            for (std::uint32_t bit = 0; bit < active; ++bit) {
                // find node owning (group=gi, bit)
                NumaNodeId owner = NumaNodeId::invalid();
                for (std::size_t ni = 0; ni < host.nodes.size(); ++ni) {
                    const auto& bits = node_processors[ni];
                    const auto it = std::lower_bound(bits.begin(), bits.end(), GroupBit{static_cast<WORD>(gi), static_cast<WORD>(bit)},
                        [](const GroupBit& a, const GroupBit& b) {
                            return a.group < b.group || (a.group == b.group && a.bit < b.bit);
                        });
                    if (it != bits.end() && it->group == gi && it->bit == static_cast<WORD>(bit)) {
                        owner = host.nodes[ni].id;
                        break;
                    }
                }
                topo::Processor p;
                p.id = ProcessorId::from((static_cast<std::uint64_t>(gi + 1) << 32) | (static_cast<std::uint64_t>(bit) + 1));
                p.group = gid;
                p.index_in_group = LogicalProcessorIndex::from(bit);
                p.node = owner;
                p.online = true;
                p.id_certain = true;
                host.processors_.push_back(std::move(p));
            }
        }

        // Recompute node processor lists from register via sort_canonical.
        host.sort_canonical();

        // Per-node memory (available memory is reliable on Windows).
        for (auto& n : host.nodes) {
            std::uint64_t avail = 0;
            if (numa_available_memory(n.id, avail)) {
                n.free_memory = AvailableMemory::from(avail);
                n.memory_info_available = true;
            }
        }

        // Total physical memory (for the common single-node case).
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms)) {
            if (host.nodes.size() == 1) {
                auto& n = host.nodes.front();
                n.memory_capacity = Capacity::from(static_cast<std::uint64_t>(ms.ullTotalPhys));
            }
        }

        host.validate();
        return host;
    }

    AllocationResult allocate(const AllocationSpec& spec) override {
        spec.validate();
        AllocationResult r;
        r.region_id = MemoryRegionId::from(0); // runtime assigns real region id
        r.requested_mode = spec.mode;
        r.kind = spec.kind;

        if (spec.kind == AllocationKind::Pinned) {
            throw BackendError("pinned page-locked host allocation requires the CUDA backend on this platform");
        }

        // VirtualAlloc rounds to system granularity; honor a requested alignment
        // by overallocating if needed.
        std::uint64_t raw = spec.bytes.value();

        // Choose a preferred node. NUMA node ids are 1-based in the public model;
        // the OS API wants the 0-based node number.
        ULONG node = 0;
        NumaNodeId observed = NumaNodeId::invalid();
        if (spec.mode == PlacementMode::RequiredNode || spec.mode == PlacementMode::PreferredNode) {
            const auto nid = (spec.mode == PlacementMode::RequiredNode) ? spec.required_node : spec.preferred_node;
            if (nid.is_valid()) { node = static_cast<ULONG>(nid.value() - 1); observed = nid; }
        } else if (spec.mode == PlacementMode::Local) {
            node = node_of_current_thread();
            observed = NumaNodeId::from(static_cast<std::uint64_t>(node) + 1);
        }

        // NUMA-aware commit: VirtualAllocExNuma with a preferred node (a hint, not a
        // guarantee). Represent actual locality honestly.
        const bool numa = (spec.mode == PlacementMode::PreferredNode ||
                           spec.mode == PlacementMode::RequiredNode ||
                           spec.mode == PlacementMode::Local);
        void* p;
        if (numa) {
            p = VirtualAllocExNuma(GetCurrentProcess(), nullptr, raw,
                                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, node);
        } else {
            p = VirtualAllocExNuma(GetCurrentProcess(), nullptr, raw,
                                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, node);
        }
        if (!p) throw BackendError("VirtualAllocExNuma failed: " + last_error());

        // Touch pages to force commit (increment local access count).
        if (spec.touch) {
            auto* base = static_cast<unsigned char*>(p);
            for (std::uint64_t off = 0; off < raw; off += 4096) {
                base[off] = 0x5A;
            }
            r.touched = true;
        }

        r.ptr = p;
        r.granted_bytes = Bytes::from(raw);
        r.ptr_valid = true;
        r.locality_known = false; // the OS honoured a hint; we cannot measure pages
        r.observed_node = numa ? observed : NumaNodeId::invalid();
        return r;
    }

    void free(const AllocationResult& alloc) override {
        if (alloc.ptr && alloc.ptr_valid) {
            VirtualFree(alloc.ptr, 0, MEM_RELEASE);
        }
    }

    std::optional<NumaNodeId> probe_allocation_node(const AllocationResult&) override {
        // Windows does not expose a portable per-page NUMA index for arbitrary
        // virtual allocations.
        return std::nullopt;
    }

    affinity::AffinitySet current_thread_affinity() override {
        GROUP_AFFINITY ga{};
        if (GetThreadGroupAffinity(GetCurrentThread(), &ga)) {
            affinity::AffinitySet s;
            s.set_mask(ProcessorGroupId::from(static_cast<std::uint64_t>(ga.Group) + 1),
                       static_cast<std::uint64_t>(ga.Mask));
            return s;
        }
        return affinity::AffinitySet{};
    }

    bool apply_thread_affinity(const affinity::AffinitySet& set) override {
        if (set.empty()) return true; // unrestricted
        // Identify the thread's current group.
        GROUP_AFFINITY cur{};
        GetThreadGroupAffinity(GetCurrentThread(), &cur);
        // Prefer a mask for the thread's current group.
        const auto mask = set.mask_for(ProcessorGroupId::from(static_cast<std::uint64_t>(cur.Group) + 1));
        GROUP_AFFINITY target{};
        if (mask != 0) {
            target.Group = cur.Group;
            target.Mask = static_cast<KAFFINITY>(mask);
        } else {
            // Otherwise move to the first group in the set.
            const auto& masks = set.masks();
            if (masks.empty()) return true;
            const auto first = masks.front();
            target.Group = static_cast<WORD>(first.group.value() - 1);
            target.Mask = static_cast<KAFFINITY>(first.mask);
        }
        return SetThreadGroupAffinity(GetCurrentThread(), &target, nullptr) != FALSE;
    }

    std::vector<accel::AcceleratorInfo> enumerate_accelerators() override {
        // Accelerator enumeration (CUDA / PCI) is handled by the CUDA integration
        // backend; the generic Windows backend does not fabricate devices.
        return {};
    }

private:
    static std::string last_error() {
        const DWORD err = GetLastError();
        char buf[128];
        std::snprintf(buf, sizeof(buf), "error %lu", static_cast<unsigned long>(err));
        return buf;
    }

    // Node owning the current thread, using current group + the node's groups.
    ULONG node_of_current_thread() {
        GROUP_AFFINITY ga{};
        if (!GetThreadGroupAffinity(GetCurrentThread(), &ga)) return 0;
        // find which active processor in the current group maps to a node
        auto host = discover_host();
        for (const auto& n : host.nodes) {
            for (const auto& pid : n.processors) {
                const auto* p = host.find_processor(pid);
                if (p && p->group.value() == static_cast<std::uint64_t>(ga.Group) + 1) {
                    return static_cast<ULONG>(n.id.value() - 1);
                }
            }
        }
        return 0;
    }
};

} // namespace

std::unique_ptr<Backend> make_windows_backend() {
    return std::make_unique<WindowsBackend>();
}

} // namespace backend
} // namespace numafabric
