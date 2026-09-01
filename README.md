# NUMA Fabric

NUMA Fabric is a C++20 runtime boundary for **explicit host-locality governance**
in accelerator infrastructure. It is not a topology viewer, a generic benchmark,
a thread-affinity helper, or a thin wrapper around the Windows / Linux NUMA APIs.

## The systems question

NUMA Fabric answers one systems question:

> Where should CPU execution and host memory live relative to accelerators, NICs,
> storage, and other memory domains so that locality is deliberate rather than
> accidental?

Topology APIs answer *what hardware topology exists and what path costs it exposes*.
NUMA Fabric consumes (or independently derives) the relevant host-locality facts and
turns NUMA placement into **governed runtime state**: which NUMA nodes exist, which
processors belong to them, which memory allocation belongs to which locality domain,
where worker threads are allowed or preferred to execute, which accelerators and I/O
devices are locally associated with which host domains, what placement was requested,
what placement was actually achieved, whether locality evidence is
measured/derived/synthetic/stale/unknown, whether a placement remains valid after
topology/generation/policy/process/worker changes, when migration/rebinding/
reallocation should occur, what locality penalty is expected when preferred placement
cannot be honored, and who has authority to change placement.

## Why NUMA locality matters for accelerator infrastructure

For accelerator workloads the host-side placement of CPU execution and host memory is
not incidental. A kernel that launches on an accelerator attached to one host domain,
while the host buffers and the control thread live on a remote domain, pays a real
cross-domain cost on every transfer and every kernel launch. Rendering buffers,
page-locked DMA regions, and the threads that drive the accelerator should be
co-located with the device. NUMA Fabric makes that co-location explicit, typed,
explainable, and verifiable rather than a hint the OS may or may not honor.

## Architecture

NUMA Fabric is vendor-neutral at its architectural boundary. The runtime model never
depends on a specific operating system; it talks to the machine only through the
backend::Backend interface. The public concerns are separated into focused components:

- topology - canonical NUMA inventory, processor groups, immutable snapshots, stable semantic digests
- memory - governed host-memory placement, lifecycle state machines, per-node accounting
- affinity - group-aware affinity sets, worker identities and scoped binding
- placement - a deterministic placement engine with inspectable explanations
- reservation - atomic NUMA-local reservations and accounting
- migration - explicit migration/rebinding state machine
- accelerator - accelerator-to-host locality relationships
- persistence - versioned, CRC-32 protected binary persistence
- protocol - a versioned framed-TCP binary protocol
- coordinator / worker - distributed authority and the framed-TCP service
- cuda - an optional CUDA integration / proof backend
- runtime - the composition root (one backend plus the managers above)

A Runtime is the composition root: it owns one backend and wires the concerns together.
The CLI, examples, tests, coordinator and workers all drive this same public API;
nothing duplicates model logic elsewhere.

## Discovery model

Discovery builds a canonical inventory from a Backend. On Windows it uses supported OS
APIs (GetActiveProcessorGroupCount, GetActiveProcessorCount, GetLogicalProcessor-
InformationEx with RelationNumaNode, GetNumaAvailableMemoryNode, GlobalMemoryStatusEx).
Processor-group correctness is explicit: membership is derived from per-node, per-group
masks, and a single 64-bit affinity mask is **never** assumed to represent the whole
machine. Unavailable facts are represented as UNKNOWN rather than invented.

Immutable TopologySnapshot objects are produced for comparison, persistence, replay,
deterministic ranking, diff generation and explanation. Stable semantic digests never
depend on process addresses or unstable enumeration ordering (node/group/processor
identities are canonicalized before hashing).

### Physical validation host

The validation host reports exactly one NUMA node and one processor group:

- **NUMA nodes:** 1 (node 0)
- **Processor groups:** 1 (group 0) with **16 active logical processors**
- **CPU:** AMD Ryzen 7 9800X3D (8 cores, 16 threads, single socket AM5)
- **Physical memory:** 63,124 MB

