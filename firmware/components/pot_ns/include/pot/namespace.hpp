// The namespace — ARCHITECTURE.md §7.2, §4, §6.
//
// §6 budgets "Namespace table, 128 entries × 64 B = 8.0 KB | path hash, type, unit, class,
// staleness bound, cached value+ts". Sixty-four bytes does not hold a path string, and that is the
// design rather than a limitation: §14's mitigation for namespace lookup cost is to "perfect-hash
// the paths at build time from the manifest", so a node stores a hash and the *toolchain* keeps the
// strings. A node never parses `potluck://lab/node-07/adc/1`; it looks up a 32-bit number.
//
// The consequence to be honest about: a node cannot enumerate its own namespace as text, and
// `LIST` (§5.2, M1+) returns hashes that the host resolves against the manifest. That is a real
// cost, paid deliberately, and it is why the hash function is specified here rather than left to
// whatever each side happens to implement.

#pragma once

#include <cstddef>
#include <cstdint>

#include "pot/value.hpp"

namespace pot {

// §6: 128 entries.
constexpr size_t kNamespaceEntries = 128;

// ---------------------------------------------------------------------------------------------
// Path hashing. FNV-1a, 32-bit.
//
// Specified here, byte for byte, because the host computes it from a string and the node only ever
// sees the result — the two must agree exactly or every read misses. Chosen over anything cleverer
// because it is four lines, has no tables, and can be reimplemented correctly in Python in five
// minutes (host/potluck does exactly that, and a test checks the two agree).
//
// Collisions: 128 entries in a 2^32 space is a birthday probability around 1.9e-6. Not zero, so
// `insert()` rejects a colliding hash rather than silently aliasing two resources — at build time
// that is a manifest error the toolchain reports, which is the right place to find it.
// ---------------------------------------------------------------------------------------------

constexpr uint32_t kFnv1a32Offset = 2166136261u;
constexpr uint32_t kFnv1a32Prime = 16777619u;

constexpr uint32_t path_hash(const char* s) {
    uint32_t h = kFnv1a32Offset;
    while (*s != '\0') {
        h ^= static_cast<uint8_t>(*s++);
        h *= kFnv1a32Prime;
    }
    return h;
}

uint32_t path_hash_n(const char* s, size_t n);

// ---------------------------------------------------------------------------------------------
// One namespace entry. §7.2: "Every entry carries kind, type, unit, access, latency_class,
// staleness_bound_ms, owner_node, plus the cached value and timestamp."
// ---------------------------------------------------------------------------------------------

struct NsEntry {
    uint32_t path_hash;          // 0 = free slot
    uint32_t staleness_bound_ms; // §4 rule 2; 0 means "never stale" (a constant, a configuration)
    uint32_t updated_ms;         // when the cached value was last written, on the owner's clock
    uint32_t update_count;       // total writes; distinguishes "never written" from "written zero"

    Value value;                 // 10 B
    Unit unit;                   // 2 B

    uint16_t owner_node;         // §5.1 node id; kNodeBroadcast is not valid here
    uint8_t kind;                // ResourceKind
    uint8_t access;              // Access
    uint8_t latency_class;       // §4 L0..L4
    uint8_t staleness_policy;    // StalenessPolicy
    uint8_t flags;               // bit0 = faulty, as reported by the owner
    uint8_t reserved0[5];

    bool free() const { return path_hash == 0; }
    bool has_data() const { return update_count > 0; }
    bool is_local(uint16_t self) const { return owner_node == self; }
};

static_assert(sizeof(NsEntry) == 40, "one namespace entry");
static_assert(sizeof(NsEntry) * kNamespaceEntries <= 8192,
              "§6: namespace table is 128 entries in 8.0 KB");

constexpr uint8_t kNsFlagFaulty = 1u << 0;

// What a resource declares when it is registered. A separate struct from NsEntry so that
// registration reads as a declaration rather than as poking fields.
struct NsDecl {
    uint32_t path_hash = 0;
    uint16_t owner_node = 0;
    ValueType type = ValueType::None;
    Unit unit = Unit::None;
    ResourceKind kind = ResourceKind::Sampled;
    Access access = Access::Read;
    uint8_t latency_class = kClassL4;
    uint32_t staleness_bound_ms = 0;
    StalenessPolicy staleness_policy = StalenessPolicy::Informative;
};

enum class NsError : uint8_t {
    Ok = 0,
    NotFound,
    Full,
    HashCollision,   // a different path already occupies this hash — a manifest error
    TypeMismatch,    // a write whose type is not the declared one
    NotWritable,
    NotReadable,
    WrongOwner,      // a write aimed at a resource this node does not own
    EventNotCached,  // §7.2: event entries have queue semantics; READ is not how you get them
};

const char* ns_error_str(NsError e);

// ---------------------------------------------------------------------------------------------
// The table.
//
// Open addressing with linear probing over a power-of-two array, so a lookup is a mask and a
// compare — no division, no allocation, and a bounded probe. §14 lists namespace lookup cost as a
// risk that "reduces node count"; this is the mitigation, and `worst_probe()` reports the actual
// figure so the risk can be checked rather than assumed.
// ---------------------------------------------------------------------------------------------

class Namespace {
  public:
    Namespace();

