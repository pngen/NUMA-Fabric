// ============================================================================
// Runtime facade implementation.
// ============================================================================

#include "numafabric/runtime/runtime.hpp"

#include "numafabric/core/digest.hpp"
#include "numafabric/core/freshness.hpp"
#include "numafabric/core/json.hpp"
#include "numafabric/persistence/persistence_store.hpp"

#include <algorithm>
#include <sstream>

namespace numafabric {

std::function<Capacity(NumaNodeId)> Runtime::node_capacity_provider() const {
    return [this](NumaNodeId node) -> Capacity {
        for (const auto& n : topo_.nodes) {
            if (n.id == node) return n.memory_capacity;
        }
        return Capacity::zero();
    };
}

placement::PlacementModel Runtime::build_placement_model() const {
    placement::PlacementModel model;
    model.distance = [this](NumaNodeId a, NumaNodeId b) -> LocalityCost {
        if (a == b) return LocalityCost::zero();
        if (backend_->kind() == backend::BackendKind::Synthetic) {
            const auto av = a.value();
            const auto bv = b.value();
            const auto diff = av > bv ? av - bv : bv - av;
            return LocalityCost::from(40 * diff + 60);
        }
        return LocalityCost::from(100);
    };
    for (const auto& n : topo_.nodes) {
        model.node_capacity[n.id] = n.memory_capacity;
        model.node_free[n.id] = n.memory_info_available ? n.free_memory : AvailableMemory::from(0);
    }
    for (const auto& a : accelerators_) model.accelerator_locality[a.id] = a.locality;
    const auto all_res = reservations_.all();
    for (const auto& r : all_res) {
        if (r.active) model.reserved_capacity[r.node] = model.reserved_capacity[r.node] + r.bytes;
    }
    return model;
}

Runtime::Runtime(std::unique_ptr<backend::Backend> backend)
    : backend_(std::move(backend)),
      memory_(*backend_),
      reservations_(node_capacity_provider()),
      migrator_(memory_) {}

Runtime::~Runtime() = default;

std::unique_ptr<Runtime> Runtime::create_windows() {
    return std::make_unique<Runtime>(backend::make_windows_backend());
}
std::unique_ptr<Runtime> Runtime::create_synthetic(const backend::SyntheticConfig& cfg) {
    return std::make_unique<Runtime>(backend::make_synthetic_backend(cfg));
}

topo::HostTopology Runtime::discover() {
    auto found = backend_->discover_host();
    {
        std::lock_guard<std::mutex> lk(mutex_);
        topo_ = std::move(found);
    }
    return topo_;
}

topo::TopologySnapshot Runtime::snapshot() const {
    topo::TopologySnapshot s;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        s.host = topo_;
    }
    s.digest = s.host.semantic_digest().value();
    s.snapshot_id = SnapshotId::from(1);
    return s;
}

std::vector<accel::AcceleratorInfo> Runtime::refresh_accelerators() {
    auto found = backend_->enumerate_accelerators();
    {
        std::lock_guard<std::mutex> lk(mutex_);
        accelerators_ = std::move(found);
    }
    return accelerators_;
}

void Runtime::register_accelerators(std::vector<accel::AcceleratorInfo> accelerators) {
    std::lock_guard<std::mutex> lk(mutex_);
    accelerators_.insert(accelerators_.end(), accelerators.begin(), accelerators.end());
}

const accel::AcceleratorLocality* Runtime::accelerator_locality(AcceleratorId id) const {
    for (const auto& a : accelerators_) {
        if (a.id == id) return &a.locality;
    }
    return nullptr;
}

memory::MemoryRegion Runtime::allocate(memory::MemoryManager::AllocateRequest req) {
    return memory_.allocate(req);
}

void Runtime::release(MemoryRegionId id) { memory_.release(id); }

memory::MemoryRegion Runtime::allocate_on_node(Bytes bytes, NumaNodeId node, AllocationKind kind) {
    memory::MemoryManager::AllocateRequest req;
    req.bytes = bytes;
    req.mode = PlacementMode::PreferredNode;
    req.preferred_node = node;
    req.intended_node = node;
    req.kind = kind;
    req.touch = true;
    req.alignment = Alignment::from(64 * 1024);
    return memory_.allocate(req);
}

std::uint64_t Runtime::allocated_bytes() const { return memory_.total_allocated(); }
std::uint64_t Runtime::node_allocated(NumaNodeId id) const { return memory_.per_node_allocated(id); }

