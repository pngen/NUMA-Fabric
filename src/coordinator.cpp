// ============================================================================
// Distributed coordinator implementation (Winsock, thread-per-connection).
// ============================================================================

#include "numafabric/coordinator/coordinator.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <cstring>

namespace numafabric {
namespace coord {
namespace {

using namespace numafabric::protocol;

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

Coordinator::Coordinator(std::unique_ptr<backend::Backend> backend)
    : runtime_(std::move(backend)) {}

Coordinator::~Coordinator() { shutdown(); }

std::uint16_t Coordinator::bind(std::uint16_t requested_port) {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) throw RuntimeError("WSAStartup failed");

    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == INVALID_SOCKET) { WSACleanup(); throw RuntimeError("socket() failed"); }

    BOOL reuse = TRUE;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(requested_port);
    if (::bind(listen_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(listen_sock); WSACleanup(); throw RuntimeError("bind() failed");
    }
    if (::listen(listen_sock, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(listen_sock); WSACleanup(); throw RuntimeError("listen() failed");
    }
    sockaddr_in local{};
    int len = sizeof(local);
    ::getsockname(listen_sock, reinterpret_cast<sockaddr*>(&local), &len);
    port_ = ntohs(local.sin_port);
    listen_socket_ = static_cast<std::uintptr_t>(listen_sock);
    running_.store(true);

    accept_thread_ = std::thread([this] {
        while (running_.load()) {
            SOCKET cl = ::accept(static_cast<SOCKET>(listen_socket_), nullptr, nullptr);
            if (cl == INVALID_SOCKET) { if (!running_.load()) break; continue; }
            const auto conn_id = next_conn_.fetch_add(1) + 1;
            { std::lock_guard<std::mutex> lk(conn_mutex_); conns_[conn_id] = ConnState{}; }
            conn_threads_.emplace_back([this, cl, conn_id] {
                FrameStreamDecoder decoder;
                bool lost = false;
                {
                    std::vector<std::uint8_t> buf(16 * 1024);
                    while (true) {
                        const int n = ::recv(cl, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0);
                        if (n == 0) { lost = true; break; }
                        if (n == SOCKET_ERROR) {
                            const int err = WSAGetLastError();
                            if (err == WSAECONNRESET || err == WSAECONNABORTED) { lost = true; break; }
                            if (err == WSAEWOULDBLOCK) { continue; }
                            lost = true; break;
                        }
                        // push() may already decode a complete frame; process it.
                        if (auto fr = decoder.push(buf.data(), static_cast<std::size_t>(n))) {
                            auto reply = handle_frame(conn_id, *fr);
                            if (!reply.empty() && !send_all(cl, reply)) { lost = true; break; }
                        }
                        // Drain any further complete frames buffered.
                        while (!lost) {
                            auto more = decoder.push(nullptr, 0);
                            if (!more) break;
                            auto reply = handle_frame(conn_id, *more);
                            if (!reply.empty() && !send_all(cl, reply)) { lost = true; }
                        }
                        if (lost) break;
                    }
                }
                ::closesocket(cl);
                if (lost) {
                    ConnState st;
                    { std::lock_guard<std::mutex> lk(conn_mutex_); auto it = conns_.find(conn_id); if (it != conns_.end()) st = it->second; }
                    if (st.is_worker && st.worker_id.is_valid())
                        runtime_.mark_worker_dead(st.worker_id, st.boot);
                }
                { std::lock_guard<std::mutex> lk(conn_mutex_); conns_.erase(conn_id); }
            });
        }
    });
    return port_;
}

void Coordinator::shutdown() {
    if (!running_.exchange(false)) return;
    if (listen_socket_) {
        ::shutdown(static_cast<SOCKET>(listen_socket_), SD_BOTH);
        ::closesocket(static_cast<SOCKET>(listen_socket_));
        listen_socket_ = 0;
    }
    if (accept_thread_.joinable()) accept_thread_.join();
    for (auto& t : conn_threads_) { if (t.joinable()) t.join(); }
    conn_threads_.clear();
    WSACleanup();
}

std::uint64_t Coordinator::open_controller() {
    const auto id = next_conn_.fetch_add(1) + 1;
    { std::lock_guard<std::mutex> lk(conn_mutex_); conns_[id] = ConnState{}; }
    return id;
}
void Coordinator::close_controller(std::uint64_t id) {
    std::lock_guard<std::mutex> lk(conn_mutex_);
    conns_.erase(id);
}

bool Coordinator::validate_authority(WorkerId id, WorkerBootId boot, AuthorityVerification& out) const {
    const auto* w = runtime_.worker(id);
    if (!w || !w->alive) { out = AuthorityVerification::StaleBoot; return false; }
    if (w->boot_id != boot) { out = AuthorityVerification::StaleBoot; return false; }
    out = AuthorityVerification::Accepted;
    return true;
}

std::vector<std::uint8_t> Coordinator::reply(std::uint16_t type, protocol::WireWriter& payload) const {
    return protocol::encode_frame(static_cast<std::uint16_t>(type), payload.data());
}

std::uint64_t Coordinator::worker_incarnation(WorkerId id) const {
    const auto* w = runtime_.worker(id);
    return w ? w->incarnation : 0;
}
std::optional<WorkerBootId> Coordinator::worker_boot(WorkerId id) const {
    const auto* w = runtime_.worker(id);
    return w ? std::optional<WorkerBootId>(w->boot_id) : std::nullopt;
}
bool Coordinator::worker_alive(WorkerId id) const {
    const auto* w = runtime_.worker(id);
    return w && w->alive;
}

std::vector<std::uint8_t> Coordinator::handle_frame(std::uint64_t conn_id, const protocol::Frame& frame) {
    using namespace numafabric::protocol;
    const auto type = static_cast<MsgType>(frame.type);

    auto error_reply = [&](std::string msg, AuthorityVerification av) {
        WireWriter w;
        w.u8(static_cast<std::uint8_t>(av));
        w.str(msg);
        return reply(static_cast<std::uint16_t>(MsgType::Error), w);
    };

    try {
        switch (type) {
            case MsgType::Register: {
                WireReader r(frame.payload);
                const auto worker_id = WorkerId::from(r.u64());
                const auto process_id = ProcessId::from(r.u64());
                if (!worker_id.is_valid()) return error_reply("invalid worker id", AuthorityVerification::RejectedUnknown);
                affinity::Worker w;
                if (runtime_.worker(worker_id) == nullptr) w = runtime_.register_worker(worker_id, process_id);
                else w = runtime_.restart_worker(worker_id, process_id); // new incarnation -> fresh boot
                { std::lock_guard<std::mutex> lk(conn_mutex_); conns_[conn_id] = ConnState{worker_id, w.boot_id, true}; }
                WireWriter out;
                out.u64(w.boot_id.value());
                out.u64(runtime_.epoch().value());
                out.u64(runtime_.policy_generation());
                out.u64(runtime_.topology().generation.value());
                out.u32(static_cast<std::uint32_t>(runtime_.topology().numa_node_count()));
                out.u64(runtime_.topology().semantic_digest().value());
                return reply(static_cast<std::uint16_t>(MsgType::RegisterAck), out);
            }
            case MsgType::PlacementRequest: {
                WireReader r(frame.payload);
                const auto worker_id = WorkerId::from(r.u64());
                const auto boot = WorkerBootId::from(r.u64());
                const auto epoch = r.u64();
                const auto footprint = r.u64();
                const auto pref = NumaNodeId::from(r.u64());
                const auto req_node = NumaNodeId::from(r.u64());
                const auto has_required = r.u8() != 0;
                const auto require_accel = r.u8() != 0;
                const auto accel = AcceleratorId::from(r.u64());
                const auto cpu = r.u32();
                const auto allow_fallback = r.u8() != 0;
                const auto revalidation = r.u8() != 0;
                // Deterministic authority order: epoch, then boot.
                if (epoch != runtime_.epoch().value())
                    return error_reply("stale coordinator epoch", AuthorityVerification::StaleEpoch);
                AuthorityVerification av;
                if (!validate_authority(worker_id, boot, av))
                    return error_reply("stale placement authority", av);
                placement::PlacementRequest req;
                req.workload_id = "coordinated";
                req.memory_footprint = Bytes::from(footprint);
                req.preferred_node = pref;
                req.required_node = req_node;
                req.has_required_node = has_required;
                req.require_accelerator_locality = require_accel;
                req.preferred_accelerator = accel;
                req.required_cpu_count = cpu;
                req.allow_fallback = allow_fallback;
                req.policy_generation = PolicyGeneration::from(runtime_.policy_generation() + 1);
                req.revalidation_pending = revalidation != 0;
                auto decision = runtime_.plan(req);
                WireWriter out;
                out.u8(static_cast<std::uint8_t>(decision.kind));
                out.u64(decision.selected_node.value());
                out.u64(decision.expected_penalty.value());
                out.u8(static_cast<std::uint8_t>(decision.expected_locality_class));
                out.str(decision.binding_constraint);
                out.str(decision.reason);
                out.str(decision.would_change);
                out.u32(static_cast<std::uint32_t>(decision.ranked_candidates.size()));
                for (const auto& c : decision.ranked_candidates) {
                    out.u64(c.node.value()); out.u64(c.total.value()); out.u8(c.eligible ? 1 : 0);
                }
                out.u32(static_cast<std::uint32_t>(decision.eliminated_candidates.size()));
                for (const auto& e : decision.eliminated_candidates) out.str(e);
                return reply(static_cast<std::uint16_t>(MsgType::PlacementReply), out);
            }
            case MsgType::ReserveRequest: {
                WireReader r(frame.payload);
                const auto worker_id = WorkerId::from(r.u64());
                const auto boot = WorkerBootId::from(r.u64());
                const auto node = NumaNodeId::from(r.u64());
                const auto bytes = r.u64();
                const auto cpu = r.u32();
                const auto accel = AcceleratorId::from(r.u64());
                const auto accel_cap = r.u8() != 0;
                AuthorityVerification av;
                if (!validate_authority(worker_id, boot, av)) return error_reply("stale reserve authority", av);
                reservation::ReserveRequest rr;
                rr.node = node; rr.bytes = Bytes::from(bytes); rr.cpu_slots = cpu;
                rr.accelerator = accel; rr.accelerator_capacity = accel_cap; rr.worker = worker_id;
                auto res = runtime_.reserve(rr);
                WireWriter out;
                out.u64(res.id.value()); out.u64(res.generation.value());
                return reply(static_cast<std::uint16_t>(MsgType::ReserveReply), out);
            }
            case MsgType::ReleaseRequest: {
                WireReader r(frame.payload);
                const auto worker_id = WorkerId::from(r.u64());
                const auto boot = WorkerBootId::from(r.u64());
                const auto res_id = ReservationId::from(r.u64());
                AuthorityVerification av;
                if (!validate_authority(worker_id, boot, av)) return error_reply("stale release authority", av);
                runtime_.release_reservation(res_id);
                WireWriter out; out.u8(1);
                return reply(static_cast<std::uint16_t>(MsgType::ReleaseReply), out);
            }
            case MsgType::BindRequest: {
                WireReader r(frame.payload);
                const auto worker_id = WorkerId::from(r.u64());
                const auto boot = WorkerBootId::from(r.u64());
                const auto node = NumaNodeId::from(r.u64());
                const auto apply = r.u8() != 0;
                AuthorityVerification av;
                if (!validate_authority(worker_id, boot, av)) return error_reply("stale bind authority", av);
                auto bind = runtime_.bind_worker(worker_id, boot, node, apply);
                WireWriter out;
                out.u64(bind.binding_id.value());
                out.u64(bind.generation.value());
                out.u8(bind.applied ? 1 : 0);
                return reply(static_cast<std::uint16_t>(MsgType::BindReply), out);
            }
            case MsgType::MigrateRequest: {
                WireReader r(frame.payload);
                const auto worker_id = WorkerId::from(r.u64());
                const auto boot = WorkerBootId::from(r.u64());
                const auto region = MemoryRegionId::from(r.u64());
                const auto target = NumaNodeId::from(r.u64());
                const auto reason = r.str();
                AuthorityVerification av;
                if (!validate_authority(worker_id, boot, av)) return error_reply("stale migrate authority", av);
                Runtime::Auth auth{runtime_.epoch(), boot};
                auto committed = runtime_.migrate(region, target, worker_id, auth, reason);
                WireWriter out; out.u8(committed ? 1 : 0);
                return reply(static_cast<std::uint16_t>(MsgType::MigrateReply), out);
            }
            case MsgType::ObserveReport: {
                WireReader r(frame.payload);
                const auto worker_id = WorkerId::from(r.u64());
                const auto boot = WorkerBootId::from(r.u64());
                const auto node = NumaNodeId::from(r.u64());
                const auto src = static_cast<ProvenanceSource>(r.u8());
                const auto stale = r.u8() != 0;
                AuthorityVerification av;
                if (!validate_authority(worker_id, boot, av)) return error_reply("stale observe authority", av);
                runtime_.observe(worker_id, boot, node, src, stale);
                WireWriter out; out.u8(1);
                return reply(static_cast<std::uint16_t>(MsgType::ObserveAck), out);
            }
            case MsgType::Ping: { WireWriter out; out.u64(1); return reply(static_cast<std::uint16_t>(MsgType::Pong), out); }
            case MsgType::Shutdown: {
                running_.store(false);
                WireWriter out; out.u8(1);
                return reply(static_cast<std::uint16_t>(MsgType::Shutdown), out);
            }
            case MsgType::DiscoverRequest: {
                const auto& t = runtime_.topology();
                WireWriter out;
                out.u32(static_cast<std::uint32_t>(t.numa_node_count()));
                out.u32(static_cast<std::uint32_t>(t.groups.size()));
                out.u32(static_cast<std::uint32_t>(t.processor_count()));
                out.u64(t.semantic_digest().value());
                out.u8(t.is_single_node() ? 1 : 0);
                return reply(static_cast<std::uint16_t>(MsgType::DiscoverReply), out);
            }
            case MsgType::RevalidateRequest: {
                WireReader r(frame.payload);
                const auto worker_id = WorkerId::from(r.u64());
                const auto boot = WorkerBootId::from(r.u64());
                AuthorityVerification av;
                if (!validate_authority(worker_id, boot, av)) return error_reply("stale revalidate authority", av);
                WireWriter out; out.u8(1);
                return reply(static_cast<std::uint16_t>(MsgType::RevalidateReply), out);
            }
            default:
                return error_reply("unhandled message type", AuthorityVerification::RejectedUnknown);
        }
    } catch (const std::exception& e) {
        WireWriter w;
        w.u8(static_cast<std::uint8_t>(AuthorityVerification::RejectedUnknown));
        w.str(e.what());
        return reply(static_cast<std::uint16_t>(MsgType::Error), w);
    }
}

} // namespace coord
} // namespace numafabric
