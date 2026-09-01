#pragma once
// ============================================================================
// NUMA Fabric - tiny wire codec for protocol payloads.
//
// Little-endian primitive + length-prefixed string/blob encoding used to build
// and parse the payloads of protocol messages. Bounded and strict on decode.
// ============================================================================

#include "numafabric/core/ids.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace numafabric {
namespace protocol {

class WireError final : public std::exception {
public:
    explicit WireError(std::string m) : message_(std::move(m)) {}
    const char* what() const noexcept override { return message_.c_str(); }
private:
    std::string message_;
};

class WireWriter {
public:
    void u8(std::uint8_t v) { b_.push_back(v); }
    void u16(std::uint16_t v) { b_.push_back(static_cast<std::uint8_t>(v & 0xFF)); b_.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF)); }
    void u32(std::uint32_t v) { for (int i = 0; i < 4; ++i) b_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF)); }
    void u64(std::uint64_t v) { for (int i = 0; i < 8; ++i) b_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF)); }
    void str(const std::string& s) { u32(static_cast<std::uint32_t>(s.size())); b_.insert(b_.end(), s.begin(), s.end()); }
    void bytes(const std::vector<std::uint8_t>& v) { u32(static_cast<std::uint32_t>(v.size())); b_.insert(b_.end(), v.begin(), v.end()); }
    const std::vector<std::uint8_t>& data() const { return b_; }
    std::vector<std::uint8_t> take() { return std::move(b_); }
private:
    std::vector<std::uint8_t> b_;
};

class WireReader {
public:
    WireReader(const std::uint8_t* p, std::size_t n) : p_(p), n_(n) {}
    explicit WireReader(const std::vector<std::uint8_t>& v) : p_(v.data()), n_(v.size()) {}

    std::size_t remaining() const { return n_ - i_; }
    bool at_end() const { return i_ == n_; }
    void need(std::size_t k) const { if (i_ + k > n_) throw WireError("truncated wire payload"); }

    std::uint8_t u8() { need(1); return p_[i_++]; }
    std::uint16_t u16() { need(2); std::uint16_t v = static_cast<std::uint16_t>(p_[i_] | (p_[i_ + 1] << 8)); i_ += 2; return v; }
    std::uint32_t u32() { need(4); std::uint32_t v = 0; for (int k = 0; k < 4; ++k) v |= static_cast<std::uint32_t>(p_[i_ + k]) << (8 * k); i_ += 4; return v; }
    std::uint64_t u64() { need(8); std::uint64_t v = 0; for (int k = 0; k < 8; ++k) v |= static_cast<std::uint64_t>(p_[i_ + k]) << (8 * k); i_ += 8; return v; }
    std::string str() { const auto l = u32(); if (l > remaining()) throw WireError("truncated wire string"); std::string s(reinterpret_cast<const char*>(p_ + i_), l); i_ += l; return s; }
    std::vector<std::uint8_t> bytes() { const auto l = u32(); if (l > remaining()) throw WireError("truncated wire blob"); std::vector<std::uint8_t> v(p_ + i_, p_ + i_ + l); i_ += l; return v; }

private:
    const std::uint8_t* p_;
    std::size_t n_;
    std::size_t i_ = 0;
};

} // namespace protocol
} // namespace numafabric
