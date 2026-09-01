#pragma once
// ============================================================================
// NUMA Fabric - provenance for every locality fact.
//
// Every fact that states something about the machine carries a provenance
// source. The distinction is preserved through serialization, persistence,
// replay, aggregation and explanation: a DERIVED accelerator-local relationship
// is never coerced into MEASURED, and SYNTHETIC evidence is never presented as
// physical validation.
// ============================================================================

#include "numafabric/core/ids.hpp"
#include "numafabric/core/enums.hpp"

#include <ostream>
#include <string>
#include <string_view>

namespace numafabric {

struct Provenance {
    ProvenanceSource source = ProvenanceSource::Unknown;
    SourceId source_id = SourceId::invalid();
    SourceGeneration source_generation = SourceGeneration::initial();
    std::string note;

    static Provenance unknown() { return Provenance{}; }
    static Provenance measured(std::string note = {}) {
        Provenance p; p.source = ProvenanceSource::Measured; p.note = std::move(note); return p;
    }
    static Provenance reported(std::string note = {}) {
        Provenance p; p.source = ProvenanceSource::Reported; p.note = std::move(note); return p;
    }
    static Provenance derived(std::string note = {}) {
        Provenance p; p.source = ProvenanceSource::Derived; p.note = std::move(note); return p;
    }
    static Provenance estimated(std::string note = {}) {
        Provenance p; p.source = ProvenanceSource::Estimated; p.note = std::move(note); return p;
    }
    static Provenance synthetic(std::string note = {}) {
        Provenance p; p.source = ProvenanceSource::Synthetic; p.note = std::move(note); return p;
    }

    bool operator==(const Provenance& o) const {
        return source == o.source && source_id == o.source_id &&
               source_generation == o.source_generation && note == o.note;
    }
    bool operator!=(const Provenance& o) const { return !(*this == o); }
};

inline std::ostream& operator<<(std::ostream& os, const Provenance& p) {
    os << numafabric::to_string(p.source);
    if (p.source_id.is_valid()) { os << ":" << p.source_id; }
    if (!p.note.empty()) { os << "(" << p.note << ")"; }
    return os;
}

} // namespace numafabric
