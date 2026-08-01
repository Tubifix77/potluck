// The namespace — see namespace.hpp and ARCHITECTURE.md §7.2, §4.

#include "pot/namespace.hpp"

#include <cstring>

namespace pot {

const char* ns_error_str(NsError e) {
    switch (e) {
        case NsError::Ok: return "ok";
        case NsError::NotFound: return "not_found";
        case NsError::Full: return "full";
        case NsError::HashCollision: return "hash_collision";
        case NsError::TypeMismatch: return "type_mismatch";
        case NsError::NotWritable: return "not_writable";
        case NsError::NotReadable: return "not_readable";
        case NsError::WrongOwner: return "wrong_owner";
        case NsError::EventNotCached: return "event_not_cached";
    }
    return "unknown";
}

uint32_t path_hash_n(const char* s, size_t n) {
    uint32_t h = kFnv1a32Offset;
    for (size_t i = 0; i < n; ++i) {
        h ^= static_cast<uint8_t>(s[i]);
        h *= kFnv1a32Prime;
    }
    return h;
}

Namespace::Namespace() { reset(); }

void Namespace::reset() {
    // Value-initialise rather than memset: NsEntry contains a Value, which has default member
    // initialisers and so is not trivially default-constructible. GCC's -Wclass-memaccess is right
    // to object — memset would happen to produce the same bytes today and would silently stop doing
    // so the moment anyone gave a member a non-zero default.
    for (size_t i = 0; i < kNamespaceEntries; ++i) {
        entries_[i] = NsEntry{};
        arrived_ms_[i] = 0;
    }
}

size_t Namespace::index_for(uint32_t hash) const {
    return static_cast<size_t>(hash) & (kNamespaceEntries - 1);
}

NsError Namespace::declare(const NsDecl& d) {
    if (d.path_hash == 0) {
        // 0 marks a free slot, so it cannot also be a valid path. The odds of FNV-1a producing it
        // are 1 in 4 billion; rejecting it costs nothing and removes the case entirely.
        return NsError::HashCollision;
    }

    const size_t start = index_for(d.path_hash);
    for (size_t probe = 0; probe < kNamespaceEntries; ++probe) {
        const size_t i = (start + probe) & (kNamespaceEntries - 1);
        NsEntry& e = entries_[i];

        if (e.free()) {
            e.path_hash = d.path_hash;
            e.staleness_bound_ms = d.staleness_bound_ms;
            e.updated_ms = 0;
            e.update_count = 0;
            e.value = Value{};
            e.value.type = d.type;
            e.unit = d.unit;
            e.owner_node = d.owner_node;
            e.kind = static_cast<uint8_t>(d.kind);
            e.access = static_cast<uint8_t>(d.access);
            e.latency_class = d.latency_class;
            e.staleness_policy = static_cast<uint8_t>(d.staleness_policy);
            e.flags = 0;
            arrived_ms_[i] = 0;
            return NsError::Ok;
        }

        if (e.path_hash == d.path_hash) {
            // Re-declaring the same path is idempotent, so reloading a manifest is not a special
            // case. The declaration is updated; the cached value is not disturbed.
            e.staleness_bound_ms = d.staleness_bound_ms;
            e.unit = d.unit;
            e.owner_node = d.owner_node;
            e.kind = static_cast<uint8_t>(d.kind);
            e.access = static_cast<uint8_t>(d.access);
            e.latency_class = d.latency_class;
            e.staleness_policy = static_cast<uint8_t>(d.staleness_policy);
            if (e.value.type != d.type) {
                // A type change is not a re-declaration, it is a different resource wearing the
                // same name. Refusing is the only safe answer: the cached bytes mean something else.
                return NsError::TypeMismatch;
            }
            return NsError::Ok;
        }
    }
    return NsError::Full;
}

NsEntry* Namespace::find(uint32_t hash) {
    return const_cast<NsEntry*>(static_cast<const Namespace*>(this)->find(hash));
}

const NsEntry* Namespace::find(uint32_t hash) const {
    if (hash == 0) {
        return nullptr;
    }
    const size_t start = index_for(hash);
    for (size_t probe = 0; probe < kNamespaceEntries; ++probe) {
        const size_t i = (start + probe) & (kNamespaceEntries - 1);
        const NsEntry& e = entries_[i];
        if (e.free()) {
            // Linear probing never leaves a hole before an entry that hashed here, so a free slot
            // ends the search. Nothing is ever removed from this table, which is what keeps that
            // true — a namespace shrinks by rebuilding, not by deleting.
            return nullptr;
        }
        if (e.path_hash == hash) {
            return &e;
        }
    }
    return nullptr;
}

NsError Namespace::write_local(uint32_t hash, const Value& v, uint32_t now_ms) {
    const NsEntry* e = find(hash);
    if (e == nullptr) {
        return NsError::NotFound;
    }
    if ((e->access & static_cast<uint8_t>(Access::Write)) == 0) {
        // A WRITE from the wire against a read-only resource. The owner's own publish() path does
        // not come through here — see the header.
        return NsError::NotWritable;
    }
    return publish(hash, v, now_ms);
}

NsError Namespace::publish(uint32_t hash, const Value& v, uint32_t now_ms) {
    NsEntry* e = find(hash);
    if (e == nullptr) {
        return NsError::NotFound;
    }
    if (e->value.type != ValueType::None && v.type != e->value.type) {
        // The declared type is part of the resource's identity (§4: "the class is part of the
        // type"). A write of the wrong type is a bug in the writer, not a conversion to perform.
        return NsError::TypeMismatch;
    }

    e->value = v;
    // The sample timestamp is when the value was produced, not when the write was processed. Age is
    // computed from it, and an age that silently excludes queueing delay is exactly the kind of
    // quiet inaccuracy §4 rule 2 exists to prevent.
    e->updated_ms = now_ms;
    ++e->update_count;
    e->flags &= static_cast<uint8_t>(~kNsFlagFaulty);

    const size_t i = static_cast<size_t>(e - entries_);
    arrived_ms_[i] = now_ms;
    return NsError::Ok;
}

NsError Namespace::apply_remote(uint32_t hash, const Value& v, uint32_t sampled_ms,
                                uint32_t local_now_ms, bool faulty) {
    NsEntry* e = find(hash);
    if (e == nullptr) {
        return NsError::NotFound;
    }
    if (e->value.type != ValueType::None && v.type != e->value.type) {
        return NsError::TypeMismatch;
    }

    e->value = v;
    e->updated_ms = sampled_ms;  // the owner's clock, carried for reference and for the host
    ++e->update_count;
    if (faulty) {
        e->flags |= kNsFlagFaulty;
    } else {
        e->flags &= static_cast<uint8_t>(~kNsFlagFaulty);
    }

    // Age will be measured from arrival on *our* clock. The owner's timestamp cannot be subtracted
    // from our clock — they are unsynchronised, and the difference would be an offset rather than
    // an age. Arrival is conservative: never younger than the truth.
    const size_t i = static_cast<size_t>(e - entries_);
    arrived_ms_[i] = local_now_ms;
    return NsError::Ok;
}

NsError Namespace::read(uint32_t hash, uint32_t now_ms, bool owner_alive, Reading& out) const {
    const NsEntry* e = find(hash);
    if (e == nullptr) {
        return NsError::NotFound;
    }
    if ((e->access & static_cast<uint8_t>(Access::Read)) == 0) {
        return NsError::NotReadable;
    }
    if (static_cast<ResourceKind>(e->kind) == ResourceKind::Event) {
        // §7.2: event entries have queue semantics — "every publication is delivered to subscribers
        // in order, staleness does not apply". A cached last-value read of an event stream is
        // precisely the click-eaten-by-a-cache failure the kind field exists to prevent, so READ is
        // refused rather than answered with the most recent event.
        return NsError::EventNotCached;
    }

    const size_t i = static_cast<size_t>(e - entries_);
    const uint32_t age = e->has_data() ? (now_ms - arrived_ms_[i]) : 0;

    out.unit = e->unit;
    out.timestamp_ms = e->updated_ms;
    out.age_ms = age;
    out.latency_class = e->latency_class;
    out.quality = classify(age, e->staleness_bound_ms,
                           static_cast<StalenessPolicy>(e->staleness_policy), e->has_data(),
                           owner_alive);
    if ((e->flags & kNsFlagFaulty) != 0 && quality_has_value(out.quality)) {
        out.quality = Quality::Faulty;
    }

    // The value travels only when the quality says it may. Under `strict` past the bound, and for
    // an unavailable or faulty resource, the caller gets no number at all — which is the difference
    // between "old data, marked" and "no data", and §4 rule 2 makes it the resource's choice.
    out.value = quality_has_value(out.quality) ? e->value : Value{};
    return NsError::Ok;
}

size_t Namespace::count() const {
    size_t n = 0;
    for (size_t i = 0; i < kNamespaceEntries; ++i) {
        if (!entries_[i].free()) {
            ++n;
        }
    }
    return n;
}

size_t Namespace::worst_probe() const {
    size_t worst = 0;
    for (size_t i = 0; i < kNamespaceEntries; ++i) {
        if (entries_[i].free()) {
            continue;
        }
        const size_t home = index_for(entries_[i].path_hash);
        const size_t d = (i >= home) ? (i - home) : (i + kNamespaceEntries - home);
        if (d > worst) {
            worst = d;
        }
    }
    return worst;
}

}  // namespace pot
