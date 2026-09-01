// ============================================================================
// Protocol & persistence integrity tests: framing, CRC, adversarial decode.
// ============================================================================

#include "numafabric/numafabric.hpp"
#include "test_util.hpp"

#include <cstring>
#include <string>
#include <vector>

using namespace numafabric;

static void test_frame_roundtrip() {
    protocol::WireWriter w;
    w.u64(42);
    w.str("hello");
    auto f = protocol::encode_frame(static_cast<std::uint16_t>(protocol::MsgType::PlacementRequest), w.data());

    protocol::FrameStreamDecoder d;
    std::size_t frames = 0;
    for (std::size_t i = 0; i < f.size(); ++i) {
        auto fr = d.push(f.data() + i, 1);
        if (fr) { ++frames; CHECK(fr->type == static_cast<std::uint16_t>(protocol::MsgType::PlacementRequest)); }
    }
    CHECK(frames == 1);
    CHECK(d.buffered() == 0);
}

static void test_frame_concurrent_frames() {
    protocol::WireWriter a; a.u64(1);
    protocol::WireWriter b; b.u64(2);
    auto fa = protocol::encode_frame(static_cast<std::uint16_t>(protocol::MsgType::ReserveRequest), a.data());
    auto fb = protocol::encode_frame(static_cast<std::uint16_t>(protocol::MsgType::BindRequest), b.data());
    std::vector<std::uint8_t> both;
    both.insert(both.end(), fa.begin(), fa.end());
    both.insert(both.end(), fb.begin(), fb.end());
    protocol::FrameStreamDecoder d;
    std::size_t frames = 0;
    for (std::size_t i = 0; i < both.size(); ++i) {
        auto fr = d.push(both.data() + i, 1);
        if (fr) ++frames;
    }
    CHECK(frames == 2);
}

static void test_adversarial_decode() {
    std::vector<std::uint8_t> junk(protocol::kFrameHeaderSize, 0);
    protocol::FrameStreamDecoder d;
    CHECK_THROWS(d.push(junk.data(), junk.size()));

    protocol::WireWriter w; w.u64(123);
    auto f = protocol::encode_frame(static_cast<std::uint16_t>(protocol::MsgType::Ping), w.data());
    f.back() ^= 0xFF;
    protocol::FrameStreamDecoder d2;
    CHECK_THROWS(d2.push(f.data(), f.size()));

    auto f2 = protocol::encode_frame(static_cast<std::uint16_t>(protocol::MsgType::Ping), w.data());
    protocol::FrameStreamDecoder d3;
    auto part = d3.push(f2.data(), f2.size() - 3);
    CHECK(!part.has_value());
    auto done = d3.push(f2.data() + (f2.size() - 3), 3);
    CHECK(done.has_value());

    protocol::WireWriter w2; w2.u64(1);
    auto fv = protocol::encode_frame(static_cast<std::uint16_t>(protocol::MsgType::Ping), w2.data());
    fv[8] = 0xFF;
    protocol::FrameStreamDecoder d4;
    CHECK_THROWS(d4.push(fv.data(), fv.size()));

    protocol::WireWriter w3; w3.u64(1);
    auto fl = protocol::encode_frame(static_cast<std::uint16_t>(protocol::MsgType::Ping), w3.data());
    fl[12] = 0xFF; fl[13] = 0xFF; fl[14] = 0xFF; fl[15] = 0x7F;
    protocol::FrameStreamDecoder d5;
    CHECK_THROWS(d5.push(fl.data(), fl.size()));
}

static void test_crc32_reference() {
    const char* s = "123456789";
    auto c = protocol::crc32(reinterpret_cast<const std::uint8_t*>(s), 9);
    CHECK(c == 0xCBF43926u);
}

static void test_persistence_roundtrip_and_integrity() {
    persist::PersistedState st;
    st.coordinator_epoch = 3;
    st.host_generation = 2;
    persist::PersistedWorker pw;
    pw.id = WorkerId::from(7);
    pw.boot = WorkerBootId::from(11);
    pw.incarnation = 2;
    pw.alive = true;
    st.workers.push_back(pw);

    persist::PersistedReservation pr;
    pr.id = ReservationId::from(4);
    pr.node = NumaNodeId::from(1);
    pr.bytes = 1024;
    pr.active = true;
    st.reservations.push_back(pr);

    auto bytes = persist::serialize(st);
    auto back = persist::deserialize(bytes);
    CHECK(back.coordinator_epoch == 3);
    CHECK(back.workers.size() == 1);
    CHECK(back.workers[0].id == WorkerId::from(7));
    CHECK(back.workers[0].boot == WorkerBootId::from(11));
    CHECK(back.reservations.size() == 1);
    CHECK(back.reservations[0].bytes == 1024);

    auto bytes2 = persist::serialize(st);
    CHECK(bytes == bytes2);

    auto trunc = bytes;
    trunc.resize(trunc.size() - 4);
    CHECK_THROWS(persist::deserialize(trunc));
    auto trailing = bytes;
    trailing.push_back(0x99);
    CHECK_THROWS(persist::deserialize(trailing));
}

static void run_all() {
    RUN_TEST(test_frame_roundtrip);
    RUN_TEST(test_frame_concurrent_frames);
    RUN_TEST(test_adversarial_decode);
    RUN_TEST(test_crc32_reference);
    RUN_TEST(test_persistence_roundtrip_and_integrity);
}

TEST_MAIN()