placement::PlacementDecision Runtime::plan(const placement::PlacementRequest& req) const {
    const auto model = build_placement_model();
    return engine_.plan(topo_, req, model);
}

std::string Runtime::execute(const placement::PlacementDecision& decision, const ExecuteOptions& opts) {
    switch (decision.kind) {
        case PlacementDecisionKind::Reject:
            return "REJECT: " + decision.reason;
        case PlacementDecisionKind::Defer:
            return "DEFER: " + decision.reason;
        case PlacementDecisionKind::RevalidationRequired:
            return "REVALIDATION_REQUIRED: " + decision.reason;
        default:
            break;
    }
    std::ostringstream out;
    const auto node = decision.selected_node;
    out << decision.kind << " -> node " << node.value() << " (penalty " << decision.expected_penalty.value() << ")";

    if (opts.allocate_memory) {
        memory::MemoryManager::AllocateRequest areq;
        areq.bytes = opts.memory_footprint;
        areq.mode = PlacementMode::RequiredNode;
        areq.required_node = node;
        areq.preferred_node = node;
        areq.intended_node = node;
        areq.touch = true;
        areq.alignment = Alignment::from(64 * 1024);
        areq.provenance = decision.provenance;
        auto region = memory_.allocate(areq);
        if (opts.verify_memory && region.kind != AllocationKind::Synthetic &&
            region.backend_result.ptr_valid && region.backend_result.ptr) {
            auto* p = static_cast<volatile unsigned char*>(region.backend_result.ptr);
            const auto n = static_cast<std::size_t>(region.granted_bytes.value());
            for (std::size_t i = 0; i < n; ++i) p[i] = static_cast<unsigned char>((i * 131) + 17);
            bool ok = true;
            for (std::size_t i = 0; i < n; ++i) {
                if (p[i] != static_cast<unsigned char>((i * 131) + 17)) { ok = false; break; }
                p[i] = 0;
            }
            out << " region=" << region.id.value();
            out << " granted=" << region.granted_bytes.value();
            out << " verify=" << (ok ? "ok" : "FAILED");
        } else {
            out << " region=" << region.id.value() << " kind=" << region.kind;
        }
    }
    if (opts.bind_worker) out << " (binding delegated to bind_worker)";
    return out.str();
}

reservation::Reservation Runtime::reserve(reservation::ReserveRequest req) { return reservations_.reserve(req); }
void Runtime::release_reservation(ReservationId id) { reservations_.release(id); }
std::uint64_t Runtime::reserved_bytes() const { return reservations_.total_reserved_memory(); }
bool Runtime::reservations_clean() const { return reservations_.accounting_clean(); }
std::uint64_t Runtime::node_reserved(NumaNodeId id) const { return reservations_.node_reserved_memory(id); }

affinity::Worker Runtime::register_worker(WorkerId id, ProcessId pid) { return registry_.register_worker(id, pid); }
affinity::Worker Runtime::restart_worker(WorkerId id, ProcessId pid) { return registry_.restart_worker(id, pid); }
void Runtime::mark_worker_dead(WorkerId id, WorkerBootId boot) { registry_.mark_dead(id, boot); }
const affinity::Worker* Runtime::worker(WorkerId id) const { return registry_.find(id); }
std::vector<affinity::Worker> Runtime::workers() const { return registry_.all(); }

Runtime::BindResult Runtime::bind_worker(WorkerId id, WorkerBootId boot, NumaNodeId node, bool apply_affinity) {
    if (!registry_.validate_boot(id, boot)) throw RuntimeError("stale worker boot for binding " + id.to_string());
    if (!topo_.find_node(node)) throw RuntimeError("cannot bind to unknown node " + node.to_string());

    affinity::AffinitySet set;
    for (const auto& p : topo_.processors_) {
        if (p.node == node) set.add_processor(p.group, p.index_in_group.value());
    }
    bool applied = false;
    if (apply_affinity) applied = backend_->apply_thread_affinity(set);
    auto* w = registry_.find_mut(id);
    BindResult r;
    r.binding_id = BindingId::from(next_placement_);
    r.generation = BindingGeneration::from(w ? w->binding_generation.next().value() : 1);
    r.applied = applied;
    if (w) {
        w->binding_id = r.binding_id;
        w->binding_generation = r.generation;
        w->node = node;
        w->bound = true;
        w->requested_affinity = set;
        w->current_affinity = set;
    }
    return r;
}

