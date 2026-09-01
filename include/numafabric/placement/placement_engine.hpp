#pragma once
// ============================================================================
// NUMA Fabric - deterministic placement engine.
//
// Turns placement into a typed, explained decision. Candidate ranking is NOT an
// opaque master score: every named cost component (memory locality, accelerator
// locality, distance/cost, capacity headroom, migration/rebind cost, policy
// priority, CPU load where measurable) is inspectable, and exactly which hard
// constraint bound the decision is reported. The engine never emits 'no binding
// constraint' when a hard constraint actually exists.
// ============================================================================

#include "numafabric/accelerator/accelerator.hpp"
#include "numafabric/core/enums.hpp"
#include "numafabric/core/ids.hpp"
#include "numafabric/core/provenance.hpp"
#include "numafabric/core/quantities.hpp"
#include "numafabric/topology/topology.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <ostream>
#include <string>
#include <vector>

namespace numafabric {
namespace placement {

// ---------------------------------------------------------------------------
// Placement request
// ---------------------------------------------------------------------------
struct PlacementRequest {
    std::string workload_id;
    AcceleratorId preferred_accelerator = AcceleratorId::invalid();
    Bytes memory_footprint = Bytes::zero();
    NumaNodeId preferred_node = NumaNodeId::invalid();
    NumaNodeId current_node = NumaNodeId::invalid();     // where the worker currently is
    NumaNodeId memory_owner_node = NumaNodeId::invalid(); // node owning the working set
    NumaNodeId io_node = NumaNodeId::invalid();           // NIC/storage locality if known
    NumaNodeId required_node = NumaNodeId::invalid();
    bool has_required_node = false;
    bool require_accelerator_locality = false;   // accelerator must be same node
    bool prefer_accelerator_locality = false;
    std::uint32_t required_cpu_count = 0;        // minimum processors on the node
    bool allow_fallback = true;
    PolicyGeneration policy_generation = PolicyGeneration::invalid();
    bool revalidation_pending = false; // topology/evidence stale -> force revalidation
    bool worker_already_bound = false;
};

// ---------------------------------------------------------------------------
// Candidate cost (inspectable components)
// ---------------------------------------------------------------------------
struct CandidateCost {
    NumaNodeId node = NumaNodeId::invalid();
    bool eligible = true;
    std::map<std::string, LocalityCost> components;
    LocalityCost total = LocalityCost::zero();
    std::vector<std::string> hard_constraint_violations;
    std::uint32_t processor_count = 0;
};

// ---------------------------------------------------------------------------
// Placement model (aggregated runtime facts consumed by the engine)
// ---------------------------------------------------------------------------
struct PlacementModel {
    // accelerator locality by accelerator id
    std::map<AcceleratorId, accel::AcceleratorLocality> accelerator_locality;
    // per-node capacity & free memory (headroom)
    std::map<NumaNodeId, Capacity> node_capacity;
    std::map<NumaNodeId, AvailableMemory> node_free;
    // distance function (default: 0 on diagonal, 100 otherwise)
    std::function<LocalityCost(NumaNodeId, NumaNodeId)> distance =
        [](NumaNodeId a, NumaNodeId b) { return LocalityCost::from(a == b ? 0 : 100); };
    // named policy weights (default 1.0)
    std::map<std::string, double> policy_weight;
    // node set that holds a reservation (headroom already reserved)
    std::map<NumaNodeId, Bytes> reserved_capacity;
};

// ---------------------------------------------------------------------------
// Placement decision (typed output + full explanation)
// ---------------------------------------------------------------------------
struct PlacementDecision {
    PlacementDecisionKind kind = PlacementDecisionKind::Reject;
    NumaNodeId selected_node = NumaNodeId::invalid();
    std::vector<NumaNodeId> eligible_alternatives;
    std::vector<std::string> eliminated_candidates; // "node N: <reason>"
    std::string binding_constraint;   // exact hard constraint that bound the decision ("none" only if none)
    LocalityCost expected_penalty = LocalityCost::zero();
    LocalityClass expected_locality_class = LocalityClass::Unknown;
    Provenance provenance = Provenance::unknown();
    PolicyGeneration policy_generation = PolicyGeneration::invalid();
    PlacementId placement_id = PlacementId::invalid();
    PlacementGeneration placement_generation = PlacementGeneration::invalid();
    std::vector<CandidateCost> ranked_candidates;
    std::string reason;
    std::string would_change; // what would change this decision

    [[nodiscard]] bool is_placement() const noexcept {
        return kind == PlacementDecisionKind::Place || kind == PlacementDecisionKind::PlaceWithPenalty;
    }
};

// ---------------------------------------------------------------------------
// PlacementEngine
// ---------------------------------------------------------------------------
class PlacementEngine {
public:
    PlacementEngine() = default;

    // Deterministic plan. Topology changes -> revalidation_pending -> engine
    // emits REVALIDATION_REQUIRED rather than answering on stale evidence.
    PlacementDecision plan(const topo::HostTopology& topo,
                           const PlacementRequest& req,
                           const PlacementModel& model) const;

    // Helper: expected locality class given the selected node + accelerator.
    LocalityClass classify_locality(NumaNodeId node, const PlacementRequest& req,
                                    const PlacementModel& model) const;

private:
    bool evaluate_eligibility(const topo::NumaNode& node,
                              const PlacementRequest& req,
                              const PlacementModel& model,
                              CandidateCost& cost) const;
    LocalityCost compute_component(const std::string& name, NumaNodeId node,
                                   const PlacementRequest& req,
                                   const PlacementModel& model) const;
};

} // namespace placement
} // namespace numafabric
