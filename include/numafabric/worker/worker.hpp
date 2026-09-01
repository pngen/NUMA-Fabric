#pragma once
// ============================================================================
// NUMA Fabric - worker session (protocol client).
//
// A synchronous framed-TCP client that registers with a coordinator, receives a
// fresh WorkerBootId, discovers reported NUMA capabilities, requests placement,
// reserves/releases governed resources and performs real work. It is used by
// the real OS worker process and by examples.
// ============================================================================

#include "numafabric/protocol/protocol_frame.hpp"
#include "numafabric/protocol/wire.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace numafabric {
namespace worker {

class WorkerSession {
public:
    WorkerSession() = default;
    ~WorkerSession();
    WorkerSession(const WorkerSession&) = delete;
    WorkerSession& operator=(const WorkerSession&) = delete;

    bool connect(const std::string& host, std::uint16_t port);
    void close();

    // Register; returns the freshly issued boot id + coordinator epoch.
    struct RegisterInfo { WorkerBootId boot = WorkerBootId::invalid(); std::uint64_t epoch = 0; std::uint64_t policy_gen = 0; std::uint64_t host_gen = 0; std::uint32_t node_count = 0; std::uint64_t digest = 0; };
    RegisterInfo send_register(WorkerId id, ProcessId pid);

    struct DiscoverInfo { std::uint32_t node_count = 0; std::uint32_t groups = 0; std::uint32_t processors = 0; std::uint64_t digest = 0; bool single_node = false; };
    DiscoverInfo send_discover();

    struct PlacementOut {
        bool ok = false;
        std::uint8_t decision = 0;
        std::uint64_t node = 0;
        std::uint64_t penalty = 0;
        std::uint8_t locality = 0;
        std::string binding_constraint;
        std::string reason;
        std::string would_change;
        std::uint32_t candidate_count = 0;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> candidates;
    };
    PlacementOut send_placement(WorkerId id, WorkerBootId boot, std::uint64_t epoch,
                                std::uint64_t footprint, std::uint64_t pref, std::uint64_t required,
                                bool has_required, bool require_accel, std::uint64_t accel,
                                std::uint32_t cpu, bool allow_fallback, bool revalidation);

    std::pair<std::uint64_t, std::uint64_t> send_reserve(WorkerId id, WorkerBootId boot, std::uint64_t node,
                                                         std::uint64_t bytes, std::uint32_t cpu,
                                                         std::uint64_t accel, bool accel_cap);
    bool send_release(WorkerId id, WorkerBootId boot, std::uint64_t reservation_id);
    std::pair<std::uint64_t, std::uint64_t> send_bind(WorkerId id, WorkerBootId boot, std::uint64_t node, bool apply);
    bool send_migrate(WorkerId id, WorkerBootId boot, std::uint64_t region, std::uint64_t target, std::string reason);
    bool send_observe(WorkerId id, WorkerBootId boot, std::uint64_t node, std::uint8_t source, bool stale);
    bool send_ping();

    // Returns the last authority-rejection reason if an Error frame was received.
    bool last_was_error() const { return last_error_; }
    std::string last_error_message() const { return last_error_message_; }
    std::uint8_t last_error_code() const { return last_error_code_; }

    bool connected() const { return sock_ != 0; }

private:
    std::uintptr_t sock_ = 0;
    protocol::FrameStreamDecoder decoder_;
    bool last_error_ = false;
    std::string last_error_message_;
    std::uint8_t last_error_code_ = 0;

    bool send_frame(const std::vector<std::uint8_t>& frame);
    std::optional<protocol::Frame> recv_frame();
    void record_error(const protocol::Frame& f);
};

} // namespace worker
} // namespace numafabric
