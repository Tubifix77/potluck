"""M24: key and pointer events from the PC (potluck.events), the hub's route and delivery, the host node's call record,
and the key-echo test app's compiled config."""

import struct
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace as NS

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from potluck import events as ev  # noqa: E402
from potluck import frame as fr  # noqa: E402
from potluck.paths import path_hash  # noqa: E402


class Check(unittest.TestCase):
    def test_keys_and_pointers_become_wire_tuples_and_bad_ones_are_named(self) -> None:
        self.assertEqual(ev.check_events([{"kind": "key", "code": 4, "down": True},
                                          {"kind": "pointer", "dx": -3, "dy": 7, "buttons": 1}], ("key", "pointer")),
                         [(1, 1, 4, 0), (2, 1, -3, 7)])
        for bad, why in (([], "list"), ([{"kind": "wheel"}], "kind"), ([{"kind": "key", "code": 70000}], "code"),
                         ([{"kind": "key", "code": 4, "down": 1}], "down"), ([{"kind": "pointer", "dx": 1.5}], "dx")):
            with self.assertRaisesRegex(ev.InputError, why):
                ev.check_events(bad, ("key", "pointer"))
        with self.assertRaisesRegex(ev.InputError, "does not take pointer"):
            ev.check_events([{"kind": "pointer"}], ("key",))

    def test_a_batch_is_laid_out_as_pot_input_parses_it(self) -> None:
        b = ev.encode_batch([(7, 1, 1, 0x04, 0, 0.0), (8, 2, 0, -1, 2, 0.0)], 0xBEEF)
        self.assertEqual(b[:4], bytes([2, 2]) + struct.pack("<H", 0xBEEF))  # version 2, two events, the stream
        self.assertEqual(struct.unpack_from("<HBBhh", b, 4), (7, 1, 1, 4, 0))
        self.assertEqual(struct.unpack_from("<HBBhh", b, 12), (8, 2, 0, -1, 2))
        self.assertEqual(len(ev.encode_batch([(i, 1, 0, 0, 0) for i in range(ev.BATCH_MAX)], 1)), 4 + 8 * ev.BATCH_MAX)

    def test_each_queue_has_a_stream_of_its_own(self) -> None:
        """A restarted hub numbers from 1 again: its stream tells the board these are new (2026-10-25)."""
        a, b = (ev.InputQueue("echo", 0x8160, 1, ("key",)) for _ in range(2))
        self.assertTrue(1 <= a.stream <= 0xFFFF)
        a.put([(1, 1, 4, 0)], 0.0)
        self.assertEqual(struct.unpack_from("<H", a.due(0.0), 2)[0], a.stream)
        self.assertNotEqual(a.stream, b.stream)  # 1 in 65535 to fail by chance


