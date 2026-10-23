"""M15: telemetry over the cell, the host's half -- the binary sections a board answers TELEMETRY with
(firmware: pot/telemetry.hpp; tests/test_telemetry.cpp builds the same golden bytes tests/test_telemetry.py reads).

    from potluck import telemetry as tm
    health = tm.health(bridge, 0x7368)          # None if the board did not answer
    links = tm.links(bridge, 0x7368)            # every peer, paged
    events, nxt = tm.events(bridge, 0x7368, since)

What a board printed to its USB console until M15, carried over the radio for the hub; every reply fits the cable's
226-byte payload floor, so a section too large for one reply is paged.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

from . import frame as fr

VERSION = 1
HEALTH, LINKS, ACTORS, EVENTS, AUTH, CHIP = 1, 2, 3, 4, 5, 6

HEALTH_NO_RADIO, HEALTH_BUSY, HEALTH_CONNECTED, HEALTH_LONG_WINDOW, HEALTH_ENROLLED = 1, 2, 4, 8, 16
LINK_CABLE, LINK_VERIFIED = 1, 2
ACTOR_RUNNING_HERE = 1

PEER_STATES = {0: "free", 1: "known", 2: "alive", 3: "dead", 4: "left"}
EVENT_KINDS = ["none", "peer_discovered", "peer_admitted", "peer_alive", "peer_dead", "peer_revived", "peer_rebooted",
               "peer_left", "bad_frame", "tx_error", "probe_timeout", "version_pinned", "safe_state_rx",
               "peer_refused", "frame_rejected", "safe_state_rejected", "channel_changed", "actor_started",
               "actor_stopped", "actor_owner"]

_HEALTH = struct.Struct("<IIBBHIIIIIBBBB12IHH")
_LINK = struct.Struct("<HBbBBHBB6IHHH")
_ACTOR = struct.Struct("<IBBHHIHH")
_EVENT = struct.Struct("<IIBHII")
_AUTH = struct.Struct("<12I")
_CHIP = struct.Struct("<BBHHHI")
assert (_HEALTH.size, _LINK.size, _ACTOR.size, _EVENT.size, _AUTH.size, _CHIP.size) == (88, 40, 18, 19, 48, 12)

#: The Auth section's counters, in their wire order (pot/telemetry.hpp, Node::AuthCounters; 2026-10-19).
AUTH_FIELDS = ("verified", "cache_hits", "refused", "rate_limited", "stale_epoch", "tags_ok", "bad_tag", "replayed",
               "untagged", "bcast_dropped", "beacon_epoch_ignored", "group_refusals")


#: esp_chip_model_t (ESP-IDF esp_chip_info.h), named as a person reads it (M21, 2026-10-23).
CHIP_MODELS = {1: "ESP32", 2: "ESP32-S2", 9: "ESP32-S3", 5: "ESP32-C3", 12: "ESP32-C2", 13: "ESP32-C6", 16: "ESP32-H2",
               18: "ESP32-P4", 20: "ESP32-C61", 23: "ESP32-C5", 25: "ESP32-H21", 28: "ESP32-H4"}
#: CHIP_FEATURE_* (esp_chip_info.h): bit -> name.
CHIP_FEATURES = {0: "embedded_flash", 1: "wifi", 4: "ble", 5: "bt_classic", 6: "ieee802154", 7: "embedded_psram"}


class TelemetryError(ValueError):
    """Bytes that are not the section asked for (or a board that could not be reached)."""


def encode_request(target: int, section: int, *, start: int = 0, since: int = 0) -> bytes:
    return struct.pack("<HBBI", target, section, start, since & 0xFFFFFFFF)


@dataclass
class Health:
    node: int
    uptime_s: int
    boot_epoch: int
    channel: int
    flags: int
    hb_period_ms: int
    int_free: int
    int_min: int
    int_largest: int
    ext_free: int
    ext_min: int
    peers_alive: int
    peers_dead: int
    actors_pinned: int
    actors_portable_here: int
    deaths: int
    revivals: int
    reboots_seen: int
    rx_total: int
    rx_rejected: int
    rx_queue_dropped: int
    tx_total: int
    tx_enqueue_err: int
    tx_done_dropped: int
    events_dropped: int
    writes_served: int
    writes_rejected: int
    link_stack_free: int
    int_total_kib: int = 0  # 0 from a board that does not say

    @property
    def long_window(self) -> bool:
        return bool(self.flags & HEALTH_LONG_WINDOW)

    def flag_names(self) -> list[str]:
        names = {HEALTH_NO_RADIO: "no_radio", HEALTH_BUSY: "busy", HEALTH_CONNECTED: "connected",
                 HEALTH_LONG_WINDOW: "long_window", HEALTH_ENROLLED: "enrolled"}
        return [n for bit, n in names.items() if self.flags & bit]


@dataclass
class Link:
    node: int
    state: int
    rssi: int
    espnow_version: int
    flags: int
    hb_period_ms: int
    miss_limit: int
    misses: int
    tx_frames: int
    tx_cb_ok: int
    tx_cb_fail: int
    rx_frames: int
    rx_lost: int
    rx_bcast: int
    rtt_p50_us100: int
    rtt_p99_us100: int
    rtt_timeouts: int

    @property
    def state_name(self) -> str:
        return PEER_STATES.get(self.state, str(self.state))

    @property
    def delivery(self) -> float | None:
        """Outbound delivery as the radio's acknowledgements say -- None when nothing was sent to it (the cable)."""
        n = self.tx_cb_ok + self.tx_cb_fail
        return round(self.tx_cb_ok / n, 4) if n else None

    @staticmethod
    def _ms(v: int) -> float | None:
        return None if v >= 0xFFFE else round(v / 10, 1)

    @property
    def rtt_p50_ms(self) -> float | None:
        return self._ms(self.rtt_p50_us100)

    @property
    def rtt_p99_ms(self) -> float | None:
        return self._ms(self.rtt_p99_us100)


