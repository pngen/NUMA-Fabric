// ============================================================================
// Synthetic multi-NUMA scenario tests.
//
// The physical host exposes a single NUMA node; these scenarios model
// multi-socket / multi-NUMA / multi-accelerator systems deterministically so
// the production decision paths can be validated. Provenance is SYNTHETIC and
// never presented as physical validation.
// ============================================================================

#include "numafabric/numafabric.hpp"
#include "test_util.hpp"

#include <memory>
#include <string>

using namespace numafabric;

static backend::SyntheticConfig cfg2() {
    backend::SyntheticConfig cfg;
    cfg.node_count = 2;
    cfg.processors_per_node = 8;
    cfg.group_capacity = 64;
    return cfg;
}

static void scenario_gpu_local_to_each_node() {
    auto cfg = cfg2();
    cfg.accelerators.push_back(backend::SyntheticConfig::AcceleratorPlacement{LocalityClass::SameNumaNode, NumaNodeId::from(1)});
    cfg.accelerators.push_back(backend::SyntheticConfig::AcceleratorPlacement{LocalityClass::SameNumaNode, NumaNodeId::from(2)});
    auto rt = Runtime::create_synthetic(cfg);
    rt->discover();
    rt->register_accelerators(rt->refresh_accelerators());
    placement::PlacementRequest p;
    p.workload_id = "gpu-local";
    p.memory_footprint = Bytes::from(8 * 1024 * 1024);
    p.preferred_accelerator = AcceleratorId::from(1);
    p.prefer_accelerator_locality = true;
    auto d = rt->plan(p);
    CHECK(d.selected_node == NumaNodeId::from(1));
    CHECK(d.expected_locality_class == LocalityClass::SameNumaNode);
}

static void scenario_required_node_rejects_fallback() {
    auto rt = Runtime::create_synthetic(cfg2());
    rt->discover();
    placement::PlacementRequest p;
    p.has_required_node = true;
    p.required_node = NumaNodeId::from(2);
    p.memory_footprint = Bytes::from(8 * 1024 * 1024);
    p.allow_fallback = false;
    auto d = rt->plan(p);
    CHECK(d.selected_node == NumaNodeId::from(2));
    CHECK(d.binding_constraint == "required_node");
    CHECK(d.expected_penalty.value() == 0);
}

static void scenario_preferred_fallback_penalty() {
    auto rt = Runtime::create_synthetic(cfg2());
    rt->discover();
    placement::PlacementRequest p;
    p.preferred_node = NumaNodeId::from(1);
    p.memory_footprint = Bytes::from(8 * 1024 * 1024);
    p.allow_fallback = true;
    auto d = rt->plan(p);
    CHECK(d.selected_node == NumaNodeId::from(1));
    CHECK(d.kind == PlacementDecisionKind::Place);
    CHECK(d.expected_penalty.value() == 0);
}

static void scenario_capacity_exhaustion() {
    backend::SyntheticConfig cfg;
    cfg.node_count = 2;
    cfg.processors_per_node = 4;
    cfg.node_capacities = {Capacity::from(16 * 1024 * 1024), Capacity::from(16 * 1024 * 1024)};
    auto rt = Runtime::create_synthetic(cfg);
    rt->discover();
    placement::PlacementRequest p;
    p.memory_footprint = Bytes::from(64 * 1024 * 1024);
    p.allow_fallback = true;
    auto d = rt->plan(p);
    CHECK(d.kind == PlacementDecisionKind::Defer || d.kind == PlacementDecisionKind::Reject);
}

static void scenario_unknown_accelerator_locality() {
    auto cfg = cfg2();
    cfg.accelerators.push_back(backend::SyntheticConfig::AcceleratorPlacement{LocalityClass::Unknown, NumaNodeId::invalid()});
    auto rt = Runtime::create_synthetic(cfg);
    rt->discover();
    rt->refresh_accelerators();
    placement::PlacementRequest p;
    p.preferred_accelerator = AcceleratorId::from(1);
    p.prefer_accelerator_locality = true;
    p.memory_footprint = Bytes::from(8 * 1024 * 1024);
    auto d = rt->plan(p);
    CHECK(d.expected_locality_class == LocalityClass::Unknown || d.kind == PlacementDecisionKind::Reject);
}