This is reported exactly as it is. There is **one** physical NUMA node; the runtime never
fabricates a second physical node.

## Host-memory placement

NUMA Fabric implements governed host-memory allocation with explicit NUMA intent.
Supported placement modes include ANY, LOCAL, PREFERRED_NODE, REQUIRED_NODE,
INTERLEAVED (where meaningfully implementable), and SYNTHETIC (controlled validation
only). Every governed region records its requested bytes, granted bytes, alignment,
placement policy, preferred/required node, actual observed or derived locality, owning
process, allocation generation, provenance, lifecycle state, touched/committed status,
locality verification and freshness, and any associated workload/device.

**NUMA Fabric never claims placement merely because an API accepted a preferred-node
hint.** On Windows the OS does not expose a portable per-page NUMA index for arbitrary
virtual allocations, so governed Windows allocations are represented honestly as
*requested/derived* locality (locality_known == false) with the node the allocation was
requested for recorded separately from any observed locality. The physical proof
exercises the governed allocation path against real OS/VirtualAllocExNuma memory and real
thread binding.

The allocation lifecycle is explicit and guarded: CREATED -> RESERVED -> ALLOCATED ->
ACTIVE -> MIGRATION_PENDING -> MIGRATING -> REBOUND -> RELEASE_PENDING -> RELEASED,
plus FAILED / INVALIDATED. Disallowed transitions throw; double free, stale release,
stale migration and generation rollback are rejected by construction. Per-node
accounting closes exactly to zero after teardown.

## Worker / thread affinity

NUMA Fabric implements governed CPU execution affinity with preferred/required NUMA
nodes, processor-group-aware affinity, explicit allowed processor sets, policy-driven
worker placement, temporary scoped binding, restoration of prior affinity, rebinding
after worker restart, and deterministic selection among eligible processors. Requested
affinity is modeled separately from actual affinity. A scoped binding
(ScopedThreadBinding) restores the previous affinity on scope exit; the runtime never
leaves process/thread affinity silently altered unless the API contract explicitly
requests persistent binding.

## Accelerator locality

NUMA Fabric represents the accelerator-to-host locality *relationship* (SAME_NUMA_NODE,
SAME_SOCKET, SAME_HOST_REMOTE_NUMA, UNKNOWN, SYNTHETIC) using real CUDA / PCI / OS device
evidence where available, and distinguishes DERIVED from MEASURED. It does **not** assert
that a GPU belongs to NUMA node 0 merely because the machine has one visible node without
recording how that conclusion was derived.

On the validation host the OS does not expose a conclusive GPU-to-NUMA-node mapping, so
the RTX 5090's locality is reported as UNKNOWN (provenance: DERIVED, "no OS GPU->NUMA
mapping exposed") with its real PCI identity (0000:01:00.0) and compute capability
(12.0, sm_120). The CUDA proof still exercises the governed host-allocation path against
real CPU/host/CUDA execution.

## Placement decisions

The deterministic placement engine turns a request into a typed decision: PLACE,
PLACE_WITH_PENALTY, REBIND, MIGRATE, DEFER, REJECT, or REVALIDATION_REQUIRED. Candidate
ranking is **not** an opaque master score: every named cost component (memory locality,
accelerator locality, preference, distance/cost, capacity headroom, migration/rebind
cost, I/O locality, CPU load where genuinely measurable) is inspectable. Each decision
reports the selected NUMA node, eligible alternatives, eliminated candidates (with
reasons), the binding constraint, the expected locality class, the expected penalty, the
provenance of the locality facts, the policy generation, and what would change the
decision. The engine never emits "no binding constraint" when a hard constraint exists.

## Reservations and accounting

