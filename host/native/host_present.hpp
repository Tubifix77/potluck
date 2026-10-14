// M14: the hub presenting in the cell's place -- the presenting role (M13), run by the PC's placement node.
//
// The cell's presenting role is one portable actor (pot/present.hpp). When the cluster file says the hub presents
// (`present.hub`), `potluck apply` lists the PC's node among its eligible nodes with the highest gravity, so the
// reconciler gives the role to the PC while it is in the cell and hands it back to a connected board when it goes --
// M10's handover, never two holders. This is that actor on the PC: the board's PresentActor (pot_present/present.cpp)
// line for line where it builds what is published -- the same present_core, the same namespace, the same reads through
// the cell -- with the transport handed to potluck.hostnode, which owns the MQTT connection:
//
//   out (stderr, one JSON object a line): {"t":"mqtt","k":"S", ...start...}  {"t":"mqtt","k":"P","topic":..,"b64":..}
//                                         {"t":"mqtt","k":"X"}
//   in  (stdin, a control record: length | 0x8000, then text): "connected 1|0", "discovery", "cmd <hash> <b64>",
//                                         "net 1|0" (the PC can reach the broker: HELLO's connected bit)
#pragma once

#include <cstddef>
#include <cstdint>

#include "pot/actor.hpp"
#include "pot/deploy.hpp"

namespace hostpresent {

struct Services {
    const pot::DeployImage* img = nullptr;
    const char* cell_id = "00000000";  // 8 hex digits of the cluster CA's fingerprint, as every board computes it
    const char* system = "lab";
    const char* prefix = "homeassistant";
    void (*emit)(const char* json_line) = nullptr;  // one line to stderr, flushed
};
void set_services(const Services& s);

// From potluck.hostnode, on the main thread.
void on_connected(bool c);
void on_discovery();
void on_command(uint32_t hash, const char* payload, size_t len);

extern const pot::ActorKind kHostPresentKind;

}  // namespace hostpresent
