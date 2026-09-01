// ============================================================================
// nf-coordinator - a real NUMA Fabric coordinator OS process.
//
// Serves the framed-TCP authority model. Binds to a port, prints "LISTENING
// <port>", and serves until killed (or sent a Shutdown frame). May --recover a
// persisted authoritative state on startup so a fresh coordinator process can
// bring logical state back and require physical evidence revalidation.
// ============================================================================

#include "numafabric/numafabric.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

using namespace numafabric;

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string backend = "synthetic";
    std::uint32_t nodes = 2;
    std::uint32_t procs = 8;
    std::uint16_t requested_port = 0;
    std::string recover_path;
    std::string port_file;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--backend") == 0 && i + 1 < argc) backend = argv[++i];
        else if (std::strcmp(a, "--nodes") == 0 && i + 1 < argc) nodes = std::stoul(argv[++i]);
        else if (std::strcmp(a, "--procs") == 0 && i + 1 < argc) procs = std::stoul(argv[++i]);
        else if (std::strcmp(a, "--port") == 0 && i + 1 < argc) requested_port = static_cast<std::uint16_t>(std::stoul(argv[++i]));
        else if (std::strcmp(a, "--recover") == 0 && i + 1 < argc) recover_path = argv[++i];
        else if (std::strcmp(a, "--port-file") == 0 && i + 1 < argc) port_file = argv[++i];
    }

    std::unique_ptr<backend::Backend> bk;
    if (backend == "synthetic") {
        backend::SyntheticConfig cfg;
        cfg.node_count = nodes;
        cfg.processors_per_node = procs;
        cfg.group_capacity = 64;
        bk = backend::make_synthetic_backend(cfg);
    } else {
        bk = backend::make_windows_backend();
    }

    coord::Coordinator coordinator(std::move(bk));
    coordinator.runtime().discover();
    coordinator.runtime().refresh_accelerators();

    if (!recover_path.empty()) {
        coordinator.recover(recover_path);
        std::printf("RECOVERED=%s\n", recover_path.c_str());
    }

    auto port = coordinator.bind(requested_port);
    std::printf("LISTENING %u\n", port);
    std::fflush(stdout);
    if (!port_file.empty()) {
        std::ofstream pf(port_file, std::ios::trunc);
        pf << port << "\n";
        pf.close();
    }

    while (coordinator.running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    coordinator.shutdown();
    std::printf("STOPPED\n");
    return 0;
}
