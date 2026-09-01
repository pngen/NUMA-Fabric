// ============================================================================
// nf-worker - a real NUMA Fabric worker OS process.
//
// Connects to a coordinator, registers (fresh WorkerBootId), reports discovered
// NUMA capabilities, obtains a placement, reserves/releases governed capacity,
// performs REAL bounded work, and optionally holds so the process can be killed
// as a real OS process. Progress is written to a marker file for the proof
// harness. Physical locality evidence is reported with its provenance.
// ============================================================================

#include "numafabric/numafabric.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace numafabric;

namespace {
std::string g_marker;

void marker(const std::string& line) {
    if (g_marker.empty()) return;
    std::ofstream os(g_marker, std::ios::app);
    os << line << "\n";
    os.close();
}
std::uint64_t checksum_work(std::size_t bytes) {
    std::vector<std::uint64_t> buf(bytes / sizeof(std::uint64_t) + 1);
    std::uint64_t sum = 0;
    for (std::size_t i = 0; i < buf.size(); ++i) { buf[i] = (i * 0x9E3779B97F4A7C15ULL) ^ 0x12345678ULL; sum += buf[i] ^ (i * 7); }
    return sum;
}
} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::uint32_t id = 1;
    std::string host = "127.0.0.1";
    std::uint16_t port = 0;
    std::uint64_t footprint = 32 * 1024 * 1024;
    int iterations = 1;
    bool hold = false;
    bool release_at_end = true;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--id") == 0 && i + 1 < argc) id = std::stoul(argv[++i]);
        else if (std::strcmp(a, "--host") == 0 && i + 1 < argc) host = argv[++i];
        else if (std::strcmp(a, "--port") == 0 && i + 1 < argc) port = static_cast<std::uint16_t>(std::stoul(argv[++i]));
        else if (std::strcmp(a, "--footprint") == 0 && i + 1 < argc) footprint = std::stoull(argv[++i]);
        else if (std::strcmp(a, "--iterations") == 0 && i + 1 < argc) iterations = std::stoi(argv[++i]);
        else if (std::strcmp(a, "--hold") == 0) hold = true;
        else if (std::strcmp(a, "--no-release") == 0) release_at_end = false;
        else if (std::strcmp(a, "--marker") == 0 && i + 1 < argc) g_marker = argv[++i];
    }
    if (port == 0) { std::printf("nf-worker: --port is required\n"); return 1; }

    try {
        worker::WorkerSession s;
        if (!s.connect(host, port)) { marker("CONNECT=failed"); std::printf("nf-worker: connect failed\n"); return 2; }
        marker("CONNECT=ok");

        auto reg = s.send_register(WorkerId::from(id), ProcessId::from(GetCurrentProcessId()));
        if (!reg.boot.is_valid()) { marker("REGISTER=failed"); return 3; }
        marker("BOOT=" + reg.boot.to_string() + " epoch=" + std::to_string(reg.epoch));

        auto disc = s.send_discover();
        marker("DISCOVER=nodes=" + std::to_string(disc.node_count) +
               " groups=" + std::to_string(disc.groups) +
               " processors=" + std::to_string(disc.processors) +
               " single=" + std::to_string(disc.single_node ? 1 : 0));

        for (int iter = 0; iter < iterations; ++iter) {
            auto place = s.send_placement(WorkerId::from(id), reg.boot, reg.epoch, footprint, 0, 0, false, false, 0, 0, true, false);
            marker("PLACED=iter=" + std::to_string(iter) + " node=" + std::to_string(place.node) +
                   " kind=" + std::to_string(place.decision) + " penalty=" + std::to_string(place.penalty) +
                   " binding=" + place.binding_constraint);
            if (!place.ok) { marker("PLACE=failed " + s.last_error_message()); return 4; }

            auto rr = s.send_reserve(WorkerId::from(id), reg.boot, place.node, footprint, 2, 0, false);
            marker("RESERVED=iter=" + std::to_string(iter) + " id=" + std::to_string(rr.first));

            auto sum = checksum_work(static_cast<std::size_t>(footprint));
            marker("WORK=iter=" + std::to_string(iter) + " checksum=" + std::to_string(sum));

            if (release_at_end) {
                s.send_release(WorkerId::from(id), reg.boot, rr.first);
                marker("RELEASED=iter=" + std::to_string(iter));
            }
        }
        marker("DONE=true");
        std::printf("nf-worker %u done\n", id);

        if (hold) {
            std::atomic<bool> stop{false};
            while (!stop.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
        }
        s.close();
    } catch (const std::exception& e) {
        marker("ERROR=" + std::string(e.what()));
        return 5;
    }
    return 0;
}
