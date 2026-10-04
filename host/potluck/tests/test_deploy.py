"""Section 7.4 deploy, host half: compiling a package to the node image, and reading the replies.

The golden image below is checked here AND parsed by tests/test_deploy.cpp, so the host compiler and
the node parser cannot drift apart without one suite failing.
"""

from __future__ import annotations

import os
import struct
import sys
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from potluck import deploy as dp
from potluck import frame as fr
from potluck.bridge import Bridge, _Pending
from potluck.manifest import parse
from potluck.transport import LoopbackTransport


def manifest(actors: list[dict]) -> dict:
    return {
        "schema": 1,
        "system": "m3-test",
        "min_core_version": 1,
        "nodes": [{"node_id": 0x6300, "label": "a"}, {"node_id": 0x7368, "label": "b"}],
        "actors": actors,
    }


LED_A = {"name": "led_a", "module": "builtin:led", "latency_class": 4, "pin": 0x6300,
         "config": {"r": 160, "g": 0, "b": 200, "blink": True, "period_ms": 1500}}

#: compile_image(parse(manifest([LED_A])), counter=3), decoded by hand on 2026-10-04: magic POTD,
#: version 1, 1 actor, reserved, counter 3, the manifest's 8-byte digest, body 10 B, then node 0x6300,
#: LED, 6 config bytes = 160, 0, 200, blink, 1500 ms. tests/test_deploy.cpp parses these exact bytes.
#: The digest is the manifest's own, so a change to the canonical form shows up here too.
GOLDEN = "504f54440101000003000000bfba6702f4f20e6d0a00000000630106a000c801dc05"


class CompileImage(unittest.TestCase):
    def test_an_led_actor_compiles_to_the_node_format(self) -> None:
        m = parse(manifest([LED_A]))
        img = dp.compile_image(m, 3)
        magic, ver, count, _, counter, digest, body_len = struct.unpack_from("<IBBHI8sI", img)
        self.assertEqual(magic, dp.IMAGE_MAGIC)
        self.assertEqual((ver, count, counter), (1, 1, 3))
        self.assertEqual(digest, bytes.fromhex(m.digest())[:8])
        self.assertEqual(body_len, len(img) - dp.IMAGE_HEADER_LEN)
        node, typ, cfg_len = struct.unpack_from("<HBB", img, dp.IMAGE_HEADER_LEN)
        self.assertEqual((node, typ, cfg_len), (0x6300, dp.ACTOR_LED, 6))
        r, g, b, blink, period = struct.unpack_from("<BBBBH", img, dp.IMAGE_HEADER_LEN + 4)
        self.assertEqual((r, g, b, blink, period), (160, 0, 200, 1, 1500))

    def test_the_same_package_compiles_to_the_same_bytes(self) -> None:
        a = dp.compile_image(parse(manifest([LED_A])), 3)
        b = dp.compile_image(parse(manifest([LED_A])), 3)
        self.assertEqual(a, b)

    def test_golden_image(self) -> None:
        # Shared with tests/test_deploy.cpp, which parses these exact bytes.
        img = dp.compile_image(parse(manifest([LED_A])), 3)
        self.assertEqual(img.hex(), GOLDEN)

    def test_a_module_this_firmware_lacks_is_refused_by_name(self) -> None:
        bad = dict(LED_A, module="vent_loop.so")
        with self.assertRaises(dp.DeployError) as cm:
            dp.compile_image(parse(manifest([bad])), 1)
        self.assertIn("vent_loop.so", str(cm.exception))

    def test_an_unplaced_actor_is_refused(self) -> None:
        bad = {k: v for k, v in LED_A.items() if k != "pin"}
        with self.assertRaises(dp.DeployError):
            dp.compile_image(parse(manifest([bad])), 1)

    def test_out_of_range_config_is_refused_before_anything_is_sent(self) -> None:
        for cfg in ({"r": 300}, {"period_ms": 50}, {"colour": 1}):
            with self.subTest(cfg=cfg):
                with self.assertRaises(dp.DeployError):
                    dp.compile_image(parse(manifest([dict(LED_A, config=cfg)])), 1)

    def test_config_is_signed_but_absent_config_changes_nothing(self) -> None:
        with_cfg = parse(manifest([LED_A]))
        self.assertIn(b'"config"', with_cfg.canonical_bytes())
        without = parse(manifest([{k: v for k, v in LED_A.items() if k != "config"}]))
        self.assertNotIn(b'"config"', without.canonical_bytes())
        self.assertNotEqual(with_cfg.digest(), without.digest())


class Payloads(unittest.TestCase):
    def test_begin_chunk_commit_layouts(self) -> None:
        img = bytes(range(40))
        b = dp.begin_payload(img, 7, b"\x01" * 8, distribute=True)
        self.assertEqual(len(b), 24)
        n, crc, counter, digest, flags = struct.unpack_from("<III8sB", b)
        self.assertEqual((n, crc, counter, flags), (40, dp.crc32(img), 7, dp.FLAG_DISTRIBUTE))
        c = dp.chunk_payload(8, img[8:20])
        self.assertEqual(struct.unpack_from("<IH", c), (8, 12))
        self.assertEqual(c[6:], img[8:20])
        self.assertEqual(dp.commit_payload(img), struct.pack("<I", dp.crc32(img)))

    def test_a_reply_parses_with_its_status_name(self) -> None:
        raw = struct.pack("<HBBIIBB2x", 7, 2, 1, 40, 3, 2, 0)
        r = dp.DeployReply.parse(raw)
        self.assertFalse(r.ok)
        self.assertEqual(r.status_name, "TRIAL_IN_PROGRESS")
        self.assertEqual((r.slot, r.peers_ok), (2, 2))


class BridgeRoutesDeployReplies(unittest.TestCase):
    def test_a_deploy_reply_is_handed_back_raw_not_parsed_as_a_reading(self) -> None:
        host, _node = LoopbackTransport.pair()
        bridge = Bridge(host, heartbeat=False)
        try:
            pend = _Pending(msg_id=42, op=fr.Op.DEPLOY_BEGIN, path_hash=0,
                            sent_monotonic=time.monotonic())
            bridge._pending[42] = pend
            payload = struct.pack("<HBBIIBB2x", 0, 1, 0, 0, 3, 0, 0)
            raw = fr.encode(src=0x6300, dst=bridge.node_id, opcode=fr.Op.REPLY, msg_id=42,
                            payload=payload)
            bridge._on_raw_frame(raw)
            self.assertTrue(pend.done.is_set())
            self.assertEqual(pend.reply, payload)
            self.assertEqual(bridge.stats.bad_frames, 0)
        finally:
            bridge.close()



if __name__ == "__main__":
    unittest.main()