@dataclass
class ActorTel:
    key: int
    flags: int
    owner: int
    assigned: int
    activations: int
    fenced: int
    released: int


@dataclass
class EventTel:
    seq: int
    at_ms: int
    kind: int
    node: int
    a: int
    b: int

    @property
    def kind_name(self) -> str:
        return EVENT_KINDS[self.kind] if self.kind < len(EVENT_KINDS) else f"kind_{self.kind}"


def _header(raw: bytes, section: int) -> int:
    if len(raw) < 4:
        raise TelemetryError(f"telemetry reply of {len(raw)} bytes")
    version, sec, node = struct.unpack_from("<BBH", raw, 0)
    if version != VERSION:
        raise TelemetryError(f"telemetry version {version}")
    if sec == 0:
        raise TelemetryError(f"board 0x{node:04x} could not be reached (or does not know section {section})")
    if sec != section:
        raise TelemetryError(f"asked for section {section}, got {sec}")
    return node


def parse_health(raw: bytes) -> Health:
    node = _header(raw, HEALTH)
    if len(raw) < 4 + _HEALTH.size:
        raise TelemetryError("health section cut short")
    return Health(node, *_HEALTH.unpack_from(raw, 4))


def parse_links(raw: bytes) -> tuple[int, int, list[Link]]:
    _header(raw, LINKS)
    if len(raw) < 7:
        raise TelemetryError("links section cut short")
    total, start, n = raw[4], raw[5], raw[6]
    if len(raw) < 7 + n * _LINK.size:
        raise TelemetryError("links section cut short")
    return total, start, [Link(*_LINK.unpack_from(raw, 7 + i * _LINK.size)) for i in range(n)]


def parse_actors(raw: bytes) -> tuple[int, int, list[ActorTel]]:
    _header(raw, ACTORS)
    if len(raw) < 7:
        raise TelemetryError("actors section cut short")
    total, start, n = raw[4], raw[5], raw[6]
    if len(raw) < 7 + n * _ACTOR.size:
        raise TelemetryError("actors section cut short")
    out = []
    for i in range(n):
        key, flags, _r, owner, assigned, act, fenced, released = _ACTOR.unpack_from(raw, 7 + i * _ACTOR.size)
        out.append(ActorTel(key, flags, owner, assigned, act, fenced, released))
    return total, start, out


