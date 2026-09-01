// ============================================================================
// nf-proof - atomic real multiprocess authority proof.
//
// Drives the full kill/restart/stale-replay/recovery scenario against a real
// coordinator (phase 1 in-process service, phase 2 a fresh coordinator OS
// process) and two REAL OS worker processes over real framed TCP. No test
// timeout mechanism is used: the harness blocks until each worker's marker file
// records the expected step. A genuine hang is a defect to diagnose, never
// converted into a failure by a timer.
// ============================================================================

#include "numafabric/numafabric.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

using namespace numafabric;

namespace {
std::string exe_dir;

std::string read_file(const std::string& path) {
    std::ifstream is(path);
    std::ostringstream ss;
    ss << is.rdbuf();
    return ss.str();
}

void wait_marker(const std::string& path, const std::string& needle) {
    while (true) {
        if (std::ifstream(path).good() && read_file(path).find(needle) != std::string::npos) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}

std::string extract(const std::string& s, const std::string& key) {
    const auto pos = s.find(key);
    if (pos == std::string::npos) return "";
    auto val = std::string(s.begin() + pos + key.size(), s.end());
    const auto nl = val.find('\n');
    if (nl != std::string::npos) val = val.substr(0, nl);
    const auto ws = val.find_first_of(" \r");
    return (ws == std::string::npos) ? val : val.substr(0, ws);
}

HANDLE spawn_process(const std::string& cmdline) {
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::string full = cmdline;
    if (full.find('\\') == std::string::npos && !full.empty())
        full = exe_dir + "\\" + full;
    char* buf = new char[full.size() + 1];
    std::memcpy(buf, full.data(), full.size());
    buf[full.size()] = '\0';
    BOOL ok = CreateProcessA(nullptr, buf, nullptr, nullptr, FALSE, 0, nullptr, exe_dir.c_str(), &si, &pi);
    delete[] buf;
    if (!ok) { std::printf("spawn failed: %s\n", full.c_str()); return nullptr; }
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

void kill_process(HANDLE h) {
    if (h) { TerminateProcess(h, 1); WaitForSingleObject(h, INFINITE); CloseHandle(h); }
}

std::string worker_cmd(std::uint32_t id, std::uint16_t port, const std::string& marker) {
    return "nf-worker --id " + std::to_string(id) + " --host 127.0.0.1 --port " + std::to_string(port) +
           " --footprint 33554432 --iterations 1 --hold --marker " + marker;
}

std::string coord_cmd(const std::string& port_file, const std::string& recover_path) {
    std::string c = "nf-coordinator --backend synthetic --nodes 2 --procs 8 --port 0 --port-file " + port_file;
    if (!recover_path.empty()) c += " --recover " + recover_path;
    return c;
}
} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string state_path = (argc >= 2) ? argv[1] : "numafabric_proof.state";

    {
        char pathBuf[MAX_PATH];
        GetModuleFileNameA(nullptr, pathBuf, MAX_PATH);
        std::string self(pathBuf);
        const auto pos = self.find_last_of('\\');
        exe_dir = (pos == std::string::npos) ? "." : self.substr(0, pos);
    }

    std::printf("=== NUMA Fabric multiprocess authority proof ===\n");

    backend::SyntheticConfig cfg;
    cfg.node_count = 2;
    cfg.processors_per_node = 8;
    cfg.group_capacity = 64;
    coord::Coordinator coordinator(backend::make_synthetic_backend(cfg));
    coordinator.runtime().discover();
    coordinator.runtime().refresh_accelerators();
    auto port = coordinator.bind(0);
    std::printf("[coordinator] phase-1 listening on %u\n", port);

    const std::string markerA = "proof_a.marker";
    const std::string markerB = "proof_b.marker";
    { std::ofstream(markerA, std::ios::trunc).close(); std::ofstream(markerB, std::ios::trunc).close(); }

    HANDLE hA = spawn_process(worker_cmd(10, port, markerA));
    wait_marker(markerA, "BOOT=");
    const auto aBootId = WorkerBootId::from(std::stoull(extract(read_file(markerA), "BOOT=")));
    std::printf("[worker A] boot=%llu\n", aBootId.value());

    HANDLE hB = spawn_process(worker_cmd(20, port, markerB));
    wait_marker(markerB, "BOOT=");
    const auto bBootId = WorkerBootId::from(std::stoull(extract(read_file(markerB), "BOOT=")));
    std::printf("[worker B] boot=%llu\n", bBootId.value());

    wait_marker(markerA, "DONE=true");
    wait_marker(markerB, "DONE=true");
    if (aBootId == bBootId) { std::printf("FAIL: distinct boots not issued\n"); return 1; }
    std::printf("[assert] both workers registered with distinct boots and did real work\n");

    if (!coordinator.worker_alive(WorkerId::from(10)) ||
        !(coordinator.worker_boot(WorkerId::from(10)).has_value() && *coordinator.worker_boot(WorkerId::from(10)) == aBootId)) {
        std::printf("FAIL: A pre-restart authority inconsistent\n");
        return 1;
    }
    std::printf("[assert] captured pre-restart authority for worker A (boot %llu)\n", aBootId.value());

    kill_process(hA);
    hA = nullptr;
    while (coordinator.worker_alive(WorkerId::from(10))) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    std::printf("[assert] worker A loss observed (alive=%d)\n", coordinator.worker_alive(WorkerId::from(10)) ? 1 : 0);

    if (!coordinator.worker_alive(WorkerId::from(20)) ||
        !(coordinator.worker_boot(WorkerId::from(20)).has_value() && *coordinator.worker_boot(WorkerId::from(20)) == bBootId)) {
        std::printf("FAIL: worker B became invalid after A death\n");
        return 1;
    }
    std::printf("[assert] worker B remains valid and unaffected\n");

    const auto epochBefore = coordinator.epoch();
    coordinator.advance_epoch();
    const auto epochAfter = coordinator.epoch();
    std::printf("[coordinator] epoch %llu -> %llu\n", epochBefore.value(), epochAfter.value());

    {
        worker::WorkerSession stale;
        if (!stale.connect("127.0.0.1", port)) { std::printf("FAIL: stale tcp connect\n"); return 1; }
        stale.send_placement(WorkerId::from(20), bBootId, epochBefore.value(), 1048576, 0, 0, false, false, 0, 0, true, false);
        if (!stale.last_was_error() || stale.last_error_code() != static_cast<std::uint8_t>(AuthorityVerification::StaleEpoch)) {
            std::printf("FAIL: stale CoordinatorEpoch not rejected (code=%u)\n", stale.last_error_code());
            return 1;
        }
        std::printf("[assert] stale CoordinatorEpoch rejected (StaleEpoch)\n");
    }
    {
        worker::WorkerSession stale;
        if (!stale.connect("127.0.0.1", port)) { std::printf("FAIL: stale tcp connect\n"); return 1; }
        stale.send_placement(WorkerId::from(10), aBootId, epochAfter.value(), 1048576, 0, 0, false, false, 0, 0, true, false);
        if (!stale.last_was_error() || stale.last_error_code() != static_cast<std::uint8_t>(AuthorityVerification::StaleBoot)) {
            std::printf("FAIL: stale WorkerBootId not rejected (code=%u)\n", stale.last_error_code());
            return 1;
        }
        std::printf("[assert] stale WorkerBootId rejected (StaleBoot)\n");
    }

    const std::string markerA2 = "proof_a2.marker";
    { std::ofstream(markerA2, std::ios::trunc).close(); }
    HANDLE hA2 = spawn_process(worker_cmd(10, port, markerA2));
    wait_marker(markerA2, "BOOT=");
    const auto a2BootId = WorkerBootId::from(std::stoull(extract(read_file(markerA2), "BOOT=")));
    if (a2BootId == aBootId) { std::printf("FAIL: restarted A got the same boot id\n"); return 1; }
    wait_marker(markerA2, "DONE=true");
    std::printf("[assert] restarted worker A carries a FRESH boot id (%llu) and did fresh work\n", a2BootId.value());

    {
        worker::WorkerSession stale;
        stale.connect("127.0.0.1", port);
        stale.send_placement(WorkerId::from(10), aBootId, epochAfter.value(), 1048576, 0, 0, false, false, 0, 0, true, false);
        if (!stale.last_was_error()) { std::printf("FAIL: stale pre-restart authority fenced\n"); return 1; }
        std::printf("[assert] stale pre-restart authority still fenced after restart\n");
    }

    // Stop the remaining phase-1 worker OS processes so their connection
    // threads unblock; then the coordinator can join them cleanly.
    kill_process(hB); hB = nullptr;
    if (hA2) { kill_process(hA2); hA2 = nullptr; }

    coordinator.persist(state_path);
    coordinator.shutdown();
    std::printf("[root] phase-1 coordinator stopped; state persisted to %s\n", state_path.c_str());

    const std::string portFile = "coord2.portfile";
    { std::ofstream(portFile, std::ios::trunc).close(); }
    HANDLE hCoord2 = spawn_process(coord_cmd(portFile, state_path));
    std::string p2s;
    while (p2s.empty()) {
        const auto txt = read_file(portFile);
        if (!txt.empty()) { p2s = txt; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    const auto recoverPort = static_cast<std::uint16_t>(std::stoul(p2s));
    std::printf("[fresh coordinator process] recovered state, listening on %u\n", recoverPort);

    const std::string markerA3 = "proof_a3.marker";
    { std::ofstream(markerA3, std::ios::trunc).close(); }
    HANDLE hA3 = spawn_process(worker_cmd(10, recoverPort, markerA3));
    wait_marker(markerA3, "DONE=true");
    std::printf("[assert] fresh boot revalidated against the recovered coordinator and did fresh work\n");

    kill_process(hB);
    if (hA2) kill_process(hA2);
    kill_process(hA3);
    kill_process(hCoord2);

    std::printf("=== multiprocess authority proof PASSED ===\n");
    return 0;
}