static void scenario_topology_generation_rollover() {
    auto rt = Runtime::create_synthetic(cfg2());
    rt->discover();
    placement::PlacementRequest p;
    p.memory_footprint = Bytes::from(8 * 1024 * 1024);
    auto d0 = rt->plan(p);
    CHECK(d0.kind == PlacementDecisionKind::Place || d0.kind == PlacementDecisionKind::PlaceWithPenalty);
    rt->memory_manager().invalidate_locality_stale();
    placement::PlacementRequest p2;
    p2.revalidation_pending = true;
    p2.memory_footprint = Bytes::from(8 * 1024 * 1024);
    auto d1 = rt->plan(p2);
    CHECK(d1.kind == PlacementDecisionKind::RevalidationRequired);
}

static void scenario_worker_restart_incarnation() {
    auto rt = Runtime::create_synthetic(cfg2());
    rt->discover();
    auto w = rt->register_worker(WorkerId::from(3), ProcessId::from(1));
    auto b1 = w.boot_id;
    auto w2 = rt->restart_worker(WorkerId::from(3), ProcessId::from(2));
    CHECK(w2.incarnation == 2);
    CHECK(w2.boot_id != b1);
    const auto node = rt->topology().nodes.front().id;
    auto b = rt->bind_worker(WorkerId::from(3), w2.boot_id, node, false);
    CHECK(b.binding_id.is_valid());
}

static void scenario_unequal_capacity_ranking() {
    backend::SyntheticConfig cfg;
    cfg.node_count = 2;
    cfg.processors_per_node = 8;
    cfg.node_capacities = {Capacity::from(8 * 1024 * 1024), Capacity::from(64 * 1024 * 1024)};
    auto rt = Runtime::create_synthetic(cfg);
    rt->discover();
    placement::PlacementRequest p;
    p.memory_footprint = Bytes::from(16 * 1024 * 1024);
    p.allow_fallback = true;
    auto d = rt->plan(p);
    CHECK(d.selected_node == NumaNodeId::from(2));
    bool has_capacity_violation = false;
    for (const auto& e : d.eliminated_candidates) {
        if (e.find("capacity") != std::string::npos) has_capacity_violation = true;
    }
    CHECK(has_capacity_violation);
}

static void scenario_concurrent_reservations_across_nodes() {
    auto rt = Runtime::create_synthetic(cfg2());
    rt->discover();
    reservation::ReserveRequest r1;
    r1.node = NumaNodeId::from(1);
    r1.bytes = Bytes::from(4 * 1024 * 1024);
    auto a = rt->reserve(r1);
    reservation::ReserveRequest r2;
    r2.node = NumaNodeId::from(2);
    r2.bytes = Bytes::from(4 * 1024 * 1024);
    auto b = rt->reserve(r2);
    CHECK(rt->node_reserved(NumaNodeId::from(1)) == 4 * 1024 * 1024);
    CHECK(rt->node_reserved(NumaNodeId::from(2)) == 4 * 1024 * 1024);
    rt->release_reservation(a.id);
    rt->release_reservation(b.id);
    CHECK(rt->reservations_clean());
}

static void scenario_remote_memory_penalty() {
    auto rt = Runtime::create_synthetic(cfg2());
    rt->discover();
    placement::PlacementRequest p;
    p.memory_owner_node = NumaNodeId::from(1);
    p.memory_footprint = Bytes::from(8 * 1024 * 1024);
    auto d = rt->plan(p);
    CHECK(d.selected_node == NumaNodeId::from(1));
}

static void scenario_sparse_membership_groups() {
    backend::SyntheticConfig cfg;
    cfg.node_count = 2;
    cfg.processors_per_node = 4;
    cfg.group_capacity = 8;
    auto rt = Runtime::create_synthetic(cfg);
    rt->discover();
    CHECK(rt->topology().processor_count() == 8);
    CHECK(rt->topology().numa_node_count() == 2);
    for (const auto& p : rt->topology().processors_) {
        CHECK(p.group.is_valid());
        CHECK(p.node.is_valid());
        CHECK(rt->topology().find_node(p.node) != nullptr);
    }
}

static void run_all() {
    RUN_TEST(scenario_gpu_local_to_each_node);
    RUN_TEST(scenario_required_node_rejects_fallback);
    RUN_TEST(scenario_preferred_fallback_penalty);
    RUN_TEST(scenario_capacity_exhaustion);
    RUN_TEST(scenario_unknown_accelerator_locality);
    RUN_TEST(scenario_topology_generation_rollover);
    RUN_TEST(scenario_worker_restart_incarnation);
    RUN_TEST(scenario_unequal_capacity_ranking);
    RUN_TEST(scenario_concurrent_reservations_across_nodes);
    RUN_TEST(scenario_remote_memory_penalty);
    RUN_TEST(scenario_sparse_membership_groups);
}

TEST_MAIN()
