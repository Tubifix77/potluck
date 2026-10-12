// M13: the board's own network connection. See uplink.hpp.

#include "uplink.hpp"

#include "extender.hpp"
#include "pot/wifi.hpp"
#include "sdkconfig.h"

namespace pot::uplink {

namespace {
Kind g_kind = Kind::None;
}

bool start(uint8_t cell_channel) {
    if (extender::start(cell_channel)) {
        g_kind = Kind::Extender;
    } else if (wifi::start(cell_channel)) {
        g_kind = Kind::Wifi;
    }
    return g_kind != Kind::None;
}

Kind kind() { return g_kind; }

const char* name() {
    switch (g_kind) {
        case Kind::Extender: return "extender";
        case Kind::Wifi: return "wifi";
        case Kind::None: break;
    }
    return "none";
}

bool active() { return g_kind != Kind::None; }

bool associated() {
    switch (g_kind) {
        case Kind::Extender: return extender::associated();
        case Kind::Wifi: return wifi::associated();
        case Kind::None: break;
    }
    return false;
}

bool has_ip() {
    switch (g_kind) {
        case Kind::Extender: return extender::has_ip();
        case Kind::Wifi: return wifi::has_ip();
        case Kind::None: break;
    }
    return false;
}

uint8_t take_channel() {
    switch (g_kind) {
        case Kind::Extender: return extender::take_channel();
        case Kind::Wifi: return wifi::take_channel();
        case Kind::None: break;
    }
    return 0;
}

uint8_t hunt_misses() {
#if CONFIG_POT_EXTENDER
    if (g_kind == Kind::Extender) return CONFIG_POT_EXTENDER_HUNT_MISSES;
#endif
#if CONFIG_POT_WIFI
    if (g_kind == Kind::Wifi) return CONFIG_POT_WIFI_HUNT_MISSES;
#endif
    return CONFIG_POT_HB_MISS_LIMIT;
}

void prefer(uint8_t channel) {
    if (g_kind == Kind::Wifi && channel != 0) wifi::prefer_channel(channel);
}

bool defer_to(uint8_t channel) {
    if (g_kind != Kind::Wifi || channel == 0) return false;
    wifi::prefer_channel(channel);
    return wifi::repick();
}

}  // namespace pot::uplink