NUMA-local reservations atomically claim memory capacity, worker/execution slots, and
optional accelerator-associated capacity across one or more nodes. If any dimension
fails, the whole reservation is refused (no partial claim is left behind). 
Oversubscription, double reservation, duplicate release, stale release, generation
rollback and cross-node accounting leakage are rejected. Concurrent reserve/release
stress closes exactly to zero.

## Migration / rebinding semantics

Migration is modeled as an explicit state machine with a source placement, target
placement, attempt identity, generation, authority, reason, expected benefit and cost,
lifecycle and completion state. Real physical page migration is **not** claimed: the
platform does not expose it portably, so migration is implemented as a governed
**reallocate + copy + rebind**, stated explicitly. A failed migration never destroys the
last valid authoritative copy; if the new allocation succeeds but the copy fails, the
original authority is retained and provisional state is cleaned up. On commit the old
placement is fenced from becoming current again.

## Provenance

Every locality fact carries a provenance source: MEASURED, REPORTED, DERIVED, ESTIMATED,
SYNTHETIC, or UNKNOWN. OS processor/node membership is measured or reported according to
the exact source semantics; a derived accelerator locality relationship is DERIVED;
synthetic dual-socket validation is SYNTHETIC. These distinctions are preserved through
serialization, persistence, replay, aggregation and explanation.

## Freshness & revalidation

Freshness is explicit. Topology/locality evidence becomes stale after worker restart,
coordinator epoch change, device generation change, topology refresh, processor-group
change, policy change, process restart, reservation change, or recovery from persistence.
A recovered observation is never treated as CURRENT merely because its timestamp is
recent: the runtime demands an explicit REVALIDATION_REQUIRED state and does not infer
freshness from wall-clock age.

## Distributed authority

All mutable distributed operations are fenced against CoordinatorEpoch, WorkerBootId,
worker generation, topology/host generation, PlacementGeneration, BindingGeneration,
MemoryGeneration, PolicyGeneration, AttemptId, and reservation generation, with a
deterministic validation order. Old authority never mutates fresh state. A restarted
logical worker carries a fresh WorkerBootId and does not inherit authority merely because
WorkerId is unchanged; worker-specific generation gates are scoped to the worker
incarnation so a freshly restarted worker is not fenced by an older incarnation's higher
generation.

The distributed proof runs a real framed-TCP coordinator with two real OS worker
processes. It registers both, does real work, kills one worker as a real OS process,
observes its loss, advances the coordinator epoch, rejects stale epoch/boot/placement/
binding/memory mutations over the real protocol, restarts the same logical worker with a
fresh boot, proves the other worker is unaffected, performs fresh post-restart work,
persists authoritative state, stops the coordinator, starts a fresh coordinator *process*
that recovers persisted logical state, and requires physical evidence revalidation before
it becomes current.

## Persistence & recovery

Persistence is versioned and binary. The container is magic-prefixed and versioned, has
an explicit payload length, and is CRC-32 protected; encoding is deterministic; writes
are atomic (temp -> flush -> close -> rename). Decoding is bounded and strict:
truncation, duplicate IDs, impossible counts, malformed enums, invalid generations and
trailing garbage are rejected. Logical state (identities, generations, policy,
reservations/placement intent, provenance, historical observations, authority metadata)
is persisted separately from physical freshness. Recovery never falsely claims the
current worker boot, current process/thread affinity, current physical page locality,
current device health, or current topology freshness unless revalidated.

## Windows backend

The Windows backend uses supported operating-system APIs for group-aware NUMA discovery,
governed allocation (VirtualAllocExNuma, a preferred-node hint that is never claimed to be
measured), thread affinity (GetThreadGroupAffinity / SetThreadGroupAffinity), and
available per-node memory. Processor-group correctness is explicit. It does not
fabricate pinned page-locked allocations; that path is CUDA-only on this platform.

## Synthetic backend

