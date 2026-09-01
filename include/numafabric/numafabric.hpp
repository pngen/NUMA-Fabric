#pragma once
// ============================================================================
// NUMA Fabric - public umbrella header.
//
// Includes the complete public runtime model. Vendor-neutral: the library talks
// to the machine only through the backend::Backend interface.
// ============================================================================

#include "numafabric/core/digest.hpp"
#include "numafabric/core/enums.hpp"
#include "numafabric/core/freshness.hpp"
#include "numafabric/core/ids.hpp"
#include "numafabric/core/json.hpp"
#include "numafabric/core/provenance.hpp"
#include "numafabric/core/quantities.hpp"

#include "numafabric/topology/topology.hpp"
#include "numafabric/accelerator/accelerator.hpp"
#include "numafabric/affinity/affinity_set.hpp"
#include "numafabric/affinity/worker.hpp"
#include "numafabric/backend/backend.hpp"
#include "numafabric/memory/memory_region.hpp"
#include "numafabric/memory/memory_manager.hpp"
#include "numafabric/migration/migration.hpp"
#include "numafabric/placement/placement_engine.hpp"
#include "numafabric/reservation/reservation_store.hpp"
#include "numafabric/runtime/runtime.hpp"

#include "numafabric/protocol/protocol_frame.hpp"
#include "numafabric/protocol/wire.hpp"
#include "numafabric/persistence/persistence_store.hpp"
#include "numafabric/coordinator/coordinator.hpp"
#include "numafabric/worker/worker.hpp"

namespace numafabric {
inline constexpr const char* kVersion = "1.0.0";
}
