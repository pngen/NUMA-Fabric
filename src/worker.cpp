// ============================================================================
// Worker session implementation (Winsock client).
// ============================================================================

#include "numafabric/worker/worker.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstring>

namespace numafabric {
namespace worker {

namespace {
bool send_all(SOCKET s, const std::vector<std::uint8_t>& data) {
    int off = 0;
    while (off < static_cast<int>(data.size())) {
        const int n = ::send(s, reinterpret_cast<const char*>(data.data() + off),
                             static_cast<int>(data.size() - off), 0);
        if (n == SOCKET_ERROR) return false;
        off += n;
    }
    return true;
}
} // namespace

WorkerSession::~WorkerSession() { close(); }

bool WorkerSession::connect(const std::string& host, std::uint16_t port) {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { WSACleanup(); return false; }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (host == "127.0.0.1") addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    else { inet_pton(AF_INET, host.c_str(), &addr.sin_addr); }
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s); WSACleanup(); return false;
    }
    sock_ = static_cast<std::uintptr_t>(s);
    decoder_.reset();
    return true;
}

void WorkerSession::close() {
    if (sock_) { closesocket(static_cast<SOCKET>(sock_)); sock_ = 0; WSACleanup(); }
}

bool WorkerSession::send_frame(const std::vector<std::uint8_t>& frame) {
    return send_all(static_cast<SOCKET>(sock_), frame);
}

std::optional<protocol::Frame> WorkerSession::recv_frame() {
    if (!sock_) return std::nullopt;
    if (auto f = decoder_.push(nullptr, 0)) {
        if (f->type == static_cast<std::uint16_t>(protocol::MsgType::Error)) record_error(*f);
        return f;
    }
    std::uint8_t buf[16 * 1024];
    const int n = ::recv(static_cast<SOCKET>(sock_), reinterpret_cast<char*>(buf), sizeof(buf), 0);
    if (n <= 0) return std::nullopt;
    if (auto f = decoder_.push(buf, static_cast<std::size_t>(n))) {
        if (f->type == static_cast<std::uint16_t>(protocol::MsgType::Error)) record_error(*f);
        return f;
    }
    while (true) {
        const int k = ::recv(static_cast<SOCKET>(sock_), reinterpret_cast<char*>(buf), sizeof(buf), 0);
        if (k <= 0) return std::nullopt;
        if (auto f = decoder_.push(buf, static_cast<std::size_t>(k))) {
            if (f->type == static_cast<std::uint16_t>(protocol::MsgType::Error)) record_error(*f);
            return f;
        }
    }
}

void WorkerSession::record_error(const protocol::Frame& f) {
    last_error_ = true;
    last_error_code_ = 0;
    last_error_message_ = "unknown error";
    try {
        protocol::WireReader r(f.payload);
        last_error_code_ = r.u8();
        last_error_message_ = r.str();
    } catch (...) { /* keep default */ }
}

WorkerSession::RegisterInfo WorkerSession::send_register(WorkerId id, ProcessId pid) {
    protocol::WireWriter w;
    w.u64(id.value());
    w.u64(pid.value());
    auto frame = protocol::encode_frame(static_cast<std::uint16_t>(protocol::MsgType::Register), w.data());
    send_frame(frame);
    RegisterInfo info;
    auto reply = recv_frame();
    if (!reply) return info;
    protocol::WireReader r(reply->payload);
    info.boot = WorkerBootId::from(r.u64());
    info.epoch = r.u64();
    info.policy_gen = r.u64();
    info.host_gen = r.u64();
    info.node_count = r.u32();
    info.digest = r.u64();
    return info;
}

WorkerSession::DiscoverInfo WorkerSession::send_discover() {
    protocol::WireWriter w;
    auto frame = protocol::encode_frame(static_cast<std::uint16_t>(protocol::MsgType::DiscoverRequest), w.data());
    send_frame(frame);
    DiscoverInfo info;
    auto reply = recv_frame();
    if (!reply) return info;
    protocol::WireReader r(reply->payload);
    info.node_count = r.u32();
    info.groups = r.u32();
    info.processors = r.u32();
    info.digest = r.u64();
    info.single_node = r.u8() != 0;
    return info;
}

