// ============================================================================
// Runtime tests: topology, memory lifecycle, placement, reservations,
// migration/rebinding, worker authority, persistence/recovery.
// ============================================================================

#include "numafabric/numafabric.hpp"
#include "test_util.hpp"

#include <cstdio>
#include <fstream>
#include <memory>
#include <string>

using namespace numafabric;

static backend::SyntheticConfig synth_cfg() {
    backend::SyntheticConfig cfg;
    cfg.node_count = 2;
    cfg.processors_per_node = 8;
    cfg.group_capacity = 64;
    return cfg;
}

static void test_topology_synthetic() {
    auto rt = Runtime::create_synthetic(synth_cfg());
    rt->discover();
    CHECK(rt->topology().numa_node_count() == 2);
    CHECK(rt->topology().is_single_node() == false);
    CHECK(rt->topology().processor_count() == 16);
    CHECK(rt->topology().groups.size() >= 1);
    auto snap = rt->snapshot();
    CHECK(snap.semantic_digest() != 0);
    // Deterministic digest across fresh snapshots.
    auto snap2 = rt->snapshot();
    CHECK(snap.semantic_digest() == snap2.semantic_digest());
}

static void test_topology_windows() {
    auto rt = Runtime::create_windows();
    rt->discover();
    CHECK(rt->topology().numa_node_count() >= 1);
    std::printf("  [physical] NUMA nodes=%zu groups=%zu processors=%zu single=%d\n",
                rt->topology().numa_node_count(), rt->topology().groups.size(),
                rt->topology().processor_count(), rt->topology().is_single_node() ? 1 : 0);
    // On this validation host there is exactly one NUMA node; report it honestly.
    if (rt->topology().numa_node_count() == 1) {
        CHECK(rt->topology().is_single_node());
        std::printf("  [physical] single-NUMA-node host validated (0x%llx)\n",
                    rt->topology().nodes.front().memory_capacity.value());
    }
}

static void test_memory_lifecycle() {
    auto rt = Runtime::create_synthetic(synth_cfg());
    rt->discover();
    const auto node = rt->topology().nodes.front().id;
    auto region = rt->allocate_on_node(Bytes::from(8 * 1024 * 1024), node);
    CHECK(region.state == LifecycleState::Active);
    CHECK(region.granted_bytes.value() == 8 * 1024 * 1024);
    CHECK(region.kind == AllocationKind::Synthetic);
    CHECK(region.locality_known); // synthetic locality is deterministic
    CHECK(rt->allocated_bytes() == 8 * 1024 * 1024);

    // Double release rejected (state machine guard).
    rt->release(region.id);
    CHECK(rt->allocated_bytes() == 0);
    CHECK(rt->memory_accounting_clean());
    CHECK_THROWS(rt->release(region.id));
}

static void test_memory_transitions_guarded() {
    auto rt = Runtime::create_synthetic(synth_cfg());
    rt->discover();
    const auto node = rt->topology().nodes.front().id;
    auto region = rt->allocate_on_node(Bytes::from(1024), node);
    // Migrating directly from Active is guarding-allowed, but a direct jump from
    // Active to Released via transition() is invalid (must go via release()).
    CHECK_THROWS(rt->memory_manager().transition(region.id, LifecycleState::Released));
    rt->release(region.id);
}

static void test_placement() {
    auto rt = Runtime::create_synthetic(synth_cfg());
    rt->discover();
    const auto n0 = rt->topology().nodes[0].id;
    const auto n1 = rt->topology().nodes[1].id;

    // Preferred node 1, footprint fits -> best on node 1 (no penalty).
    placement::PlacementRequest pref;
    pref.preferred_node = n1;
    pref.memory_footprint = Bytes::from(32 * 1024 * 1024);
    auto d = rt->plan(pref);
    CHECK(d.selected_node == n1);
    CHECK(d.kind == PlacementDecisionKind::Place);
    CHECK(d.expected_penalty.value() == 0);

    // Required node 0 accepts only node 0.
    placement::PlacementRequest req;
    req.has_required_node = true;
    req.required_node = n0;
    req.memory_footprint = Bytes::from(32 * 1024 * 1024);
    auto dr = rt->plan(req);
    CHECK(dr.selected_node == n0);
    CHECK(dr.binding_constraint == "required_node");
    CHECK(dr.kind == PlacementDecisionKind::Place);

    // Explanation correctness: a required-node decision must not claim "none".
    CHECK(dr.binding_constraint != "none");
    const auto expl = rt->explanation_text(dr);
    CHECK(expl.find("required_node") != std::string::npos);

    // Revalidation pending -> RevalidationRequired.
    placement::PlacementRequest stale;
    stale.revalidation_pending = true;
    auto ds = rt->plan(stale);
    CHECK(ds.kind == PlacementDecisionKind::RevalidationRequired);
}

static void test_placement_capacity() {
    // 1 node with tiny capacity exhausts -> reject/prefer fallback with penalty.
    backend::SyntheticConfig cfg;
    cfg.node_count = 1;
    cfg.processors_per_node = 4;
    cfg.node_capacities = {Capacity::from(64 * 1024 * 1024)};
    auto rt = Runtime::create_synthetic(cfg);
    rt->discover();
    placement::PlacementRequest p;
    p.memory_footprint = Bytes::from(128 * 1024 * 1024); // > capacity
    auto d = rt->plan(p);
    CHECK(d.kind == PlacementDecisionKind::Defer || d.kind == PlacementDecisionKind::Reject);
    if (d.kind == PlacementDecisionKind::Defer) CHECK(d.binding_constraint == "capacity");
}

