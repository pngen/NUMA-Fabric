// ============================================================================
// Deterministic placement engine implementation.
// ============================================================================

#include "numafabric/placement/placement_engine.hpp"

#include <algorithm>
#include <sstream>

namespace numafabric {
namespace placement {

LocalityCost PlacementEngine::compute_component(const std::string& name, NumaNodeId node,
                                                 const PlacementRequest& req,
                                                 const PlacementModel& model) const {
    if (name == "preference") {
        if (!req.preferred_node.is_valid()) return LocalityCost::zero();
        return LocalityCost::from(req.preferred_node == node ? 0 : 1);
    }
    if (name == "memory_local") {
        if (!req.memory_owner_node.is_valid()) return LocalityCost::zero();
        const auto dist = model.distance(req.memory_owner_node, node);
        return LocalityCost::from(dist.value() + (req.memory_owner_node == node ? 0 : 100));
    }
    if (name == "accelerator_local") {
        if (!req.preferred_accelerator.is_valid()) return LocalityCost::zero();
        auto it = model.accelerator_locality.find(req.preferred_accelerator);
        if (it == model.accelerator_locality.end()) return LocalityCost::zero();
        const auto& loc = it->second;
        if (!loc.local_node.is_valid()) return LocalityCost::from(50);
        if (loc.local_node == node) return LocalityCost::zero();
        return LocalityCost::from(200);
    }
    if (name == "distance") {
        if (!req.current_node.is_valid()) return LocalityCost::zero();
        return model.distance(req.current_node, node);
    }
    if (name == "io_local") {
        if (!req.io_node.is_valid()) return LocalityCost::zero();
        return model.distance(req.io_node, node);
    }
    if (name == "capacity") {
        auto fit = model.node_free.find(node);
        if (fit == model.node_free.end()) return LocalityCost::zero();
        const auto free_bytes = fit->second.value();
        const auto need = req.memory_footprint.value();
        if (free_bytes == 0) return LocalityCost::from(500);
        if (need == 0) return LocalityCost::zero();
        const double ratio = static_cast<double>(free_bytes) / static_cast<double>(need);
        if (ratio >= 2.0) return LocalityCost::zero();
        if (ratio < 1.0) return LocalityCost::from(400);
        return LocalityCost::from(static_cast<std::uint64_t>((2.0 - ratio) * 200.0));
    }
    if (name == "migration") {
        if (!req.worker_already_bound) return LocalityCost::zero();
        if (req.current_node == node || !req.current_node.is_valid()) return LocalityCost::zero();
        return LocalityCost::from(150);
    }
    return LocalityCost::zero();
}

bool PlacementEngine::evaluate_eligibility(const topo::NumaNode& node,
                                           const PlacementRequest& req,
                                           const PlacementModel& model,
                                           CandidateCost& cost) const {
    const auto nid = node.id;
    cost.node = nid;
    cost.processor_count = static_cast<std::uint32_t>(node.processor_count());

    if (req.has_required_node && nid != req.required_node) {
        cost.eligible = false;
        cost.hard_constraint_violations.push_back("required_node");
        return false;
    }
    if (req.require_accelerator_locality && req.preferred_accelerator.is_valid()) {
        auto it = model.accelerator_locality.find(req.preferred_accelerator);
        if (it == model.accelerator_locality.end() || !it->second.local_node.is_valid()) {
            cost.eligible = false;
            cost.hard_constraint_violations.push_back("accelerator_locality_unknown");
            return false;
        }
        if (it->second.local_node != nid) {
            cost.eligible = false;
            cost.hard_constraint_violations.push_back("accelerator_locality");
            return false;
        }
    }
    if (req.required_cpu_count > 0 && node.processor_count() < req.required_cpu_count) {
        cost.eligible = false;
        cost.hard_constraint_violations.push_back("required_cpu_count");
        return false;
    }
    const auto cap_it = model.node_capacity.find(nid);
    const auto cap = (cap_it != model.node_capacity.end()) ? cap_it->second.value() : 0ULL;
    const auto res_it = model.reserved_capacity.find(nid);
    const auto reserved = (res_it != model.reserved_capacity.end()) ? res_it->second.value() : 0ULL;
    const std::uint64_t need = req.memory_footprint.value() + reserved;
    if (cap != 0 && need > cap) {
        cost.eligible = false;
        cost.hard_constraint_violations.push_back("capacity");
        return false;
    }
    return true;
}

LocalityClass PlacementEngine::classify_locality(NumaNodeId node,
                                                 const PlacementRequest& req,
                                                 const PlacementModel& model) const {
    if (req.preferred_accelerator.is_valid()) {
        auto it = model.accelerator_locality.find(req.preferred_accelerator);
        if (it != model.accelerator_locality.end()) {
            const auto& loc = it->second;
            if (loc.locality == LocalityClass::Unknown) return LocalityClass::Unknown;
            if (loc.locality == LocalityClass::Synthetic) return LocalityClass::Synthetic;
            if (loc.local_node.is_valid() && loc.local_node == node) return LocalityClass::SameNumaNode;
            return LocalityClass::SameHostRemoteNuma;
        }
    }
    if (req.memory_owner_node.is_valid() && req.memory_owner_node == node) return LocalityClass::SameNumaNode;
    return LocalityClass::SameHostRemoteNuma;
}

PlacementDecision PlacementEngine::plan(const topo::HostTopology& topo,
                                        const PlacementRequest& req,
                                        const PlacementModel& model) const {
    PlacementDecision d;
    d.policy_generation = req.policy_generation;

    if (req.revalidation_pending) {
        d.kind = PlacementDecisionKind::RevalidationRequired;
        d.reason = "topology/locality evidence is stale or not revalidated";
        d.would_change = "revalidate physical topology before placing";
        d.provenance = Provenance::derived("placement engine");
        return d;
    }

    std::vector<CandidateCost> eligible;
    std::vector<std::string> eliminated;
    for (const auto& node : topo.nodes) {
        CandidateCost cost;
        if (!evaluate_eligibility(node, req, model, cost)) {
            std::ostringstream os;
            os << "node " << node.id.value() << ": " << cost.hard_constraint_violations.front();
            eliminated.push_back(os.str());
            continue;
        }
        for (const auto& comp : {"preference", "memory_local", "accelerator_local", "distance", "io_local", "capacity", "migration"}) {
            cost.components[comp] = compute_component(comp, node.id, req, model);
        }
        const double w_pref = model.policy_weight.count("preference") ? model.policy_weight.at("preference") : 100.0;
        const double w_mem = model.policy_weight.count("memory_local") ? model.policy_weight.at("memory_local") : 1.0;
        const double w_accel = model.policy_weight.count("accelerator_local") ? model.policy_weight.at("accelerator_local") : 1.0;
        const double w_dist = model.policy_weight.count("distance") ? model.policy_weight.at("distance") : 1.0;
        const double w_io = model.policy_weight.count("io_local") ? model.policy_weight.at("io_local") : 1.0;
        const double w_cap = model.policy_weight.count("capacity") ? model.policy_weight.at("capacity") : 1.0;
        const double w_mig = model.policy_weight.count("migration") ? model.policy_weight.at("migration") : 1.0;
        const double total = cost.components["preference"].value() * w_pref +
                             cost.components["memory_local"].value() * w_mem +
                             cost.components["accelerator_local"].value() * w_accel +
                             cost.components["distance"].value() * w_dist +
                             cost.components["io_local"].value() * w_io +
                             cost.components["capacity"].value() * w_cap +
                             cost.components["migration"].value() * w_mig;
        cost.total = LocalityCost::from(static_cast<std::uint64_t>(total));
        eligible.push_back(cost);
    }

    std::stable_sort(eligible.begin(), eligible.end(),
                     [](const CandidateCost& a, const CandidateCost& b) {
                         if (a.total.value() != b.total.value()) return a.total.value() < b.total.value();
                         return a.node < b.node;
                     });

    d.ranked_candidates = eligible;
    d.eliminated_candidates = eliminated;

    if (eligible.empty()) {
        if (req.has_required_node) {
            d.kind = PlacementDecisionKind::Reject;
            d.binding_constraint = "required_node";
            d.reason = "required node cannot satisfy the placement constraints";
            d.would_change = "relax the required-node constraint";
        } else if (!eliminated.empty() && eliminated.front().find("capacity") != std::string::npos) {
            d.kind = PlacementDecisionKind::Defer;
            d.binding_constraint = "capacity";
            d.reason = "no node has enough free capacity for the memory footprint";
            d.would_change = "free capacity or reduce the memory footprint";
        } else {
            d.kind = PlacementDecisionKind::Reject;
            d.binding_constraint = eliminated.empty() ? "none" : eliminated.front();
            d.reason = "no eligible node";
            d.would_change = "relax the placement constraints";
        }
        d.provenance = Provenance::derived("placement engine");
        return d;
    }

    const auto& best = eligible.front();
    d.selected_node = best.node;

    for (const auto& c : eligible) {
        if (c.node != best.node) d.eligible_alternatives.push_back(c.node);
    }

    NumaNodeId ideal = req.preferred_node;
    if (!ideal.is_valid() && req.preferred_accelerator.is_valid()) {
        auto it = model.accelerator_locality.find(req.preferred_accelerator);
        if (it != model.accelerator_locality.end() && it->second.local_node.is_valid())
            ideal = it->second.local_node;
    }
    if (!ideal.is_valid()) ideal = req.memory_owner_node;
    if (!ideal.is_valid()) ideal = best.node;

    LocalityCost ideal_cost = LocalityCost::zero();
    for (const auto& c : eligible) {
        if (c.node == ideal) { ideal_cost = c.total; break; }
    }
    d.expected_penalty = (best.total.value() > ideal_cost.value())
                             ? LocalityCost::from(best.total.value() - ideal_cost.value())
                             : LocalityCost::zero();

    if ((req.has_required_node || req.require_accelerator_locality) && best.node != ideal) {
        d.kind = PlacementDecisionKind::Reject;
        d.binding_constraint = "required_node_or_accelerator_locality";
        d.reason = "hard locality preference not met by the best eligible node";
        d.would_change = "relax the hard locality requirement";
        d.provenance = Provenance::derived("placement engine");
        d.expected_locality_class = classify_locality(best.node, req, model);
        return d;
    }

    if (best.node == ideal) {
        d.kind = (req.worker_already_bound && best.node != req.current_node)
                     ? (req.memory_footprint.value() > 0 ? PlacementDecisionKind::Migrate
                                                         : PlacementDecisionKind::Rebind)
                     : PlacementDecisionKind::Place;
        d.expected_penalty = LocalityCost::zero();
    } else if (req.allow_fallback) {
        d.kind = (req.worker_already_bound && req.memory_footprint.value() > 0)
                     ? PlacementDecisionKind::Migrate
                     : (req.worker_already_bound ? PlacementDecisionKind::Rebind
                                                 : PlacementDecisionKind::PlaceWithPenalty);
    } else {
        d.kind = PlacementDecisionKind::Reject;
        d.binding_constraint = "preferred_node";
        d.reason = "fallback disabled and preferred node not the best candidate";
        d.would_change = "enable fallback";
        d.provenance = Provenance::derived("placement engine");
        d.expected_locality_class = classify_locality(best.node, req, model);
        return d;
    }

    if (req.has_required_node) d.binding_constraint = "required_node";
    else if (req.require_accelerator_locality) d.binding_constraint = "accelerator_locality";
    else if (req.required_cpu_count > 0) d.binding_constraint = "required_cpu_count";
    else if (std::any_of(eliminated.begin(), eliminated.end(),
                         [](const std::string& e) { return e.find("capacity") != std::string::npos; }))
        d.binding_constraint = "capacity";
    else d.binding_constraint = "none";

    if (d.binding_constraint == "required_node") d.would_change = "relax required-node";
    else if (d.binding_constraint == "accelerator_locality") d.would_change = "re-derive accelerator locality";
    else if (d.binding_constraint == "required_cpu_count") d.would_change = "raise eligible CPU count";
    else if (d.binding_constraint == "capacity") d.would_change = "free node capacity";
    else d.would_change = "topology/policy change";

    d.expected_locality_class = classify_locality(best.node, req, model);
    if (topo.provenance.source == ProvenanceSource::Synthetic)
        d.provenance = Provenance::synthetic("placement engine");
    else
        d.provenance = Provenance::derived("placement engine");
    return d;
}

} // namespace placement
} // namespace numafabric
