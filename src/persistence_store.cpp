// ============================================================================
// Versioned binary persistence implementation.
// ============================================================================

#include "numafabric/persistence/persistence_store.hpp"
#include "numafabric/protocol/protocol_frame.hpp" // reuse CRC-32

#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>

#include <windows.h>

namespace numafabric {
namespace persist {
namespace {

constexpr std::size_t kHeaderSize = 12 + 4 + 4 + 4;
constexpr std::size_t kMaxPayload = 1u << 28; // 256 MiB

class Writer {
public:
    std::vector<std::uint8_t> buf;
    void u8(std::uint8_t v) { buf.push_back(v); }
    void u16(std::uint16_t v) { buf.push_back(static_cast<std::uint8_t>(v & 0xFF)); buf.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF)); }
    void u32(std::uint32_t v) { for (int i = 0; i < 4; ++i) buf.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF)); }
    void u64(std::uint64_t v) { for (int i = 0; i < 8; ++i) buf.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF)); }
    void str(const std::string& s) { u32(static_cast<std::uint32_t>(s.size())); buf.insert(buf.end(), s.begin(), s.end()); }
    void bytes(const std::vector<std::uint8_t>& v) { u32(static_cast<std::uint32_t>(v.size())); buf.insert(buf.end(), v.begin(), v.end()); }
};

class Reader {
public:
    const std::uint8_t* p;
    std::size_t n;
    std::size_t i = 0;

    Reader(const std::uint8_t* ptr, std::size_t len) : p(ptr), n(len) {}

    void need(std::size_t k) const { if (i + k > n) throw PersistenceError("truncated payload"); }
    std::uint8_t u8() { need(1); return p[i++]; }
    std::uint16_t u16() { need(2); std::uint16_t v = static_cast<std::uint16_t>(p[i] | (p[i+1] << 8)); i += 2; return v; }
    std::uint32_t u32() { need(4); std::uint32_t v = 0; for (int k = 0; k < 4; ++k) v |= (static_cast<std::uint32_t>(p[i+k]) << (8*k)); i += 4; return v; }
    std::uint64_t u64() { need(8); std::uint64_t v = 0; for (int k = 0; k < 8; ++k) v |= (static_cast<std::uint64_t>(p[i+k]) << (8*k)); i += 8; return v; }
    std::string str() {
        const auto len = u32();
        if (len > n - i) throw PersistenceError("truncated string");
        std::string s(reinterpret_cast<const char*>(p + i), len);
        i += len;
        return s;
    }
    std::vector<std::uint8_t> bytes() {
        const auto len = u32();
        if (len > n - i) throw PersistenceError("truncated byte blob");
        std::vector<std::uint8_t> v(p + i, p + i + len);
        i += len;
        return v;
    }
    bool at_end() const { return i == n; }
};