WorkerSession::PlacementOut WorkerSession::send_placement(WorkerId id, WorkerBootId boot, std::uint64_t epoch,
                                                          std::uint64_t footprint, std::uint64_t pref,
                                                          std::uint64_t required, bool has_required,
                                                          bool require_accel, std::uint64_t accel,
                                                          std::uint32_t cpu, bool allow_fallback,
                                                          bool revalidation) {
    protocol::WireWriter w;
    w.u64(id.value()); w.u64(boot.value()); w.u64(epoch); w.u64(footprint); w.u64(pref); w.u64(required);
    w.u8(has_required ? 1 : 0); w.u8(require_accel ? 1 : 0); w.u64(accel); w.u32(cpu);
    w.u8(allow_fallback ? 1 : 0); w.u8(revalidation ? 1 : 0);
    send_frame(protocol::encode_frame(static_cast<std::uint16_t>(protocol::MsgType::PlacementRequest), w.data()));
    PlacementOut out;
    auto reply = recv_frame();
    if (!reply || reply->type == static_cast<std::uint16_t>(protocol::MsgType::Error)) return out;
    protocol::WireReader r(reply->payload);
    out.ok = true;
    out.decision = r.u8();
    out.node = r.u64();
    out.penalty = r.u64();
    out.locality = r.u8();
    out.binding_constraint = r.str();
    out.reason = r.str();
    out.would_change = r.str();
    out.candidate_count = r.u32();
    for (std::uint32_t i = 0; i < out.candidate_count; ++i) {
        const auto node = r.u64(); const auto total = r.u64(); (void)r.u8();
        out.candidates.emplace_back(node, total);
    }
    (void)r.u32();
    return out;
}

std::pair<std::uint64_t, std::uint64_t> WorkerSession::send_reserve(WorkerId id, WorkerBootId boot,
                                                                    std::uint64_t node, std::uint64_t bytes,
                                                                    std::uint32_t cpu, std::uint64_t accel,
                                                                    bool accel_cap) {
    protocol::WireWriter w;
    w.u64(id.value()); w.u64(boot.value()); w.u64(node); w.u64(bytes); w.u32(cpu); w.u64(accel);
    w.u8(accel_cap ? 1 : 0);
    send_frame(protocol::encode_frame(static_cast<std::uint16_t>(protocol::MsgType::ReserveRequest), w.data()));
    auto reply = recv_frame();
    std::pair<std::uint64_t, std::uint64_t> out{0, 0};
    if (!reply || reply->type == static_cast<std::uint16_t>(protocol::MsgType::Error)) return out;
    protocol::WireReader r(reply->payload);
    out.first = r.u64();
    out.second = r.u64();
    return out;
}

bool WorkerSession::send_release(WorkerId id, WorkerBootId boot, std::uint64_t reservation_id) {
    protocol::WireWriter w;
    w.u64(id.value()); w.u64(boot.value()); w.u64(reservation_id);
    send_frame(protocol::encode_frame(static_cast<std::uint16_t>(protocol::MsgType::ReleaseRequest), w.data()));
    auto reply = recv_frame();
    return reply && reply->type != static_cast<std::uint16_t>(protocol::MsgType::Error);
}

std::pair<std::uint64_t, std::uint64_t> WorkerSession::send_bind(WorkerId id, WorkerBootId boot,
                                                                 std::uint64_t node, bool apply) {
    protocol::WireWriter w;
    w.u64(id.value()); w.u64(boot.value()); w.u64(node); w.u8(apply ? 1 : 0);
    send_frame(protocol::encode_frame(static_cast<std::uint16_t>(protocol::MsgType::BindRequest), w.data()));
    auto reply = recv_frame();
    std::pair<std::uint64_t, std::uint64_t> out{0, 0};
    if (!reply || reply->type == static_cast<std::uint16_t>(protocol::MsgType::Error)) return out;
    protocol::WireReader r(reply->payload);
    out.first = r.u64(); out.second = r.u64();
    return out;
}

bool WorkerSession::send_migrate(WorkerId id, WorkerBootId boot, std::uint64_t region,
                                 std::uint64_t target, std::string reason) {
    protocol::WireWriter w;
    w.u64(id.value()); w.u64(boot.value()); w.u64(region); w.u64(target); w.str(reason);
    send_frame(protocol::encode_frame(static_cast<std::uint16_t>(protocol::MsgType::MigrateRequest), w.data()));
    auto reply = recv_frame();
    if (!reply || reply->type == static_cast<std::uint16_t>(protocol::MsgType::Error)) return false;
    protocol::WireReader r(reply->payload);
    return r.u8() != 0;
}

bool WorkerSession::send_observe(WorkerId id, WorkerBootId boot, std::uint64_t node,
                                 std::uint8_t source, bool stale) {
    protocol::WireWriter w;
    w.u64(id.value()); w.u64(boot.value()); w.u64(node); w.u8(source); w.u8(stale ? 1 : 0);
    send_frame(protocol::encode_frame(static_cast<std::uint16_t>(protocol::MsgType::ObserveReport), w.data()));
    auto reply = recv_frame();
    return reply && reply->type != static_cast<std::uint16_t>(protocol::MsgType::Error);
}

bool WorkerSession::send_ping() {
    protocol::WireWriter w;
    send_frame(protocol::encode_frame(static_cast<std::uint16_t>(protocol::MsgType::Ping), w.data()));
    return recv_frame().has_value();
}

} // namespace worker
} // namespace numafabric
