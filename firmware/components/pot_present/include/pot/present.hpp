// M13: the presenting role -- the board half (an optional module, `firmware.modules: ["present"]`, CONFIG_POT_PRESENT).
//
// A portable actor (ActorType::Present): the reconciler gives it to one board that carries the module and declares
// itself connected (kHelloCapConnected), and moves it when that board dies or loses its network. While it holds the
// role it connects out to the broker the package names (ESP-IDF's managed client, espressif/mqtt), and:
//
//  * publishes one discovery config per exposed resource (pot/present_core.hpp), at connect and whenever Home
//    Assistant announces itself (`<prefix>/status` = online), nothing retained;
//  * every period, publishes its availability (the live boards and the resources with nothing to show) and every
//    exposed resource's state, refreshing a replica from its owner by READ -- polling, since SUBSCRIBE/PUBLISH are
//    not built (the review's item 11);
//  * takes outside writes from potluck/<cell>/<hash>/set, checks them against the package (exposed, writable from
//    outside, not safety-relevant), passes them to the owner as a WRITE marked from outside -- the owning board
//    checks again (pot/namespace.hpp) -- and announces each, refused or sent, on potluck/<cell>/event;
//  * declares its will: its availability topic with online 0, so Home Assistant marks it gone when the broker sees
//    the session die (1.5 keep-alives), and the other presenters' topics keep the entities available.
//
// It writes on behalf of others and owns nothing, so it is portable although it writes: the explicit exemption from
// ADR-006's "actuator-owning actors are never portable" (the review's item 3). It holds no deploy key; a deploy never
// arrives this way.
//
// The MQTT client lives only while the role is held; its task, not the link task, talks to the network. The actor
// (on the link task, under the node's lock) hands it messages with esp_mqtt_client_enqueue(), which does not block on
// the network. Teardown is handed to a short task of its own, so a dead broker never stalls the heartbeat.

#pragma once

#include <cstddef>

#include "pot/actor.hpp"

namespace pot::present {

extern const ActorKind kPresentKind;

// {"t":"present",...}: whether this board holds the role, the broker connection, messages sent and refused, and
// the internal heap's free and largest block (the review's item 4). Nothing when the module is not built.
void print_status();

// Console, for the owner only:
//   POT! present login <user> <password>   the broker's login (saved in NVS; used at the next connection)
//   POT! present forget                    erase it
// Prints the result only, never the arguments. True if the line was one of these.
bool handle_console(const char* line, size_t len);

}  // namespace pot::present
