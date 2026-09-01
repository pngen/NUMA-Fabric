#pragma once
// ============================================================================
// NUMA Fabric - versioned framed binary protocol.
//
// Every frame is: magic | protocol version | message type | payload length |
// CRC-32 | payload. Decoding is bounded and strict: it rejects malformed magic,
// unsupported versions, impossible lengths, truncation, trailing garbage,
// corrupted checksums and unknown message types. No test-timeout mechanism is
// used anywhere; socket no-data conditions never terminate healthy connections.
// ============================================================================

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace numafabric {
namespace protocol {

class ProtocolError final : public std::exception {
public:
    explicit ProtocolError(std::string m) : message_(std::move(m)) {}
    const char* what() const noexcept override { return message_.c_str(); }
private:
    std::string message_;
};

inline constexpr std::uint8_t kMagic[8] = {'N', 'U', 'M', 'A', 'F', 'R', 'M', '1'};
inline constexpr std::uint16_t kProtocolVersion = 1;
inline constexpr std::uint32_t kFrameHeaderSize = 8 + 2 + 2 + 4 + 4; // 20 bytes
inline constexpr std::uint32_t kMaxPayloadLength = 1u << 24;          // 16 MiB

// Message types.
enum class MsgType : std::uint16_t {
    Hello = 1,
    Register = 2,
    RegisterAck = 3,
    DiscoverRequest = 4,
    DiscoverReply = 5,
    PlacementRequest = 6,
    PlacementReply = 7,
    ReserveRequest = 8,
    ReserveReply = 9,
    ReleaseRequest = 10,
    ReleaseReply = 11,
    BindRequest = 12,
    BindReply = 13,
    MigrateRequest = 14,
    MigrateReply = 15,
    ObserveReport = 16,
    ObserveAck = 17,
    Ping = 18,
    Pong = 19,
    Shutdown = 20,
    WorkerLost = 21,
    RevalidateRequest = 22,
    RevalidateReply = 23,
    Error = 24
};

struct Frame {
    std::uint16_t version = kProtocolVersion;
    std::uint16_t type = 0;
    std::vector<std::uint8_t> payload;
};

// CRC-32 (IEEE 802.3, reflected polynomial 0xEDB88320).
std::uint32_t crc32(const std::uint8_t* data, std::size_t n);
std::uint32_t crc32(const std::vector<std::uint8_t>& data);

// Encode a frame into a serialized byte vector, computing the checksum.
std::vector<std::uint8_t> encode_frame(std::uint16_t type,
                                       const std::vector<std::uint8_t>& payload);

// Stream decoder: append received bytes and pull complete frames. Bounded and
// strict (magic/version/length/checksum/trailing-garbage checks). An incomplete
// frame is simply buffered; a malformed frame throws ProtocolError.
class FrameStreamDecoder {
public:
    // Returns one frame per call when a complete, valid frame is buffered;
    // nullopt when more bytes are needed (no-data is not an error).
    std::optional<Frame> push(const std::uint8_t* data, std::size_t n);
    void reset();

    [[nodiscard]] std::size_t buffered() const { return buffer_.size(); }

private:
    std::vector<std::uint8_t> buffer_;

    bool try_extract(Frame& out);
    std::optional<Frame> decode_one(const std::uint8_t* data, std::size_t n, std::size_t& consumed);
};

} // namespace protocol
} // namespace numafabric