    void reset();

    // Declare a resource. Returns HashCollision if a *different* path already hashes here, and Ok
    // if the same path is re-declared (idempotent, so a manifest reload is not a special case).
    NsError declare(const NsDecl& d);

    NsEntry* find(uint32_t hash);
    const NsEntry* find(uint32_t hash) const;

    // Publish a value into a resource this node owns. This is the *owner's* path — a sensor driver
    // reporting its own reading — and it does not consult `access`.
    //
    // That distinction is the whole reason there are two functions. `access` is the policy for
    // WRITEs arriving from the wire; it is not a lock the owning driver has to pick. Almost every
    // useful resource is read-only to the cluster and written constantly by the node that owns it —
    // a temperature, a heap figure, an encoder count — and a single access-checked write would make
    // those impossible to populate without declaring them remotely writable, which is exactly
    // backwards.
    //
    // `now_ms` is the owner's clock and becomes the sample timestamp, not the time the call was
    // processed: age is measured from it, and an age that quietly excludes queueing is the kind of
    // inaccuracy §4 rule 2 exists to prevent.
    NsError publish(uint32_t hash, const Value& v, uint32_t now_ms);

    // Apply a WRITE that arrived from the wire. Same as publish() but honours `access`, so a
    // read-only resource refuses. Used by the WRITE handler and by nothing else.
    NsError write_local(uint32_t hash, const Value& v, uint32_t now_ms);

    // Fold in a value that arrived from the owning node. `sampled_ms` is the *owner's* timestamp
    // from the wire; `local_now_ms` is ours. See the note in read() about which clock ages use.
    NsError apply_remote(uint32_t hash, const Value& v, uint32_t sampled_ms, uint32_t local_now_ms,
                         bool faulty);

    // Read, per §4 rule 2. `owner_alive` comes from membership (§8.2) — the namespace does not
    // track liveness itself, and a read of a resource on a dead node must say Unavailable however
    // fresh the cache looks.
    //
    // On clocks: age is computed against the clock that produced the timestamp. For a local
    // resource that is our own clock and the age is exact. For a remote one it is the *arrival*
    // time we recorded locally, not the owner's timestamp, because the two clocks are unsynced
    // (the same reason M0's RTT machinery reports durations rather than instants). So a remote age
    // is "how long since this reached us", which is the conservative reading and never younger
    // than the truth.
    NsError read(uint32_t hash, uint32_t now_ms, bool owner_alive, Reading& out) const;

    size_t count() const;
    static constexpr size_t capacity() { return kNamespaceEntries; }

    NsEntry& slot(size_t i) { return entries_[i]; }
    const NsEntry& slot(size_t i) const { return entries_[i]; }

    // Longest probe distance currently in the table — §14's lookup-cost risk, measured.
    size_t worst_probe() const;

  private:
    size_t index_for(uint32_t hash) const;

    NsEntry entries_[kNamespaceEntries];
    // Arrival time on *our* clock, parallel to entries_. Kept outside NsEntry because it is a
    // property of our copy rather than of the resource, and putting it in the entry would make the
    // 64 B budget line mean something different on the owning node than on a caching one.
    uint32_t arrived_ms_[kNamespaceEntries];
};

static_assert(kNamespaceEntries > 0 && (kNamespaceEntries & (kNamespaceEntries - 1)) == 0,
              "power of two, so index_for() is a mask rather than a modulo");

}  // namespace pot