Because the physical validation host exposes one NUMA node, a deterministic synthetic
backend models multi-socket / multi-NUMA / multi-accelerator systems (with configurable
node count, processor-per-node, group boundaries, per-node capacities, distance matrices
and accelerator localities). It runs the **same** production decision paths as the
hardware backends; it is not a toy implementation. Cross-node placement, migration,
distance, policy, capacity exhaustion, accelerator locality, processor-group boundaries
and sparse membership proofs all use the synthetic backend. Synthetic evidence is always
marked SYNTHETIC and is **never** presented as physical multi-socket validation.

## CUDA proof (RTX 5090, sm_120)

When built with -DNUMAFABRIC_ENABLE_CUDA=ON, the CUDA backend selects the installed CUDA
13.1 toolkit, targets sm_120, discovers real CUDA devices, and runs a bounded real proof:
governed host allocation through NUMA Fabric -> deterministic host init -> cudaMalloc ->
H2D -> real kernel launch -> sync -> D2H -> CPU-reference verification -> cudaFree -> host
release -> CUDA error-state verification -> accounting closure. This proof passed against
the validation host's NVIDIA GeForce RTX 5090 (0000:01:00.0, 12.0). Reported transfer and
execution times are measurements on one host, not universal claims.

## Build, install, use

From a Visual Studio developer prompt (or after loading the MSVC environment):

    cmake -G Ninja -S . -B build -DNUMAFABRIC_ENABLE_CUDA=ON
    cmake --build build
    ctest --test-dir build --output-on-failure

The core library requires only a C++20 compiler; CUDA is optional at configure time
(-DNUMAFABRIC_ENABLE_CUDA=OFF builds without it). Project-controlled C++ source compiles
cleanly under /W4 /WX (MSVC); CUDA source is also warning-clean for project-controlled
code.

To install and use from downstream:

    cmake --install build --prefix <prefix>

Then a consumer can do:

    find_package(NUMAFabric CONFIG REQUIRED)
    target_link_libraries(my_app PRIVATE NUMAFabric::numafabric)

## CLI

The nf executable exercises the real library API (never duplicates model logic). Commands
include discover, nodes, processors, devices, summary, allocations, workers, placements,
reservations, snapshot, diff, save, load, validate, bench, and mask. --json emits JSON;
--backend synthetic --nodes N --procs M selects the deterministic multi-node model.

Examples:

    nf discover
    nf --backend synthetic --nodes 2 --procs 8 placements
    nf --json summary

## Examples

The examples/ directory contains runnable programs that demonstrate the real APIs,
including discovery, node/processor inspection, preferred-node allocation, worker
binding, placement evaluation and explanation, reservations, synthetic dual-NUMA
placement, fallback with penalty, topology generation rollover, stale-authority
rejection, persistence/recovery, and concurrent placement.

## Benchmarks

benchmarks/ reports completed work (no dead code or unconsumed results) in explicit
units, separated by concern: in-memory decision throughput, OS allocation/binding cost,
persistence cost, and (when linked) CUDA transfer cost. Different operations are never
presented as equivalent.

## Limitations (stated precisely)

- The validation host has **one** physical NUMA node. Cross-node placement, migration,
  distance and policy behavior is validated against the **synthetic** multi-node model,
  not against physical multi-socket hardware.
- On Windows, governed host allocation uses a **preferred-node hint**
  (VirtualAllocExNuma). Physical page locality is **not** measured and is represented as
  requested/derived, never as measured.
- Migration is a governed **reallocate + copy + rebind**. It is not physical OS page
  migration.
- On this host the GPU-to-NUMA-node mapping is not exposed by the OS, so the accelerator
  locality is UNKNOWN / DERIVED; it is not asserted as measured.
- Pinned page-locked host allocation is CUDA-only on this platform.
- Portability beyond the implemented Windows and synthetic backends (e.g., Linux NUMA) is
  not claimed; the backend interface is designed to support it without redesigning the
  public runtime model.
- CUDA timing is reported as a measurement on a single host, not a universal performance
  claim.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