void Runtime::observe(WorkerId id, WorkerBootId boot, NumaNodeId node, ProvenanceSource source, bool stale) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto* w = registry_.find_mut(id);
    if (w && w->boot_id == boot) {
        w->last_observation_generation = ObservationGeneration::from(next_observation_++);
    }
    (void)node; (void)source; (void)stale;
}

bool Runtime::migrate(MemoryRegionId region, NumaNodeId target, WorkerId worker,
                      const Auth& auth, std::string reason) {
    if (!registry_.validate_boot(worker, auth.boot)) throw RuntimeError("stale boot for migration");
    migration::MigrateRequest mreq;
    mreq.worker = worker;
    mreq.worker_boot = auth.boot;
    mreq.epoch = epoch_;
    mreq.memory_region = region;
    mreq.target_node = target;
    mreq.source_node = NumaNodeId::invalid();
    mreq.reason = std::move(reason);
    mreq.copy_data = true;
    auto attempt = migrator_.begin(mreq);
    migrator_.execute(attempt);
    const auto* m = migrator_.get(attempt);
    return m && m->committed;
}

void Runtime::advance_epoch() {
    std::lock_guard<std::mutex> lk(mutex_);
    epoch_ = epoch_.next();
}
void Runtime::advance_policy_generation() {
    std::lock_guard<std::mutex> lk(mutex_);
    ++policy_generation_;
}

std::string Runtime::summary_text() const {
    std::ostringstream os;
    os << "NUMA Fabric (version 1.0.0)\n";
    os << "backend: " << backend_->kind_name() << "\n";
    os << "host id: " << topo_.host_id.value() << " generation: " << topo_.generation.value() << "\n";
    os << "NUMA nodes: " << topo_.numa_node_count() << " (single=" << (topo_.is_single_node() ? "yes" : "no") << ")\n";
    os << "processor groups: " << topo_.groups.size() << "\n";
    os << "processors: " << topo_.processor_count() << "\n";
    os << "page size: " << topo_.system_page_size.value() << "\n";
    os << "allocation granularity: " << topo_.allocation_granularity.value() << "\n";
    for (const auto& n : topo_.nodes) {
        os << "  node " << n.id.value() << ": processors=" << n.processor_count()
           << " capacity=" << n.memory_capacity.value();
        if (n.memory_info_available) os << " free=" << n.free_memory.value();
        os << " provenance=" << n.provenance.source << "\n";
    }
    os << "allocated bytes: " << memory_.total_allocated() << "\n";
    os << "reserved bytes: " << reservations_.total_reserved_memory() << "\n";
    os << "workers: " << registry_.live_count() << "\n";
    os << "epoch: " << epoch_.value() << " policy gen: " << policy_generation_ << "\n";
    os << "digest: " << topo_.semantic_digest().hex() << "\n";
    return os.str();
}

std::string Runtime::summary_json() const {
    Json j = Json::object();
    j.object_ref()["backend"] = Json(backend_->kind_name());
    j.object_ref()["host_id"] = Json(topo_.host_id.value());
    j.object_ref()["host_generation"] = Json(topo_.generation.value());
    j.object_ref()["numa_nodes"] = Json(static_cast<std::uint64_t>(topo_.numa_node_count()));
    j.object_ref()["processor_groups"] = Json(static_cast<std::uint64_t>(topo_.groups.size()));
    j.object_ref()["processors"] = Json(static_cast<std::uint64_t>(topo_.processor_count()));
    j.object_ref()["allocated_bytes"] = Json(memory_.total_allocated());
    j.object_ref()["reserved_bytes"] = Json(reservations_.total_reserved_memory());
    j.object_ref()["workers"] = Json(static_cast<std::uint64_t>(registry_.live_count()));
    j.object_ref()["epoch"] = Json(epoch_.value());
    j.object_ref()["policy_generation"] = Json(policy_generation_);
    j.object_ref()["digest"] = Json(topo_.semantic_digest().hex());
    Json nodes = Json::array();
    for (const auto& n : topo_.nodes) {
        Json nj = Json::object();
        nj.object_ref()["id"] = Json(n.id.value());
        nj.object_ref()["processors"] = Json(static_cast<std::uint64_t>(n.processor_count()));
        nj.object_ref()["capacity"] = Json(n.memory_capacity.value());
        nj.object_ref()["provenance"] = Json(std::string(numafabric::to_string(n.provenance.source)));
        nodes.array_ref().push_back(nj);
    }
    j.object_ref()["nodes"] = nodes;
    return Json::parse(j.dump()).dump(true);
}