class Queue(unittest.TestCase):
    def q(self) -> ev.InputQueue:
        return ev.InputQueue("echo", 0x8160, 1234, ("key",))

    def test_in_order_one_batch_at_a_time_answered_then_the_next(self) -> None:
        q = self.q()
        self.assertEqual(q.put([(1, 1, 4, 0)] * 30, 0.0), 1)
        first = q.due(0.0)
        self.assertEqual(first[1], ev.BATCH_MAX)
        self.assertIsNone(q.due(ev.RESEND_S / 4))  # one batch out at a time
        self.assertEqual(q.ack(ev.BATCH_MAX, 0.05), ev.BATCH_MAX)
        second = q.due(0.06)
        self.assertEqual((second[1], struct.unpack_from("<H", second, 4)[0]), (6, ev.BATCH_MAX + 1))
        q.ack(30, 0.1)
        snap = q.snapshot()
        self.assertEqual((snap["taken"], snap["waiting"], snap["batches"], snap["resent"]), (30, 0, 2, 0))
        self.assertEqual(snap["p99_ms"], 100.0)

    def test_an_unanswered_batch_goes_again_and_a_late_answer_still_counts(self) -> None:
        q = self.q()
        q.put([(1, 1, 4, 0)] * 3, 0.0)
        a = q.due(0.0)
        self.assertIsNone(q.due(ev.RESEND_S / 2))
        self.assertEqual(q.due(ev.RESEND_S), a)  # the same numbers again
        q.ack(2, ev.RESEND_S + 0.01)  # part of it answered: the rest goes again at once
        self.assertEqual(q.due(ev.RESEND_S + 0.01)[1], 1)
        self.assertEqual(q.snapshot()["resent"], 1)

    def test_numbers_wrap_and_a_refused_batch_is_dropped_and_counted(self) -> None:
        q = self.q()
        q.next_seq = 0xFFFF
        q.put([(1, 1, 4, 0)] * 2, 0.0)
        q.due(0.0)
        self.assertEqual(q.ack(0, 0.1), 2)  # 65535 and 0
        q.put([(1, 1, 5, 0)], 0.2)
        q.due(0.2)
        q.refuse()
        self.assertEqual((q.snapshot()["refused"], q.snapshot()["waiting"]), (1, 0))
        with self.assertRaisesRegex(ev.InputError, "already waiting"):
            q.put([(1, 1, 4, 0)] * (ev.QUEUE_MAX + 1), 0.3)

    def test_the_pump_keeps_a_batch_it_could_not_send(self) -> None:
        sent, reachable = [], [False]
        inputs = ev.Inputs(lambda owner, h, args: reachable[0] and not sent.append((owner, h, args)), clock=lambda: 0.0)
        q = ev.InputQueue("echo", 0x8160, 1234, ("key",))
        inputs.queues["echo"] = q
        q.put([(1, 1, 4, 0)], 0.0)
        inputs.pump()
        self.assertEqual(sent, [])
        reachable[0] = True
        inputs.pump()
        self.assertEqual(len(sent), 1)
        self.assertEqual(q.snapshot()["resent"], 0)


class Fair(unittest.TestCase):
    def test_a_keyboards_rate_is_inside_the_input_share_and_does_not_spend_the_write_share(self) -> None:
        from potluck.hub_clients import FairShare
        f = FairShare()
        self.assertTrue(all(f.take("kbd", "input") <= 0 for _ in range(60)))  # 60 keys at once: inside the burst
        self.assertLessEqual(f.take("kbd", "write"), 0)  # writes keep their own share


