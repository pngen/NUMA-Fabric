#pragma once
// ============================================================================
// NUMA Fabric - strongly typed identifiers and generations.
//
// Stable identity and mutable generation are separated by construction:
//   - Id<Tag>  : a stable, unique identity that never changes for a live entity.
//   - Generation<Tag> : a monotonic counter that advances whenever the entity
//     is replaced, rebound, reissued, or reincarnated.
//
// Two entities with different tags can never be accidentally compared, mixed or
// hashed together. A restarted worker gets a fresh WorkerBootId. A placement
// replacement advances PlacementGeneration rather than silently mutating old
// authority. All public aliases live directly in namespace numafabric so model
// headers can use them unambiguously.
// ============================================================================

#include <cstdint>
#include <functional>
#include <ostream>
#include <stdexcept>
#include <string>

namespace numafabric {
namespace ids {

class InvalidIdError final : public std::exception {
public:
    const char* what() const noexcept override { return "invalid identifier"; }
};

// ---------------------------------------------------------------------------
// Id<Tag>
// ---------------------------------------------------------------------------
template <typename Tag>
class Id {
public:
    using value_type = std::uint64_t;
    constexpr Id() noexcept = default;
    explicit constexpr Id(value_type v) noexcept : value_(v) {}
    explicit constexpr Id(int v) noexcept : value_(static_cast<value_type>(v)) {}

    static constexpr Id invalid() noexcept { return Id(); }
    static constexpr Id from(value_type v) noexcept { return Id(v); }

    [[nodiscard]] constexpr value_type value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool is_valid() const noexcept { return value_ != 0; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return is_valid(); }

    constexpr Id& operator++() noexcept { ++value_; return *this; }
    constexpr Id next() const noexcept { return Id(value_ + 1); }

    friend constexpr bool operator==(Id a, Id b) noexcept { return a.value_ == b.value_; }
    friend constexpr bool operator!=(Id a, Id b) noexcept { return a.value_ != b.value_; }
    friend constexpr bool operator<(Id a, Id b) noexcept { return a.value_ < b.value_; }
    friend constexpr bool operator<=(Id a, Id b) noexcept { return a.value_ <= b.value_; }
    friend constexpr bool operator>(Id a, Id b) noexcept { return a.value_ > b.value_; }
    friend constexpr bool operator>=(Id a, Id b) noexcept { return a.value_ >= b.value_; }

    [[nodiscard]] std::string to_string() const { return std::to_string(value_); }
    void write(std::ostream& os) const { os << value_; }

private:
    value_type value_ = 0;
};

// ---------------------------------------------------------------------------
// Generation<Tag>
// ---------------------------------------------------------------------------
template <typename Tag>
class Generation {
public:
    using value_type = std::uint64_t;
    constexpr Generation() noexcept = default;
    explicit constexpr Generation(value_type v) noexcept : value_(v) {}

    static constexpr Generation initial() noexcept { return Generation(0); }
    static constexpr Generation from(value_type v) noexcept { return Generation(v); }
    static constexpr Generation invalid() noexcept { return Generation(); }

    [[nodiscard]] constexpr value_type value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool is_valid() const noexcept { return value_ != 0; }
    constexpr explicit operator bool() const noexcept { return is_valid(); }

    constexpr Generation next() const {
        if (value_ == UINT64_MAX) { throw InvalidIdError{}; }
        return Generation(value_ + 1);
    }
    Generation& operator++() { value_ = next().value_; return *this; }

    friend constexpr bool operator==(Generation a, Generation b) noexcept { return a.value_ == b.value_; }
    friend constexpr bool operator!=(Generation a, Generation b) noexcept { return a.value_ != b.value_; }
    friend constexpr bool operator<(Generation a, Generation b) noexcept { return a.value_ < b.value_; }

