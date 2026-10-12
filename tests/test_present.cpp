// M13: the presenting role's portable half -- the bytes Home Assistant reads and the commands it sends.
//
// Each check pins one rule of ARCHITECTURE section 13: the cell's identity in every id, availability one topic
// per presenter with `any`, a board's death and a strict resource's loss of GOOD both turning an entity
// unavailable, the tuple in every state, and an outside write parsed strictly.

#include <cstring>
#include <limits>
#include <string>

#include "pot/present_core.hpp"
#include "test_harness.hpp"

using namespace pot;
using namespace pot::present;

namespace {

const uint16_t kPresenters[] = {0x7368, 0x6300};

Cell cell() {
    Cell c;
    c.id = "ef76cc2e";
    c.system = "lab";
    c.presenters = kPresenters;
    c.n_presenters = 2;
    return c;
}

Entity die_temp() {
    Entity e;
    e.hash = 0x57d40a8e;
    e.outside = kOutsideExposed;
    e.owner = 0x7368;
    e.type = ValueType::F32;
    e.unit = Unit::Celsius;
    e.bound_ms = 5000;
    e.name = "node-7368/hw/die_temp";
    e.name_len = std::strlen(e.name);
    return e;
}

Entity led() {
    Entity e;
    e.hash = 0xd1db1882;
    e.outside = kOutsideExposed | kOutsideWritable;
    e.owner = 0x7368;
    e.type = ValueType::Bool;
    e.name = "node-7368/led/on";
    e.name_len = std::strlen(e.name);
    return e;
}

std::string s(const char* p, size_t n) { return std::string(p, n); }
bool has(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

}  // namespace

TEST(present, components_follow_the_type_and_the_outside_flags) {
    CHECK(component_of(die_temp()) == Component::Sensor);
    CHECK(component_of(led()) == Component::Switch);
    Entity ro = led();
    ro.outside = kOutsideExposed;
    CHECK(component_of(ro) == Component::BinarySensor);
    Entity num = die_temp();
    num.outside = kOutsideExposed | kOutsideWritable;
    CHECK(component_of(num) == Component::Number);
    Entity safety = led();
    safety.outside = kOutsideExposed | kOutsideSafety;
    CHECK(component_of(safety) == Component::BinarySensor);  // safety-relevant: shown, never a control
    Entity bytes = die_temp();
    bytes.type = ValueType::Bytes;
    CHECK(component_of(bytes) == Component::None);
}

TEST(present, topics_carry_the_cells_identity) {
    char t[96];
    const Cell c = cell();
    CHECK(s(t, config_topic(c, die_temp(), t, sizeof(t))) == "homeassistant/sensor/potluck_ef76cc2e/57d40a8e/config");
    CHECK(s(t, config_topic(c, led(), t, sizeof(t))) == "homeassistant/switch/potluck_ef76cc2e/d1db1882/config");
    CHECK(s(t, state_topic(c, 0x57d40a8e, t, sizeof(t))) == "potluck/ef76cc2e/57d40a8e/state");
    CHECK(s(t, command_topic(c, 0xd1db1882, t, sizeof(t))) == "potluck/ef76cc2e/d1db1882/set");
    CHECK(s(t, presenter_topic(c, 0x7368, t, sizeof(t))) == "potluck/ef76cc2e/presenter/7368");
    CHECK(s(t, command_filter(c, t, sizeof(t))) == "potluck/ef76cc2e/+/set");
    CHECK_EQ(config_topic(c, die_temp(), t, 10), static_cast<size_t>(0));  // short buffer: nothing, never a cut topic
}

TEST(present, a_sensors_config_has_its_unit_class_expiry_and_one_availability_topic_per_presenter) {
    char buf[1536];
    const std::string j = s(buf, config_payload(cell(), die_temp(), 2000, buf, sizeof(buf)));
    CHECK(!j.empty());
    CHECK(has(j, "\"name\":\"hw/die_temp\""));
    CHECK(has(j, "\"unique_id\":\"potluck_ef76cc2e_57d40a8e\""));
    CHECK(has(j, "\"unit_of_measurement\":\"\xc2\xb0" "C\""));
    CHECK(has(j, "\"device_class\":\"temperature\""));
    CHECK(has(j, "\"expire_after\":10"));  // max(3 x 2 s, 2 x 5 s) = 10 s
    CHECK(has(j, "\"availability_mode\":\"any\""));
    CHECK(has(j, "\"topic\":\"potluck/ef76cc2e/presenter/7368\""));
    CHECK(has(j, "\"topic\":\"potluck/ef76cc2e/presenter/6300\""));
    CHECK(has(j, "'7368' in value_json.nodes"));        // its board's liveness
    CHECK(has(j, "'57d40a8e' not in value_json.down"));  // and its own value
    CHECK(has(j, "\"identifiers\":[\"potluck_ef76cc2e_7368\"]"));
    CHECK(!has(j, "retain"));
    CHECK(!has(j, "command_topic"));
}

TEST(present, a_switch_has_a_command_topic_and_no_expiry) {
    char buf[1536];
    const std::string j = s(buf, config_payload(cell(), led(), 2000, buf, sizeof(buf)));
    CHECK(has(j, "\"command_topic\":\"potluck/ef76cc2e/d1db1882/set\""));
    CHECK(has(j, "{{ 'ON' if value_json.value else 'OFF' }}"));
    CHECK(!has(j, "expire_after"));  // Home Assistant's MQTT switch has none: availability carries a death
}

TEST(present, a_portable_actors_output_belongs_to_the_cell_not_a_board) {
    Entity e = die_temp();
    e.name = "act/alarm/out";
    e.name_len = std::strlen(e.name);
    char buf[1536];
    const std::string j = s(buf, config_payload(cell(), e, 2000, buf, sizeof(buf)));
    CHECK(has(j, "\"identifiers\":[\"potluck_ef76cc2e_cell\"]"));
    CHECK(!has(j, "in value_json.nodes"));  // it moves when its node dies; only its value decides
}

TEST(present, a_state_carries_the_whole_tuple_and_no_number_when_it_has_none_to_show) {
    Reading r;
    r.value = Value::of_f32(21.5f);
    r.unit = Unit::Celsius;
    r.age_ms = 120;
    r.latency_class = 4;
    r.quality = Quality::Good;
    char buf[256];
    CHECK(s(buf, state_payload(r, StalenessPolicy::Informative, 0, buf, sizeof(buf))) ==
          "{\"value\":21.5,\"unit\":\"C\",\"quality\":\"GOOD\",\"age_ms\":120,\"latency_class\":4}");
    std::string j = s(buf, state_payload(r, StalenessPolicy::Informative, 1800000000000ull, buf, sizeof(buf)));
    CHECK(has(j, "\"sampled_ms\":1799999999880"));
    r.quality = Quality::Stale;
    r.age_ms = 9000;
    CHECK(shows_value(r, StalenessPolicy::Informative));  // informative: shown, marked
    CHECK(!shows_value(r, StalenessPolicy::Strict));      // strict: no number
    j = s(buf, state_payload(r, StalenessPolicy::Strict, 0, buf, sizeof(buf)));
    CHECK(has(j, "\"value\":null"));
    CHECK(has(j, "\"quality\":\"STALE\""));
    r.quality = Quality::Unavailable;
    CHECK(!shows_value(r, StalenessPolicy::Informative));
    r.quality = Quality::Good;
    r.value = Value::of_f32(std::numeric_limits<float>::quiet_NaN());  // JSON has none
    j = s(buf, state_payload(r, StalenessPolicy::Informative, 0, buf, sizeof(buf)));
    CHECK(has(j, "\"value\":null"));
}

TEST(present, the_presenter_lists_the_live_boards_and_the_values_down_and_its_will_says_offline) {
    const uint16_t live[] = {0x6300, 0x7368};
    const uint32_t down[] = {0x57d40a8e};
    char buf[256];
    CHECK(s(buf, presenter_payload(0x7368, live, 2, down, 1, buf, sizeof(buf))) ==
          "{\"online\":1,\"node\":\"7368\",\"nodes\":[\"6300\",\"7368\"],\"down\":[\"57d40a8e\"]}");
    CHECK(s(buf, will_payload(0x7368, buf, sizeof(buf))) == "{\"online\":0,\"node\":\"7368\",\"nodes\":[],\"down\":[]}");
}

TEST(present, an_outside_command_is_parsed_strictly) {
    const Cell c = cell();
    uint32_t h = 0;
    const char* t = "potluck/ef76cc2e/d1db1882/set";
    CHECK(parse_command_topic(c, t, std::strlen(t), h));
    CHECK_EQ(h, 0xd1db1882u);
    const char* other_cell = "potluck/00000000/d1db1882/set";
    CHECK(!parse_command_topic(c, other_cell, std::strlen(other_cell), h));
    const char* upper = "potluck/ef76cc2e/D1DB1882/set";
    CHECK(!parse_command_topic(c, upper, std::strlen(upper), h));
    const char* state = "potluck/ef76cc2e/d1db1882/state";
    CHECK(!parse_command_topic(c, state, std::strlen(state), h));
    Value v;
    bool b = false;
    CHECK(parse_command_value(ValueType::Bool, "ON", 2, v) && v.as_bool(b) && b);
    CHECK(parse_command_value(ValueType::Bool, "OFF", 3, v) && v.as_bool(b) && !b);
    CHECK(!parse_command_value(ValueType::Bool, "on", 2, v));
    int32_t i = 0;
    CHECK(parse_command_value(ValueType::I32, "-42", 3, v) && v.as_i32(i) && i == -42);
    CHECK(!parse_command_value(ValueType::I32, "42x", 3, v));
    CHECK(!parse_command_value(ValueType::U32, "-1", 2, v));
    CHECK(!parse_command_value(ValueType::I32, "99999999999", 11, v));
    float f = 0;
    CHECK(parse_command_value(ValueType::F32, "2.5", 3, v) && v.as_f32(f) && f == 2.5f);
    CHECK(!parse_command_value(ValueType::Bytes, "00", 2, v));
}

TEST(present, a_write_event_echoes_the_payload_safely) {
    char buf[256];
    const std::string j =
        s(buf, write_event("node-7368/led/on", 16, 0xd1db1882, "ON\"},{\"x", 9, "sent", buf, sizeof(buf)));
    CHECK(j == "{\"what\":\"outside_write\",\"resource\":\"node-7368/led/on\",\"hash\":\"d1db1882\",\"value\":\"ON\","
               "\"result\":\"sent\"}");
}
