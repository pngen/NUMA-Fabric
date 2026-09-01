#pragma once
// ============================================================================
// NUMA Fabric - group-aware processor affinity set.
//
// A processor-group-aware affinity set is an ordered collection of per-group
// 64-bit masks. A single mask is NEVER assumed to represent the whole machine:
// on Windows a processor group is a distinct affinity domain and group-aware
// affinity must be explicit and lossless.
// ============================================================================

#include "numafabric/core/ids.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace numafabric {
namespace affinity {

class InvalidAffinityError final : public std::exception {
public:
    const char* what() const noexcept override { return "invalid or impossible affinity mask"; }
};

struct GroupMask {
    ProcessorGroupId group = ProcessorGroupId::invalid();
    std::uint64_t mask = 0;

    bool operator==(const GroupMask& o) const noexcept { return group == o.group && mask == o.mask; }
    bool operator!=(const GroupMask& o) const noexcept { return !(*this == o); }
};

class AffinitySet {
public:
    static constexpr int max_group_mask_bits = 64;

    AffinitySet() = default;
    explicit AffinitySet(std::vector<GroupMask> m) : masks_(std::move(m)) {
        normalize();
        validate();
    }

    void set_mask(ProcessorGroupId group, std::uint64_t mask) {
        for (auto& gm : masks_) {
            if (gm.group == group) { gm.mask = mask; normalize(); validate(); return; }
        }
        masks_.push_back(GroupMask{group, mask});
        normalize();
        validate();
    }

    void add_processor(ProcessorGroupId group, std::uint32_t logical_index) {
        if (logical_index >= max_group_mask_bits) throw InvalidAffinityError{};
        for (auto& gm : masks_) {
            if (gm.group == group) { gm.mask |= (std::uint64_t(1) << logical_index); normalize(); validate(); return; }
        }
        masks_.push_back(GroupMask{group, std::uint64_t(1) << logical_index});
        normalize();
        validate();
    }

    void clear() { masks_.clear(); }

    [[nodiscard]] bool empty() const noexcept { return masks_.empty(); }
    [[nodiscard]] bool is_all() const noexcept { return empty(); } // empty mask interpreted as "all" by convention (unrestricted)

    [[nodiscard]] std::uint64_t mask_for(ProcessorGroupId group) const noexcept {
        for (const auto& gm : masks_) { if (gm.group == group) return gm.mask; }
        return 0;
    }

    [[nodiscard]] bool contains(ProcessorGroupId group, std::uint32_t logical_index) const noexcept {
        if (logical_index >= max_group_mask_bits) return false;
        const auto m = mask_for(group);
        return (m & (std::uint64_t(1) << logical_index)) != 0;
    }

    [[nodiscard]] std::size_t group_count() const noexcept { return masks_.size(); }
    [[nodiscard]] const std::vector<GroupMask>& masks() const noexcept { return masks_; }

    // Total number of bits set across all groups.
    [[nodiscard]] std::size_t count() const noexcept {
        std::size_t n = 0;
        for (const auto& gm : masks_) { n += static_cast<std::size_t>(std::popcount(gm.mask)); }
        return n;
    }

    bool operator==(const AffinitySet& o) const noexcept { return masks_ == o.masks_; }
    bool operator!=(const AffinitySet& o) const noexcept { return !(*this == o); }

    [[nodiscard]] std::string to_string() const;

private:
    std::vector<GroupMask> masks_;

    void normalize() {
        std::sort(masks_.begin(), masks_.end(),
                  [](const GroupMask& a, const GroupMask& b) { return a.group < b.group; });
        // merge duplicate groups
        std::vector<GroupMask> merged;
        for (const auto& gm : masks_) {
            if (!merged.empty() && merged.back().group == gm.group) { merged.back().mask |= gm.mask; }
            else { merged.push_back(gm); }
        }
        masks_ = std::move(merged);
    }
    void validate() const {
        for (const auto& gm : masks_) {
            if (!gm.group.is_valid()) throw InvalidAffinityError{};
        }
    }
};

inline std::string AffinitySet::to_string() const {
    std::string out;
    bool first = true;
    for (const auto& gm : masks_) {
        if (!first) out += ",";
        char buf[32];
        out += "g" + gm.group.to_string() + "=0x";
        std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(gm.mask));
        out += buf;
        first = false;
    }
    return out;
}

inline std::ostream& operator<<(std::ostream& os, const AffinitySet& a) { return os << a.to_string(); }

} // namespace affinity
} // namespace numafabric
