// ============================================================================
// Framed binary protocol implementation (CRC-32, bounded strict decode).
// ============================================================================

#include "numafabric/protocol/protocol_frame.hpp"

#include <array>
#include <cstring>

namespace numafabric {
namespace protocol {

std::uint32_t crc32(const std::uint8_t* data, std::size_t n) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

std::uint32_t crc32(const std::vector<std::uint8_t>& data) {
    return crc32(data.data(), data.size());
}

namespace {
void put_u16(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(static_cast<std::uint8_t>(x & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFF));
}
void put_u32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    v.push_back(static_cast<std::uint8_t>(x & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >> 16) & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >> 24) & 0xFF));
}
std::uint16_t get_u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
std::uint32_t get_u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
} // namespace

std::vector<std::uint8_t> encode_frame(std::uint16_t type, const std::vector<std::uint8_t>& payload) {
    if (payload.size() > kMaxPayloadLength) throw ProtocolError("payload too large");
    std::vector<std::uint8_t> out;
    out.reserve(kFrameHeaderSize + payload.size());
    out.insert(out.end(), kMagic, kMagic + 8);
    put_u16(out, kProtocolVersion);
    put_u16(out, type);
    put_u32(out, static_cast<std::uint32_t>(payload.size()));
    // CRC covers header(base, no crc field) + payload.
    std::vector<std::uint8_t> tohash = out;
    tohash.insert(tohash.end(), payload.begin(), payload.end());
    const auto crc = crc32(tohash.data(), tohash.size());
    put_u32(out, crc);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::optional<Frame> FrameStreamDecoder::decode_one(const std::uint8_t* data, std::size_t n,
                                                    std::size_t& consumed) {
    if (n < kFrameHeaderSize) return std::nullopt;
    if (std::memcmp(data, kMagic, 8) != 0) {
        throw ProtocolError("malformed magic");
    }
    const auto version = get_u16(data + 8);
    if (version != kProtocolVersion) throw ProtocolError("unsupported protocol version");
    const auto type = get_u16(data + 10);
    const auto len = get_u32(data + 12);
    if (len > kMaxPayloadLength) throw ProtocolError("impossible payload length");
    const auto total = static_cast<std::size_t>(kFrameHeaderSize) + len;
    if (n < total) return std::nullopt;
    const auto expected_crc = get_u32(data + 16);
    // Recompute over header (without the crc field) + payload.
    std::vector<std::uint8_t> tohash(data, data + 16);
    tohash.insert(tohash.end(), data + kFrameHeaderSize, data + total);
    const auto actual = crc32(tohash.data(), tohash.size());
    if (actual != expected_crc) throw ProtocolError("corrupted checksum");
    Frame f;
    f.version = version;
    f.type = type;
    f.payload.assign(data + kFrameHeaderSize, data + total);
    const auto mt = static_cast<MsgType>(type);
    switch (mt) {
        case MsgType::Hello: case MsgType::Register: case MsgType::RegisterAck:
        case MsgType::DiscoverRequest: case MsgType::DiscoverReply:
        case MsgType::PlacementRequest: case MsgType::PlacementReply:
        case MsgType::ReserveRequest: case MsgType::ReserveReply:
        case MsgType::ReleaseRequest: case MsgType::ReleaseReply:
        case MsgType::BindRequest: case MsgType::BindReply:
        case MsgType::MigrateRequest: case MsgType::MigrateReply:
        case MsgType::ObserveReport: case MsgType::ObserveAck:
        case MsgType::Ping: case MsgType::Pong: case MsgType::Shutdown:
        case MsgType::WorkerLost: case MsgType::RevalidateRequest:
        case MsgType::RevalidateReply: case MsgType::Error:
            break;
        default:
            throw ProtocolError("unknown message type");
    }
    consumed = total;
    return f;
}

bool FrameStreamDecoder::try_extract(Frame& out) {
    std::size_t consumed = 0;
    auto frame = decode_one(buffer_.data(), buffer_.size(), consumed);
    if (!frame) return false;
    out = std::move(*frame);
    buffer_.erase(buffer_.begin(), buffer_.begin() + consumed);
    return true;
}

std::optional<Frame> FrameStreamDecoder::push(const std::uint8_t* data, std::size_t n) {
    buffer_.insert(buffer_.end(), data, data + n);
    Frame f;
    if (!try_extract(f)) return std::nullopt;
    return f;
}

void FrameStreamDecoder::reset() { buffer_.clear(); }

} // namespace protocol
} // namespace numafabric