    [[nodiscard]] std::string to_string() const { return std::to_string(value_); }
    void write(std::ostream& os) const { os << value_; }

private:
    value_type value_ = 0;
};

} // namespace ids

// ---------------------------------------------------------------------------
// Tag declarations (empty marker structs).
// ---------------------------------------------------------------------------
struct HostIdTag {};            struct HostGenerationTag {};
struct NumaNodeIdTag {};        struct NumaNodeGenerationTag {};
struct ProcessorIdTag {};
struct ProcessorGroupIdTag {};
struct CpuSetIdTag {};
struct MemoryRegionIdTag {};    struct MemoryGenerationTag {};
struct WorkerIdTag {};          struct WorkerBootIdTag {};
struct ProcessIdTag {};         struct ThreadIdTag {};
struct AcceleratorIdTag {};     struct AcceleratorGenerationTag {};
struct DeviceIdTag {};          struct DeviceGenerationTag {};
struct IoDeviceIdTag {};
struct PlacementIdTag {};       struct PlacementGenerationTag {};
struct BindingIdTag {};         struct BindingGenerationTag {};
struct PolicyIdTag {};          struct PolicyGenerationTag {};
struct ObservationIdTag {};     struct ObservationGenerationTag {};
struct CoordinatorEpochTag {};
struct AttemptIdTag {};
struct ReservationIdTag {};        struct ReservationGenerationTag {};
struct SnapshotIdTag {};
struct SourceIdTag {};          struct SourceGenerationTag {};

// ---------------------------------------------------------------------------
// Public aliases (in namespace numafabric).
// ---------------------------------------------------------------------------
using HostId              = ids::Id<HostIdTag>;
using HostGeneration      = ids::Generation<HostGenerationTag>;
using NumaNodeId          = ids::Id<NumaNodeIdTag>;
using NumaNodeGeneration  = ids::Generation<NumaNodeGenerationTag>;
using ProcessorId         = ids::Id<ProcessorIdTag>;
using ProcessorGroupId    = ids::Id<ProcessorGroupIdTag>;
using CpuSetId            = ids::Id<CpuSetIdTag>;
using MemoryRegionId      = ids::Id<MemoryRegionIdTag>;
using MemoryGeneration    = ids::Generation<MemoryGenerationTag>;
using WorkerId            = ids::Id<WorkerIdTag>;
using WorkerBootId        = ids::Id<WorkerBootIdTag>;
using ProcessId           = ids::Id<ProcessIdTag>;
using ThreadId            = ids::Id<ThreadIdTag>;
using AcceleratorId       = ids::Id<AcceleratorIdTag>;
using AcceleratorGeneration = ids::Generation<AcceleratorGenerationTag>;
using DeviceId            = ids::Id<DeviceIdTag>;
using DeviceGeneration    = ids::Generation<DeviceGenerationTag>;
using IoDeviceId          = ids::Id<IoDeviceIdTag>;
using PlacementId         = ids::Id<PlacementIdTag>;
using PlacementGeneration = ids::Generation<PlacementGenerationTag>;
using BindingId           = ids::Id<BindingIdTag>;
using BindingGeneration   = ids::Generation<BindingGenerationTag>;
using PolicyId            = ids::Id<PolicyIdTag>;
using PolicyGeneration    = ids::Generation<PolicyGenerationTag>;
using ObservationId       = ids::Id<ObservationIdTag>;
using ObservationGeneration = ids::Generation<ObservationGenerationTag>;
using CoordinatorEpoch    = ids::Generation<CoordinatorEpochTag>;
using AttemptId           = ids::Id<AttemptIdTag>;
using ReservationId       = ids::Id<ReservationIdTag>;
using ReservationGeneration = ids::Generation<ReservationGenerationTag>;
using SnapshotId          = ids::Id<SnapshotIdTag>;
using SourceId            = ids::Id<SourceIdTag>;
using SourceGeneration    = ids::Generation<SourceGenerationTag>;

template <typename Tag>
std::ostream& operator<<(std::ostream& os, const ids::Id<Tag>& id) { id.write(os); return os; }
template <typename Tag>
std::ostream& operator<<(std::ostream& os, const ids::Generation<Tag>& g) { g.write(os); return os; }

} // namespace numafabric
