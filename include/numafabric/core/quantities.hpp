#pragma once
// ============================================================================
// NUMA Fabric - strongly typed, validated physical quantities.
//
// Every numeric quantity carries a dimension tag so bytes, pages, nanoseconds
// and locality cost can never be interchanged silently. Construction runs
// validation that rejects NaN, infinity, negative-impossible values and
// integer overflow where the underlying physical quantity cannot take it.
// ============================================================================

#include <cmath>
#include <cstdint>
#include <limits>
#include <ostream>
#include <string>
#include <type_traits>

namespace numafabric {
namespace qty {

class InvalidQuantityError final : public std::exception {
public:
    const char* what() const noexcept override { return "invalid physical quantity"; }
};

template <typename Tag, typename Rep>
class Quantity {
public:
    static_assert(std::is_integral_v<Rep> || std::is_floating_point_v<Rep>,
                  "quantity representation must be an arithmetic type");

    using value_type = Rep;
    using tag_type   = Tag;

    constexpr Quantity() noexcept = default;
    explicit Quantity(Rep v) : value_(v) { validate(); }
    explicit Quantity(int v) : value_(static_cast<Rep>(v)) { validate(); }

    static Quantity from(Rep v) { return Quantity(v); }
    static Quantity zero() noexcept { return Quantity{}; }

    [[nodiscard]] constexpr Rep value() const noexcept { return value_; }
    [[nodiscard]] constexpr explicit operator Rep() const noexcept { return value_; }
    [[nodiscard]] constexpr bool is_zero() const noexcept { return value_ == Rep{}; }

    void validate() const {
        if constexpr (std::is_floating_point_v<Rep>) {
            if (std::isnan(value_) || std::isinf(value_) || value_ < Rep{}) throw InvalidQuantityError{};
        } else {
            if (value_ < Rep{}) throw InvalidQuantityError{};
        }
    }
    [[nodiscard]] bool is_valid() const noexcept {
        if constexpr (std::is_floating_point_v<Rep>) {
            return !(std::isnan(value_) || std::isinf(value_) || value_ < Rep{});
        } else {
            return value_ >= Rep{};
        }
    }

    Quantity operator+(Quantity o) const {
        if constexpr (std::is_floating_point_v<Rep>) {
            return Quantity(value_ + o.value_);
        } else {
            if (value_ > (std::numeric_limits<Rep>::max)() - o.value_) throw InvalidQuantityError{};
            return Quantity(value_ + o.value_);
        }
    }
    Quantity operator-(Quantity o) const {
        if constexpr (std::is_floating_point_v<Rep>) {
            return Quantity(value_ - o.value_);
        } else {
            if (o.value_ > value_) throw InvalidQuantityError{};
            return Quantity(value_ - o.value_);
        }
    }
    Quantity operator*(std::uint64_t f) const {
        if constexpr (std::is_floating_point_v<Rep>) {
            return Quantity(value_ * static_cast<Rep>(f));
        } else {
            if (f != 0 && value_ > (std::numeric_limits<Rep>::max)() / f) throw InvalidQuantityError{};
            return Quantity(value_ * f);
        }
    }
    Quantity operator/(std::uint64_t d) const {
        if (d == 0) throw InvalidQuantityError{};
        return Quantity(static_cast<Rep>(value_ / static_cast<Rep>(d)));
    }
    Quantity& operator+=(Quantity o) { *this = *this + o; return *this; }
    Quantity& operator-=(Quantity o) { *this = *this - o; return *this; }

    friend constexpr bool operator<(Quantity a, Quantity b) noexcept { return a.value_ < b.value_; }
    friend constexpr bool operator<=(Quantity a, Quantity b) noexcept { return a.value_ <= b.value_; }
    friend constexpr bool operator>(Quantity a, Quantity b) noexcept { return a.value_ > b.value_; }
    friend constexpr bool operator>=(Quantity a, Quantity b) noexcept { return a.value_ >= b.value_; }
    friend constexpr bool operator==(Quantity a, Quantity b) noexcept { return a.value_ == b.value_; }
    friend constexpr bool operator!=(Quantity a, Quantity b) noexcept { return a.value_ != b.value_; }

    [[nodiscard]] std::string to_string() const {
        if constexpr (std::is_floating_point_v<Rep>) {
            std::string s = std::to_string(value_);
            auto dot = s.find('.');
            if (dot != std::string::npos) {
                auto last = s.find_last_not_of('0');
                if (last == dot) { s.erase(dot); } else { s.erase(last + 1); }
            }
            return s;
        } else {
            return std::to_string(value_);
        }
    }
    void write(std::ostream& os) const { os << value_; }

private:
    Rep value_ = Rep{};
};

} // namespace qty

// Tag declarations.
struct BytesTag {};            struct PagesTag {};
struct ProcessorCountTag {};   struct LogicalProcessorIndexTag {};
struct ProcessorGroupIndexTag {};
struct NanosecondsTag {};      struct MicrosecondsTag {};    struct MillisecondsTag {};
struct BandwidthTag {};        struct LatencyTag {};         struct LocalityCostTag {};
struct CapacityTag {};         struct AvailableMemoryTag {}; struct UtilizationTag {};
struct TransferSizeTag {};     struct PageSizeTag {};        struct AlignmentTag {}; struct HeadroomTag {};

using Bytes          = qty::Quantity<BytesTag, std::uint64_t>;
using Pages          = qty::Quantity<PagesTag, std::uint64_t>;
using ProcessorCount = qty::Quantity<ProcessorCountTag, std::uint32_t>;
using LogicalProcessorIndex = qty::Quantity<LogicalProcessorIndexTag, std::uint32_t>;
using ProcessorGroupIndex   = qty::Quantity<ProcessorGroupIndexTag, std::uint16_t>;
using Nanoseconds    = qty::Quantity<NanosecondsTag, std::uint64_t>;
using Microseconds   = qty::Quantity<MicrosecondsTag, std::uint64_t>;
using Milliseconds   = qty::Quantity<MillisecondsTag, std::uint64_t>;
using Bandwidth      = qty::Quantity<BandwidthTag, std::uint64_t>;
using Latency        = qty::Quantity<LatencyTag, std::uint64_t>;
using LocalityCost   = qty::Quantity<LocalityCostTag, std::uint64_t>;
using Capacity       = qty::Quantity<CapacityTag, std::uint64_t>;
using AvailableMemory = qty::Quantity<AvailableMemoryTag, std::uint64_t>;
using Utilization    = qty::Quantity<UtilizationTag, double>;
using TransferSize   = qty::Quantity<TransferSizeTag, std::uint64_t>;
using PageSize       = qty::Quantity<PageSizeTag, std::uint64_t>;
using Alignment      = qty::Quantity<AlignmentTag, std::uint64_t>;
using Headroom       = qty::Quantity<HeadroomTag, std::uint64_t>;

inline constexpr std::uint64_t kKiB = 1024ULL;
inline constexpr std::uint64_t kMiB = 1024ULL * kKiB;
inline constexpr std::uint64_t kGiB = 1024ULL * kMiB;

template <typename Tag, typename Rep>
std::ostream& operator<<(std::ostream& os, const qty::Quantity<Tag, Rep>& q) { q.write(os); return os; }

} // namespace numafabric