void write_worker(Writer& w, const PersistedWorker& x) {
    w.u64(x.id.value()); w.u64(x.boot.value()); w.u64(x.incarnation); w.u8(x.alive ? 1 : 0);
    w.u64(x.node.value()); w.u64(x.placement_generation.value()); w.u64(x.binding_generation.value());
    w.u64(x.policy_generation.value()); w.u64(x.live_generation);
}
PersistedWorker read_worker(Reader& r) {
    PersistedWorker x;
    x.id = WorkerId::from(r.u64()); x.boot = WorkerBootId::from(r.u64()); x.incarnation = r.u64();
    x.alive = r.u8() != 0; x.node = NumaNodeId::from(r.u64());
    x.placement_generation = PlacementGeneration::from(r.u64()); x.binding_generation = BindingGeneration::from(r.u64());
    x.policy_generation = PolicyGeneration::from(r.u64()); x.live_generation = r.u64();
    if (!x.id.is_valid()) throw PersistenceError("invalid worker id");
    return x;
}
void write_res(Writer& w, const PersistedReservation& x) {
    w.u64(x.id.value()); w.u64(x.node.value()); w.u64(x.bytes); w.u32(x.cpu_slots);
    w.u64(x.accelerator.value()); w.u8(x.accelerator_capacity ? 1 : 0);
    w.u64(x.generation.value()); w.u8(x.active ? 1 : 0);
}
PersistedReservation read_res(Reader& r) {
    PersistedReservation x;
    x.id = ReservationId::from(r.u64()); x.node = NumaNodeId::from(r.u64()); x.bytes = r.u64();
    x.cpu_slots = r.u32(); x.accelerator = AcceleratorId::from(r.u64()); x.accelerator_capacity = r.u8() != 0;
    x.generation = ReservationGeneration::from(r.u64()); x.active = r.u8() != 0;
    if (!x.id.is_valid()) throw PersistenceError("invalid reservation id");
    return x;
}
void write_place(Writer& w, const PersistedPlacement& x) {
    w.u64(x.id.value()); w.u64(x.generation.value()); w.u64(x.worker.value()); w.u64(x.worker_boot.value());
    w.u64(x.node.value()); w.u8(static_cast<std::uint8_t>(x.source)); w.u64(x.committed_generation);
}
PersistedPlacement read_place(Reader& r) {
    PersistedPlacement x;
    x.id = PlacementId::from(r.u64()); x.generation = PlacementGeneration::from(r.u64());
    x.worker = WorkerId::from(r.u64()); x.worker_boot = WorkerBootId::from(r.u64());
    x.node = NumaNodeId::from(r.u64());
    const auto src = r.u8();
    if (src > static_cast<std::uint8_t>(ProvenanceSource::Unknown)) throw PersistenceError("malformed provenance enum");
    x.source = static_cast<ProvenanceSource>(src);
    x.committed_generation = r.u64();
    if (!x.id.is_valid()) throw PersistenceError("invalid placement id");
    return x;
}
void write_obs(Writer& w, const PersistedObservation& x) {
    w.u64(x.id.value()); w.u64(x.generation.value()); w.u64(x.worker.value()); w.u64(x.boot.value());
    w.u64(x.node.value()); w.u8(static_cast<std::uint8_t>(x.source)); w.u8(x.stale ? 1 : 0);
}
PersistedObservation read_obs(Reader& r) {
    PersistedObservation x;
    x.id = ObservationId::from(r.u64()); x.generation = ObservationGeneration::from(r.u64());
    x.worker = WorkerId::from(r.u64()); x.boot = WorkerBootId::from(r.u64()); x.node = NumaNodeId::from(r.u64());
    const auto src = r.u8();
    if (src > static_cast<std::uint8_t>(ProvenanceSource::Unknown)) throw PersistenceError("malformed provenance enum");
    x.source = static_cast<ProvenanceSource>(src);
    x.stale = r.u8() != 0;
    if (!x.id.is_valid()) throw PersistenceError("invalid observation id");
    return x;
}

} // namespace

std::vector<std::uint8_t> serialize(const PersistedState& state) {
    Writer w;
    w.u64(state.coordinator_epoch);
    w.u64(state.host_generation);
    w.u64(state.policy_generation);
    if (state.workers.size() > kMaxPersistedRecords) throw PersistenceError("too many workers");
    w.u32(static_cast<std::uint32_t>(state.workers.size()));
    for (const auto& x : state.workers) write_worker(w, x);
    if (state.reservations.size() > kMaxPersistedRecords) throw PersistenceError("too many reservations");
    w.u32(static_cast<std::uint32_t>(state.reservations.size()));
    for (const auto& x : state.reservations) write_res(w, x);
    if (state.placements.size() > kMaxPersistedRecords) throw PersistenceError("too many placements");
    w.u32(static_cast<std::uint32_t>(state.placements.size()));
    for (const auto& x : state.placements) write_place(w, x);
    if (state.observations.size() > kMaxPersistedRecords) throw PersistenceError("too many observations");
    w.u32(static_cast<std::uint32_t>(state.observations.size()));
    for (const auto& x : state.observations) write_obs(w, x);
    return std::move(w.buf);
}

