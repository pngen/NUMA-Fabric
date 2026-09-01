// ============================================================================
// Core model tests: ids, generations, quantities, enums, digest, JSON.
// ============================================================================

#include "numafabric/numafabric.hpp"
#include "test_util.hpp"

#include <cmath>
#include <limits>
#include <type_traits>

using namespace numafabric;

static void test_ids() {
    WorkerId a = WorkerId::from(7);
    WorkerId b = WorkerId::from(7);
    WorkerId c = WorkerId::from(8);
    CHECK(a == b);
    CHECK(a != c);
    CHECK(a.is_valid());
    CHECK(!WorkerId::invalid().is_valid());
    CHECK(a.value() == 7);

    ProcessId p = ProcessId::from(7);
    static_assert(!std::is_same_v<WorkerId, ProcessId>, "id tags must be distinct");
    CHECK(p.value() == 7);

    MemoryGeneration g = MemoryGeneration::initial();
    auto g1 = g.next();
    CHECK(g1.value() == 1);
    CHECK(g1.value() > g.value());

    WorkerBootId b0 = WorkerBootId::from(1);
    WorkerBootId b1 = WorkerBootId::from(2);
    CHECK(b0 != b1);
}

static void test_quantities() {
    Bytes b = Bytes::from(1024);
    CHECK(b.value() == 1024);
    CHECK((b + Bytes::from(1024)).value() == 2048);
    CHECK((b - Bytes::from(4)).value() == 1020);
    CHECK((b * 2).value() == 2048);
    CHECK((b / 2).value() == 512);
    CHECK(b.is_valid());

    CHECK_THROWS(Bytes::from(0) - Bytes::from(5));
    CHECK_THROWS(Utilization::from(std::nan("")));
    CHECK_THROWS(Utilization::from(std::numeric_limits<double>::infinity()));
    CHECK_THROWS(Bytes::from(std::numeric_limits<std::uint64_t>::max()) + Bytes::from(1));

    static_assert(!std::is_same_v<Bytes, Nanoseconds>, "quantity dimensions must be distinct");
    static_assert(!std::is_same_v<Latency, Bytes>, "quantity dimensions must be distinct");
}

static void test_enums() {
    CHECK(to_string(PlacementMode::RequiredNode) == "REQUIRED_NODE");
    CHECK(to_string(LifecycleState::Migrating) == "MIGRATING");
    CHECK(to_string(ProvenanceSource::Synthetic) == "SYNTHETIC");
    CHECK(to_string(LocalityClass::SameNumaNode) == "SAME_NUMA_NODE");
    CHECK(placement_mode_from_string("PREFERRED_NODE") == PlacementMode::PreferredNode);
    CHECK(provenance_from_string("DERIVED") == ProvenanceSource::Derived);
    CHECK_THROWS(placement_mode_from_string("NOT_A_MODE"));
    CHECK_THROWS(lifecycle_state_from_string("BOGUS"));
}

static void test_digest() {
    SemanticDigest d1;
    d1.field("a").u64(1);
    SemanticDigest d2;
    d2.field("a").u64(1);
    CHECK(d1.value() == d2.value());
    SemanticDigest d3;
    d3.field("a").u64(2);
    CHECK(d1.value() != d3.value());
    CHECK(d1.hex().size() == 16);
}

static void test_json() {
    Json j = Json::object();
    j.object_ref()["x"] = Json(42);
    j.object_ref()["name"] = Json("numa");
    j.object_ref()["items"] = Json(Json::array());
    j.object_ref()["items"].array_ref().push_back(Json(1));
    j.object_ref()["items"].array_ref().push_back(Json(2));
    const auto s = j.dump();
    auto parsed = Json::parse(s);
    CHECK(parsed.as_object().at("x").as_number() == 42);
    CHECK(parsed.as_object().at("name").as_string() == "numa");
    CHECK(parsed.as_object().at("items").as_array().size() == 2);
    CHECK_THROWS(Json::parse("{ } junk"));
    CHECK_THROWS(Json::parse("{"));
}

static void run_all() {
    RUN_TEST(test_ids);
    RUN_TEST(test_quantities);
    RUN_TEST(test_enums);
    RUN_TEST(test_digest);
    RUN_TEST(test_json);
}

TEST_MAIN()
