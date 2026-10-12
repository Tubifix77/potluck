"""M15: telemetry over the cell, the host's half -- the bytes a board answers TELEMETRY with (tests/test_telemetry.cpp
checks that the board's encoder makes exactly these), and what the hub's model makes of them."""

from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from potluck import telemetry as tm  # noqa: E402

#: C's health: up 3600 s, epoch 344, channel 1, connected and enrolled, 191,603 B internal free, PSRAM low-water
#: 7,000,000 B, three peers alive, two deaths, nine writes refused, 1,234 B of link-task stack left.
HEALTH_GOLDEN = ("01016081100e0000580100000114640073ec0200000000000000000000000000c0cf6a000300000002000000000000000000"
                 "0000000000000000000000000000000000000000000000000000000000000000000009000000d2040000")
#: A's view of B: alive, -45 dBm, ESP-NOW v2, verified, window 60 x 100 ms, 990 of 1000 acknowledged, 7 lost, RTT
#: p50 <= 5.1 ms, p99 <= 16.0 ms, timeouts saturated.
LINKS_GOLDEN = "01020063020001687302d3020264003c01e8030000de0300000a000000d007000007000000280000003300a000ffff"


class Sections(unittest.TestCase):
    def test_health(self) -> None:
        h = tm.parse_health(bytes.fromhex(HEALTH_GOLDEN))
        self.assertEqual((h.node, h.uptime_s, h.boot_epoch, h.channel), (0x8160, 3600, 344, 1))
        self.assertEqual(h.flag_names(), ["connected", "enrolled"])
        self.assertFalse(h.long_window)
        self.assertEqual((h.int_free, h.ext_min, h.peers_alive, h.deaths, h.writes_rejected, h.link_stack_free),
                         (191603, 7000000, 3, 2, 9, 1234))
        with self.assertRaises(tm.TelemetryError):
            tm.parse_health(bytes.fromhex(HEALTH_GOLDEN)[:-1])
        with self.assertRaises(tm.TelemetryError):
            tm.parse_links(bytes.fromhex(HEALTH_GOLDEN))  # not the section asked for

    def test_links(self) -> None:
        total, start, links = tm.parse_links(bytes.fromhex(LINKS_GOLDEN))
        self.assertEqual((total, start, len(links)), (2, 0, 1))
        k = links[0]
        self.assertEqual((k.node, k.state_name, k.rssi, k.misses), (0x7368, "alive", -45, 1))
        self.assertEqual(k.delivery, 0.99)
        self.assertEqual((k.rtt_p50_ms, k.rtt_p99_ms), (5.1, 16.0))
        self.assertEqual(k.hb_period_ms * k.miss_limit, 6000)

    def test_an_unreachable_board_is_an_error_naming_it(self) -> None:
        with self.assertRaisesRegex(tm.TelemetryError, "0x7368 could not be reached"):
            tm.parse_health(bytes([1, 0, 0x68, 0x73]))

    def test_the_request(self) -> None:
        self.assertEqual(tm.encode_request(0x8160, tm.EVENTS, since=0xA1B2C3D4).hex(), "60810400d4c3b2a1")


class Paging(unittest.TestCase):
    def test_events_are_paged_until_the_boards_next(self) -> None:
        import struct

        log = [tm.EventTel(s, s * 10, 4, 0x7368, 0, 0) for s in range(1, 16)]

        class B:
            def deploy_step(self, op, payload, timeout=2.0):
                since = struct.unpack_from("<I", payload, 4)[0]
                page = [e for e in log if e.seq > since][:11]
                raw = struct.pack("<BBHIB", 1, tm.EVENTS, 0x8160, 16, len(page))
                for e in page:
                    raw += struct.pack("<IIBHII", e.seq, e.at_ms, e.kind, e.node, e.a, e.b)
                return raw

        got, nxt = tm.events(B(), 0x8160, 0)
        self.assertEqual([e.seq for e in got], list(range(1, 16)))
        self.assertEqual(nxt, 16)
        self.assertEqual(got[0].kind_name, "peer_dead")


if __name__ == "__main__":
    unittest.main()
