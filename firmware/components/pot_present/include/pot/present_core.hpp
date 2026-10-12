// M13: the presenting role's portable half -- what goes on the wire to an MQTT broker, and what comes back.
//
// The cell presents its exposed resources (the package's outside section) to Home Assistant by its MQTT discovery
// (https://www.home-assistant.io/integrations/mqtt/, ledger): one config message per resource, under
// `<prefix>/<component>/potluck_<cell>/<hash>/config`, and the resource's tuple as JSON on its own state topic. This
// file builds those messages and parses what comes back -- no MQTT client, no board -- so the host tests check every
// byte. pot/present.hpp is the board half that sends them.
//
// The rules it follows, from ARCHITECTURE section 13 (M13 and the review of 2026-10-12):
//  * every id carries the cell's identity (its CA fingerprint, 8 hex digits), so two cells on one broker never
//    collide;
//  * nothing is retained; a sensor's state expires (`expire_after`) if it stops coming;
//  * every state payload carries value, unit, age and quality (section 4), and the time it was sampled when the
//    presenter knows the network's time;
//  * availability is one topic per possible presenter, `availability_mode: any` -- a dead presenter's will, which
//    the broker sends 1.5 keep-alives late, never overrides the one that took over. Each presenter's payload lists
//    the boards alive and the resources with no value to show ("down"), and every entity's value_template picks its
//    own board and its own hash out of it: so a board that dies turns its entities unavailable at once, switches
//    included (they have no expire_after), and a `strict` resource that is not GOOD goes unavailable while an
//    `informative` one stays, marked STALE with its age (ADR-002's toggle, kept at the edge);
//  * which Home Assistant component and device class a resource becomes is this integration's mapping (from the
//    value type, the unit and the outside flags), never something an actor's descriptor says (the review's item 15).

#pragma once

#include <cstddef>
#include <cstdint>

#include "pot/namespace.hpp"
#include "pot/value.hpp"

namespace pot::present {

// What the presenter knows about the cell it presents.
struct Cell {
    const char* id = "";       // the cell's identity: 8 lowercase hex digits of its CA's fingerprint
    const char* system = "";   // the cluster's name
    const char* prefix = "homeassistant";
    const uint16_t* presenters = nullptr;  // every node that may hold the role (the package's eligible list)
    size_t n_presenters = 0;
};

// One exposed resource, as the presenter sees it: its namespace entry and the name the package gives it.
struct Entity {
    uint32_t hash = 0;
    uint8_t outside = 0;  // kOutside*
    uint16_t owner = 0;
    ValueType type = ValueType::None;
    Unit unit = Unit::None;
    StalenessPolicy policy = StalenessPolicy::Informative;
    uint32_t bound_ms = 0;
    const char* name = "";
    size_t name_len = 0;
};

Entity entity_of(const NsEntry& e, const char* name, size_t name_len);

enum class Component : uint8_t { Sensor, BinarySensor, Switch, Number, None };
// Writable from outside and a bool: a switch; writable and a number: a number; read-only: a sensor or a binary
// sensor. Bytes and 64-bit integers are not presented (None).
Component component_of(const Entity& e);
const char* component_str(Component c);

// Home Assistant's words for a unit, and its device class (nullptr: none). Units without a device class still
// show their unit.
const char* ha_unit(Unit u);
const char* ha_device_class(Unit u);

// Topics. Each returns the length written (NUL-terminated), 0 if `cap` is short.
size_t config_topic(const Cell& c, const Entity& e, char* out, size_t cap);
size_t state_topic(const Cell& c, uint32_t hash, char* out, size_t cap);
size_t command_topic(const Cell& c, uint32_t hash, char* out, size_t cap);
size_t presenter_topic(const Cell& c, uint16_t node, char* out, size_t cap);
size_t event_topic(const Cell& c, char* out, size_t cap);
// What the presenter subscribes to for commands: potluck/<cell>/+/set.
size_t command_filter(const Cell& c, char* out, size_t cap);

// The discovery config for one entity. `period_ms` is how often the presenter publishes states; a sensor's
// expire_after covers three of them, or twice the resource's staleness bound if that is longer.
size_t config_payload(const Cell& c, const Entity& e, uint32_t period_ms, char* out, size_t cap);

// A resource has a value to show outside: a usable reading, and GOOD if its policy is strict. When false the
// entity goes into the presenter's "down" list.
bool shows_value(const Reading& r, StalenessPolicy p);

// {"value":..,"unit":"..","quality":"GOOD","age_ms":..,"latency_class":..[,"sampled_ms":unix ms]} -- value null
// when it has none to show. `now_unix_ms` 0 when the network's time is not known.
size_t state_payload(const Reading& r, StalenessPolicy p, uint64_t now_unix_ms, char* out, size_t cap);

// The presenter's availability: {"online":1,"node":"7368","nodes":["6300","7368"],"down":["d1db1882"]}. Its will
// is the same with online 0 and empty lists (will_payload()).
size_t presenter_payload(uint16_t self, const uint16_t* live, size_t n_live, const uint32_t* down, size_t n_down,
                         char* out, size_t cap);
size_t will_payload(uint16_t self, char* out, size_t cap);

// An outside write: the hash in potluck/<cell>/<hash>/set, and its payload as a value of the resource's type
// ("ON"/"OFF" for a bool, a decimal number otherwise). False if it is not one.
bool parse_command_topic(const Cell& c, const char* topic, size_t len, uint32_t& hash);
bool parse_command_value(ValueType t, const char* payload, size_t len, Value& out);

// The event announcing an outside write, on event_topic():
// {"what":"outside_write","resource":"<name>","hash":"<hash>","value":"<payload as given>","result":"<result>"}
size_t write_event(const char* name, size_t name_len, uint32_t hash, const char* payload, size_t payload_len,
                   const char* result, char* out, size_t cap);

}  // namespace pot::present