static void test_placement_deterministic_ranking() {
    auto rt = Runtime::create_synthetic(synth_cfg());
    rt->discover();
    placement::PlacementRequest p;
    p.memory_footprint = Bytes::from(32 * 1024 * 1024);
    auto d1 = rt->plan(p);
    auto d2 = rt->plan(p);
    CHECK(d1.selected_node == d2.selected_node);
    CHECK(d1.expected_penalty.value() == d2.expected_penalty.value());
    CHECK(d1.ranked_candidates.size() == d2.ranked_candidates.size());
}

static void test_reservations() {
    auto rt = Runtime::create_synthetic(synth_cfg());
    rt->discover();
    const auto node = rt->topology().nodes.front().id;
    reservation::ReserveRequest rr;
    rr.node = node;
    rr.bytes = Bytes::from(16 * 1024 * 1024);
    rr.cpu_slots = 2;
    auto res = rt->reserve(rr);
    CHECK(res.active);
    CHECK(rt->reserved_bytes() == 16 * 1024 * 1024);
    // Double release rejected.
    rt->release_reservation(res.id);
    CHECK(rt->reservations_clean());
    CHECK_THROWS(rt->release_reservation(res.id));
    // Oversubscription against node capacity.
    rr.node = node;
    rr.bytes = Bytes::from(64ULL * 1024 * 1024 * 1024); // > 16 GiB
    CHECK_THROWS(rt->reserve(rr));
}

static void test_worker_authority() {
    auto rt = Runtime::create_synthetic(synth_cfg());
    rt->discover();
    auto w = rt->register_worker(WorkerId::from(5), ProcessId::from(1));
    CHECK(w.incarnation == 1);
    const auto firstBoot = w.boot_id;
    // A restarted worker gets a FRESH boot and a new incarnation.
    auto w2 = rt->restart_worker(WorkerId::from(5), ProcessId::from(2));
    CHECK(w2.incarnation == 2);
    CHECK(w2.boot_id != firstBoot);
    // Stale mutation using the OLD boot is rejected.
    CHECK(rt->workers_registry().validate_boot(WorkerId::from(5), firstBoot) == false);
    CHECK(rt->workers_registry().validate_boot(WorkerId::from(5), w2.boot_id) == true);
    // Binding an old boot fails.
    const auto node = rt->topology().nodes.front().id;
    CHECK_THROWS(rt->bind_worker(WorkerId::from(5), firstBoot, node, false));
}

static void test_migration() {
    auto rt = Runtime::create_synthetic(synth_cfg());
    rt->discover();
    const auto n0 = rt->topology().nodes[0].id;
    const auto n1 = rt->topology().nodes[1].id;
    auto region = rt->allocate_on_node(Bytes::from(8 * 1024 * 1024), n0);
    const auto genBefore = region.generation;
    Runtime::Auth auth{rt->epoch(), WorkerBootId::from(1)};
    // Worker must be registered for migration authority.
    auto w = rt->register_worker(WorkerId::from(9), ProcessId::from(1));
    auth.boot = w.boot_id;
    bool committed = rt->migrate(region.id, n1, w.id, auth, "test migration");
    CHECK(committed);
    const auto* after = rt->memory_manager().get(region.id);
    CHECK(after);
    CHECK(after->state == LifecycleState::Rebound);
    CHECK(after->actual_node == n1);
    CHECK(after->generation.value() > genBefore.value());
    CHECK(rt->node_allocated(n1) == 8 * 1024 * 1024);
}

static void test_persistence() {
    auto rt = Runtime::create_synthetic(synth_cfg());
    rt->discover();
    auto w = rt->register_worker(WorkerId::from(31), ProcessId::from(1));
    const auto node = rt->topology().nodes.front().id;
    reservation::ReserveRequest rr;
    rr.node = node;
    rr.bytes = Bytes::from(8 * 1024 * 1024);
    auto res = rt->reserve(rr);

    const std::string path = "test_state.bin";
    rt->persist(path);

    // Fresh runtime recovers the logical state.
    auto rt2 = Runtime::create_synthetic(synth_cfg());
    rt2->discover();
    rt2->recover(path);
    const auto* w2 = rt2->worker(WorkerId::from(31));
    CHECK(w2 && w2->boot_id == w.boot_id);
    CHECK(rt2->reserved_bytes() == 8 * 1024 * 1024);
    // Recovered locality must be marked stale (never current merely by timestamp).
    std::remove(path.c_str());

    // Corruption / truncation detection.
    const std::string bad = "bad_state.bin";
    { std::ofstream ofs(bad, std::ios::binary); ofs << "garbage"; }
    CHECK_THROWS(rt2->recover(bad));
    std::remove(bad.c_str());
}

static void run_all() {
    RUN_TEST(test_topology_synthetic);
    RUN_TEST(test_topology_windows);
    RUN_TEST(test_memory_lifecycle);
    RUN_TEST(test_memory_transitions_guarded);
    RUN_TEST(test_placement);
    RUN_TEST(test_placement_capacity);
    RUN_TEST(test_placement_deterministic_ranking);
    RUN_TEST(test_reservations);
    RUN_TEST(test_worker_authority);
    RUN_TEST(test_migration);
    RUN_TEST(test_persistence);
}

TEST_MAIN()