std::string Runtime::explanation_text(const placement::PlacementDecision& d) const {
    std::ostringstream os;
    os << "Decision: " << d.kind << "\n";
    os << "Selected node: " << d.selected_node.value() << "\n";
    os << "Locality class: " << d.expected_locality_class << "\n";
    os << "Expected penalty: " << d.expected_penalty.value() << "\n";
    os << "Binding constraint: " << d.binding_constraint << "\n";
    os << "Reason: " << d.reason << "\n";
    os << "Would change: " << d.would_change << "\n";
    os << "Provenance: " << d.provenance.source << "\n";
    os << "Eligible alternatives: ";
    for (const auto& n : d.eligible_alternatives) os << n.value() << " ";
    os << "\nRanked candidates:\n";
    for (const auto& c : d.ranked_candidates) {
        os << "  node " << c.node.value() << " total=" << c.total.value() << " [";
        bool first = true;
        for (const auto& [k, v] : c.components) {
            if (!first) os << ", ";
            os << k << "=" << v.value();
            first = false;
        }
        os << "]\n";
    }
    os << "Eliminated:\n";
    for (const auto& e : d.eliminated_candidates) os << "  " << e << "\n";
    return os.str();
}

void Runtime::persist(const std::string& path) {
    persist::PersistedState state;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        state.coordinator_epoch = epoch_.value();
        state.host_generation = topo_.generation.value();
        state.policy_generation = policy_generation_;
        for (const auto& w : registry_.all()) {
            persist::PersistedWorker pw;
            pw.id = w.id; pw.boot = w.boot_id; pw.incarnation = w.incarnation; pw.alive = w.alive;
            pw.node = w.node; pw.placement_generation = w.placement_generation;
            pw.binding_generation = w.binding_generation; pw.policy_generation = w.policy_generation;
            pw.live_generation = w.last_observation_generation.value();
            state.workers.push_back(pw);
        }
        for (const auto& r : reservations_.all()) {
            if (!r.active) continue;
            persist::PersistedReservation pr;
            pr.id = r.id; pr.node = r.node; pr.bytes = r.bytes.value(); pr.cpu_slots = r.cpu_slots;
            pr.accelerator = r.accelerator; pr.accelerator_capacity = r.accelerator_capacity;
            pr.generation = r.generation; pr.active = r.active;
            state.reservations.push_back(pr);
        }
    }
    persist::save(state, path);
}

void Runtime::recover(const std::string& path) {
    auto state = persist::load(path);
    std::lock_guard<std::mutex> lk(mutex_);
    epoch_ = CoordinatorEpoch::from(state.coordinator_epoch);
    policy_generation_ = state.policy_generation;
    for (const auto& pw : state.workers) {
        affinity::Worker w;
        w.id = pw.id;
        w.boot_id = pw.boot;
        w.incarnation = pw.incarnation;
        w.alive = pw.alive;
        w.node = pw.node;
        w.placement_generation = pw.placement_generation;
        w.binding_generation = pw.binding_generation;
        w.policy_generation = pw.policy_generation;
        w.last_observation_generation = ObservationGeneration::from(pw.live_generation);
        registry_.restore(w);
    }
    for (const auto& pr : state.reservations) {
        reservation::Reservation r;
        r.id = pr.id; r.node = pr.node; r.bytes = Bytes::from(pr.bytes); r.cpu_slots = pr.cpu_slots;
        r.accelerator = pr.accelerator; r.accelerator_capacity = pr.accelerator_capacity;
        r.generation = pr.generation; r.active = pr.active;
        reservations_.restore(r);
    }
    memory_.invalidate_locality_stale();
}

bool Runtime::memory_accounting_clean() const { return memory_.accounting_clean(); }
bool Runtime::fully_clean() const { return memory_.accounting_clean() && reservations_.accounting_clean(); }

ScopedAffinityApply::ScopedAffinityApply(backend::Backend& b, const affinity::AffinitySet& set)
    : backend_(b), original_(b.current_thread_affinity()) {
    applied_ = backend_.apply_thread_affinity(set);
}
ScopedAffinityApply::~ScopedAffinityApply() {
    if (applied_) backend_.apply_thread_affinity(original_);
}

} // namespace numafabric
