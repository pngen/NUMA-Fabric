#pragma once
// ============================================================================
// NUMA Fabric - deterministic semantic digest.
//
// A stable FNV-1a-64 incremental hash used to fingerprint immutable topology /
// locality snapshots. It never depends on process addresses, pointer identity,
// or unstable enumeration ordering: callers sort IDs before hashing them so the
// same logical machine always yields the same digest regardless of discovery
// order. Integrity for persisted/protocol payloads uses CRC-32 (see
// persistence / frame codec); this digest is for semantic comparison only.
// ============================================================================

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace numafabric {

class SemanticDigest {
public:
    using value_type = std::uint64_t;

    SemanticDigest() : state_(offset_basis_) {}
    explicit SemanticDigest(value_type seeded) : state_(seeded) {}

    // Raw byte accumulation.
    SemanticDigest& bytes(const void* data, std::size_t n) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < n; ++i) { step(p[i]); }
        return *this;
    }
    SemanticDigest& byte(unsigned char b) { step(b); return *this; }

    // Typed accumulation with canonical byte order (little-endian), so the same
    // value produced on any host yields the same digest.
    SemanticDigest& u64(std::uint64_t v) { for (int i = 0; i < 8; ++i) { step(static_cast<unsigned char>((v >> (8 * i)) & 0xFF)); } return *this; }
    SemanticDigest& u32(std::uint32_t v) { for (int i = 0; i < 4; ++i) { step(static_cast<unsigned char>((v >> (8 * i)) & 0xFF)); } return *this; }
    SemanticDigest& u16(std::uint16_t v) { for (int i = 0; i < 2; ++i) { step(static_cast<unsigned char>((v >> (8 * i)) & 0xFF)); } return *this; }
    SemanticDigest& u8(std::uint8_t v) { step(v); return *this; }
    SemanticDigest& i64(std::int64_t v) { return u64(static_cast<std::uint64_t>(v)); }
    SemanticDigest& boolean(bool b) { return u8(b ? 1 : 0); }

    SemanticDigest& string(std::string_view s) {
        // length-prefixed so concatenations do not collide
        u64(s.size());
        bytes(s.data(), s.size());
        return *this;
    }
    SemanticDigest& field(std::string_view name) {
        u64(name.size());
        bytes(name.data(), name.size());
        u8(0xFF); // field separator
        return *this;
    }

    value_type value() const { return state_; }
    std::string hex() const;

private:
    static constexpr std::uint64_t offset_basis_ = 0xcbf29ce484222325ULL;
    static constexpr std::uint64_t prime_ = 0x100000001b3ULL;
    std::uint64_t state_;

    void step(unsigned char c) {
        state_ ^= c;
        state_ *= prime_;
    }
};

inline std::string SemanticDigest::hex() const {
    const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(16);
    for (int i = 56; i >= 0; i -= 8) {
        const auto byte = static_cast<unsigned>((state_ >> i) & 0xFF);
        out += digits[byte >> 4];
        out += digits[byte & 0x0F];
    }
    return out;
}

} // namespace numafabric