class HubSide(unittest.TestCase):
    def setUp(self) -> None:
        from potluck.manifest import parse
        doc = {"schema": 1, "system": "lab", "min_core_version": 1,
               "nodes": [{"node_id": 0x6300, "label": "A"}, {"node_id": 0x8160, "label": "C"}],
               "actors": [{"name": "echo", "module": "builtin:key_echo", "latency_class": 4, "pin": 0x8160},
                          {"name": "die", "module": "builtin:die_temp", "latency_class": 4, "pin": 0x6300}]}
        self.m = parse(doc)
        self.sent = []
        self.inputs = ev.Inputs(lambda owner, h, args: self.sent.append((owner, h, args)) or True)
        self.model = NS(package=NS(manifest=self.m), hub=NS(inputs=self.inputs))

    def test_the_route_queues_for_the_apps_board_and_refuses_what_it_cannot_deliver(self) -> None:
        from potluck import hub as hb
        code, body = hb.input_events(self.model, {"app": "echo", "events": [{"kind": "key", "code": 4}]})
        self.assertEqual((code, body["first"], body["board"]), (202, 1, "8160"))
        q = self.inputs.queues["echo"]
        self.assertEqual((q.owner, q.path_hash), (0x8160, path_hash("potluck://lab/act/echo/in")))
        self.assertEqual(hb.input_events(self.model, {"app": "nope", "events": []})[0], 404)
        self.assertEqual(hb.input_events(self.model, {"app": "die", "events": [{"kind": "key", "code": 4}]})[0], 409)
        self.assertEqual(hb.input_events(self.model, {"app": "echo", "events": [{"kind": "wheel"}]})[0], 400)
        self.assertEqual(hb.input_events(self.model, {"events": []})[0], 400)
        self.inputs.stop()

    def test_the_apps_answer_seen_on_the_cable_acks_the_batch(self) -> None:
        from potluck import hub as hb
        from potluck.ns_payloads import Reply
        from potluck.transport import LoopbackTransport
        from potluck.value import Value
        host_side, _ = LoopbackTransport.pair()
        hub = hb.Hub(host_side)
        h = path_hash("potluck://lab/act/echo/in")
        q = hub.inputs.queue("echo", 0x8160, h, ("key",))
        q.put([(1, 1, 4, 0)] * 2, 0.0)
        q.due(0.0)  # out (nothing attached to send it through: it waits as in flight)
        rep = Reply(path_hash=h, timestamp_ms=0, age_ms=0, unit=0, reply_to=fr.Op.CALL, status=0, quality=1,
                    latency_class=4, value=Value.of_u32(2))
        # passed on by the cabled board: the answer comes back from it, not from the app's board
        hub._input_reply(fr.parse(fr.encode(src=0x6300, dst=0x00FE, opcode=fr.Op.REPLY, msg_id=0x8005,
                                            payload=rep.encode())))
        self.assertEqual(q.snapshot()["taken"], 2)
        hub.inputs.stop()

    def test_the_batch_goes_to_the_cabled_board_marked_to_be_passed_on(self) -> None:
        from potluck import hub as hb
        from potluck.ns_payloads import CALL_FORWARD, Call
        from potluck.transport import LoopbackTransport
        host_side, _ = LoopbackTransport.pair()
        hub = hb.Hub(host_side)
        sent = []
        hub.send_frame = lambda op, payload, dst=None, msg_id=0, lclass=3, **kw: sent.append((op, dst, Call.parse(payload)))
        args = ev.encode_batch([(1, 1, 1, 4, 0)], 7)
        self.assertFalse(hub._send_input(0x8160, 0xABCD1234, args))  # no board on the cable yet: kept
        hub.peer_node_id = 0x6300
        self.assertTrue(hub._send_input(0x8160, 0xABCD1234, args))
        op, dst, call = sent[-1]
        self.assertEqual((op, dst, call.path_hash, call.flags), (fr.Op.CALL, 0x6300, 0xABCD1234, CALL_FORWARD))
        self.assertEqual(call.args, struct.pack("<H", 0x8160) + args)  # the member first, then the batch
        self.assertTrue(hub._send_input(0x6300, 0xABCD1234, args))  # the cabled board's own app: a plain CALL
        self.assertEqual((sent[-1][2].flags, sent[-1][2].args), (0, args))
        hub.inputs.stop()


class Compile(unittest.TestCase):
    def test_key_echo_compiles_to_its_input_and_three_resources(self) -> None:
        from potluck import deploy as dp
        from potluck.manifest import parse
        doc = {"schema": 1, "system": "lab", "min_core_version": 1, "nodes": [{"node_id": 0x8160, "label": "C"}],
               "actors": [{"name": "echo", "module": "builtin:key_echo", "latency_class": 4, "pin": 0x8160}]}
        img = dp.compile_image(parse(doc), 1)
        at = dp.IMAGE_HEADER_LEN
        self.assertEqual(struct.unpack_from("<HBB", img, at), (0x8160, dp.ACTOR_KEY_ECHO, 16))
        self.assertEqual(struct.unpack_from("<IIII", img, at + 4),
                         tuple(path_hash(f"potluck://lab/act/echo/{n}") for n in ("in", "key", "count", "gaps")))
        bad = dict(doc, actors=[dict(doc["actors"][0], config={"x": 1})])
        with self.assertRaises(dp.DeployError):
            dp.compile_image(parse(bad), 1)


if __name__ == "__main__":
    unittest.main()
