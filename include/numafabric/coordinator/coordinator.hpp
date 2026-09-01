#pragma once
// ============================================================================
// NUMA Fabric - distributed coordinator.
//
// A real framed-TCP server that brands worker authority, owns authoritative
// placement/binding/reservation decisions and fences every mutable operation
// against stale coordinator epoch, stale worker boot and stale placement /
// binding / memory / observation generations. Connection exceptions never crash
// the process; registration frames are never lost or double-consumed; restarted
// workers may register concurrently via a thread-per-connection model.
// ============================================================================

#include "numafabric/protocol/protocol_frame.hpp"
#include "numafabric/protocol/wire.hpp"
#include "numafabric/runtime/runtime.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace numafabric {
namespace coord {

class Coordinator {
public:
    explicit Coordinator(std::unique_ptr<backend::Backend> backend);
    ~Coordinator();

    Coordinator(const Coordinator&) = delete;
    Coordinator& operator=(const Coordinator&) = delete;

    std::uint16_t bind(std::uint16_t requested_port = 0);
    void shutdown();

    std::uint16_t port() const { return port_; }
    bool running() const { return running_.load(); }
    Runtime& runtime() { return runtime_; }
    const Runtime& runtime() const { return runtime_; }

    CoordinatorEpoch epoch() const { return runtime_.epoch(); }
    std::uint64_t worker_incarnation(WorkerId id) const;
    std::optional<WorkerBootId> worker_boot(WorkerId id) const;
    bool worker_alive(WorkerId id) const;
    void advance_epoch() { runtime_.advance_epoch(); }

    void persist(const std::string& path) { runtime_.persist(path); }
    void recover(const std::string& path) { runtime_.recover(path); }

    // Handle a frame from a connection (used by the per-connection thread and by
    // the proof harness for real-TCP stale replays). Returns optional reply bytes.
    std::vector<std::uint8_t> handle_frame(std::uint64_t conn_id, const protocol::Frame& frame);

    std::uint64_t open_controller();
    void close_controller(std::uint64_t id);

private:
    struct ConnState {
        WorkerId worker_id = WorkerId::invalid();
        WorkerBootId boot = WorkerBootId::invalid();
        bool is_worker = false;
    };

    Runtime runtime_;
    std::thread accept_thread_;
    std::vector<std::thread> conn_threads_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> next_conn_{1};
    std::uint16_t port_ = 0;
    std::uintptr_t listen_socket_ = 0;

    mutable std::mutex conn_mutex_;
    std::map<std::uint64_t, ConnState> conns_;

    bool validate_authority(WorkerId id, WorkerBootId boot, AuthorityVerification& out) const;
    std::vector<std::uint8_t> reply(std::uint16_t type, protocol::WireWriter& payload) const;
};

} // namespace coord
} // namespace numafabric