PersistedState deserialize(const std::vector<std::uint8_t>& data) {
    if (data.size() > kMaxPayload) throw PersistenceError("payload too large");
    Reader r(data.data(), data.size());
    PersistedState s;
    s.coordinator_epoch = r.u64();
    s.host_generation = r.u64();
    s.policy_generation = r.u64();

    const auto wc = r.u32();
    if (wc > kMaxPersistedRecords) throw PersistenceError("impossible worker count");
    for (std::uint32_t i = 0; i < wc; ++i) s.workers.push_back(read_worker(r));

    const auto rc = r.u32();
    if (rc > kMaxPersistedRecords) throw PersistenceError("impossible reservation count");
    for (std::uint32_t i = 0; i < rc; ++i) s.reservations.push_back(read_res(r));

    const auto pc = r.u32();
    if (pc > kMaxPersistedRecords) throw PersistenceError("impossible placement count");
    for (std::uint32_t i = 0; i < pc; ++i) s.placements.push_back(read_place(r));

    const auto oc = r.u32();
    if (oc > kMaxPersistedRecords) throw PersistenceError("impossible observation count");
    for (std::uint32_t i = 0; i < oc; ++i) s.observations.push_back(read_obs(r));

    if (!r.at_end()) throw PersistenceError("trailing garbage after persisted state");
    return s;
}

void save(const PersistedState& state, const std::string& path) {
    const auto payload = serialize(state);
    std::vector<std::uint8_t> container;
    container.insert(container.end(), kContainerMagic, kContainerMagic + 12);
    for (int i = 0; i < 4; ++i) container.push_back(static_cast<std::uint8_t>((kContainerVersion >> (8 * i)) & 0xFF));
    for (int i = 0; i < 4; ++i) container.push_back(static_cast<std::uint8_t>((payload.size() >> (8 * i)) & 0xFF));
    std::vector<std::uint8_t> tohash = container;
    tohash.insert(tohash.end(), payload.begin(), payload.end());
    const auto crc = protocol::crc32(tohash.data(), tohash.size());
    for (int i = 0; i < 4; ++i) container.push_back(static_cast<std::uint8_t>((crc >> (8 * i)) & 0xFF));
    container.insert(container.end(), payload.begin(), payload.end());

    const auto tmp = path + ".tmp";
    {
        std::ofstream os(tmp, std::ios::binary);
        if (!os) throw PersistenceError("cannot open temp file for write");
        os.write(reinterpret_cast<const char*>(container.data()), static_cast<std::streamsize>(container.size()));
        os.flush();
        if (!os) throw PersistenceError("write failed while persisting state");
        os.close();
    }
    // Atomic replace: rename temp over the destination.
    if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw PersistenceError("atomic rename failed");
    }
}

PersistedState load(const std::string& path) {
    std::ifstream is(path, std::ios::binary);
    if (!is) throw PersistenceError("cannot open persisted state");
    std::vector<std::uint8_t> container((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
    if (container.size() < kHeaderSize) throw PersistenceError("persisted container truncated");
    if (std::memcmp(container.data(), kContainerMagic, 12) != 0) throw PersistenceError("persisted magic mismatch");
    std::uint32_t version = 0;
    for (int i = 0; i < 4; ++i) version |= (static_cast<std::uint32_t>(container[12 + i]) << (8 * i));
    if (version != kContainerVersion) throw PersistenceError("unsupported persistence version");
    std::uint32_t payload_len = 0;
    for (int i = 0; i < 4; ++i) payload_len |= (static_cast<std::uint32_t>(container[16 + i]) << (8 * i));
    if (payload_len > kMaxPayload) throw PersistenceError("impossible persisted payload length");
    if (container.size() != kHeaderSize + payload_len) throw PersistenceError("persisted length mismatch (truncation/trailing garbage)");
    std::uint32_t expected_crc = 0;
    for (int i = 0; i < 4; ++i) expected_crc |= (static_cast<std::uint32_t>(container[20 + i]) << (8 * i));
    // recompute over header+payload
    // CRC covers header (magic+version+length, without the crc field) + payload.
    std::vector<std::uint8_t> tohash(container.begin(), container.begin() + (kHeaderSize - 4));
    tohash.insert(tohash.end(), container.begin() + kHeaderSize, container.end());
    const auto crc = protocol::crc32(tohash.data(), tohash.size());
    if (crc != expected_crc) throw PersistenceError("persisted checksum corruption");
    std::vector<std::uint8_t> payload(container.begin() + kHeaderSize, container.end());
    return deserialize(payload);
}

} // namespace persist
} // namespace numafabric