def parse_auth(raw: bytes) -> dict[str, int]:
    _header(raw, AUTH)
    if len(raw) < 4 + _AUTH.size:
        raise TelemetryError("auth section cut short")
    return dict(zip(AUTH_FIELDS, _AUTH.unpack_from(raw, 4)))


def parse_chip(raw: bytes) -> dict:
    """M21: what the board is, as it reads itself -- `model` named (`model_id` the raw code), `revision` as "v0.2"."""
    _header(raw, CHIP)
    if len(raw) < 4 + _CHIP.size:
        raise TelemetryError("chip section cut short")
    model, cores, rev, flash_mb, psram_mb, feats = _CHIP.unpack_from(raw, 4)
    return {"model": CHIP_MODELS.get(model, f"chip {model}"), "model_id": model, "revision": f"v{rev // 100}.{rev % 100}",
            "cores": cores, "flash_mb": flash_mb, "psram_mb": psram_mb,
            "features": [name for bit, name in sorted(CHIP_FEATURES.items()) if feats >> bit & 1]}


def parse_events(raw: bytes) -> tuple[int, list[EventTel]]:
    _header(raw, EVENTS)
    if len(raw) < 9:
        raise TelemetryError("events section cut short")
    nxt, n = struct.unpack_from("<IB", raw, 4)
    if len(raw) < 9 + n * _EVENT.size:
        raise TelemetryError("events section cut short")
    return nxt, [EventTel(*_EVENT.unpack_from(raw, 9 + i * _EVENT.size)) for i in range(n)]


# ---- asking a board, through any Bridge (a hub included) ------------------------------------------------------

def _ask(bridge, target: int, section: int, *, start: int = 0, since: int = 0, timeout: float = 2.0) -> bytes:
    return bridge.deploy_step(fr.Op.TELEMETRY, encode_request(target, section, start=start, since=since),
                              timeout=timeout)


def health(bridge, target: int, *, timeout: float = 2.0) -> Health:
    return parse_health(_ask(bridge, target, HEALTH, timeout=timeout))


def auth(bridge, target: int, *, timeout: float = 2.0) -> dict[str, int]:
    """Its HELLO verification and frame tag counters: how a board kept out of the cell is told apart (2026-10-19)."""
    return parse_auth(_ask(bridge, target, AUTH, timeout=timeout))


def chip(bridge, target: int, *, timeout: float = 2.0) -> dict:
    """M21: the board's chip, flash and PSRAM, from the board itself (no esptool check, which restarts it)."""
    return parse_chip(_ask(bridge, target, CHIP, timeout=timeout))


def links(bridge, target: int, *, timeout: float = 2.0) -> list[Link]:
    out: list[Link] = []
    start = 0
    while True:
        total, _, got = parse_links(_ask(bridge, target, LINKS, start=start, timeout=timeout))
        out.extend(got)
        start += len(got)
        if not got or start >= total:
            return out


def actors(bridge, target: int, *, timeout: float = 2.0) -> list[ActorTel]:
    out: list[ActorTel] = []
    start = 0
    while True:
        total, _, got = parse_actors(_ask(bridge, target, ACTORS, start=start, timeout=timeout))
        out.extend(got)
        start += len(got)
        if not got or start >= total:
            return out


def events(bridge, target: int, since: int, *, timeout: float = 2.0, max_pages: int = 6) -> tuple[list[EventTel], int]:
    """Events with seq > since, and the seq the board's next event will get. A `next` below `since` means the board
    rebooted (its log starts again at 1): the caller should ask again from 0."""
    out: list[EventTel] = []
    nxt = since + 1
    for _ in range(max_pages):
        nxt, got = parse_events(_ask(bridge, target, EVENTS, since=since, timeout=timeout))
        out.extend(got)
        if not got:
            break
        since = got[-1].seq
        if since + 1 >= nxt:
            break
    return out, nxt
