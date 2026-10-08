"""M8, section 7.5: potluck-agent answers a node's CALL for a service its user offers, refuses the
rest at once, and a plain bridge (no agent) still ignores requests. The node end is a hand-driven
loopback, so these run without hardware and use the real framing both ways."""

from __future__ import annotations

import os
import struct
import sys
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from potluck import frame as fr
from potluck.agent import Agent
from potluck.bridge import HOST_NODE_ID, Bridge
from potluck.ns_payloads import Call, Read, Reply
from potluck.paths import path_hash
from potluck.serial_framing import SerialReassembler, write_serial_frame
from potluck.transport import LoopbackTransport
from potluck.value import NsError, Quality, ValueType

NODE = 0x6300
TIME = path_hash("potluck://lab/svc/time")


class FakeNode:
    def __init__(self, t: LoopbackTransport) -> None:
        self.t = t
        self.rx = SerialReassembler()
        self.seq = 0

    def send(self, opcode: int, payload: bytes, msg_id: int) -> None:
        self.seq += 1
        raw = fr.encode(src=NODE, dst=HOST_NODE_ID, opcode=opcode, lclass=4, priority=0, seq=self.seq,
                        msg_id=msg_id, payload=payload, ack_req=True)
        self.t.write(write_serial_frame(raw))

    def replies(self, timeout: float = 1.0) -> list[fr.Frame]:
        out: list[fr.Frame] = []
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            for raw in self.rx.feed(self.t.read(4096)):
                f = fr.parse(raw)
                if f.opcode == fr.Op.REPLY:
                    out.append(f)
            if out:
                return out
            time.sleep(0.01)
        return out


def make(with_agent: bool, services=("time",)):
    host, node = LoopbackTransport.pair()
    holder = {}
    bridge = Bridge(host, heartbeat=False, on_request=(lambda f: holder["a"].handle(f)) if with_agent else None)
    if with_agent:
        holder["a"] = Agent(bridge, cluster="lab", services=list(services))
    bridge.start()
    return bridge, holder.get("a"), FakeNode(node)


class AgentServes(unittest.TestCase):
    def test_a_call_for_an_offered_service_is_answered_with_its_value(self):
        bridge, agent, node = make(True)
        try:
            before = int(time.time() * 1000)
            node.send(fr.Op.CALL, Call(path_hash=TIME).encode(), msg_id=77)
            (f,) = node.replies()
            self.assertEqual(f.msg_id, 77)
            self.assertEqual(f.dst, NODE)
            rep = Reply.parse(f.payload)
            self.assertEqual(rep.status, NsError.OK)
            self.assertEqual(rep.quality, Quality.GOOD)
            self.assertEqual(rep.reply_to, fr.Op.CALL)
            self.assertEqual(rep.value.type, ValueType.U64)
            (ms,) = struct.unpack("<Q", rep.value.raw[:8])
            self.assertTrue(before <= ms <= int(time.time() * 1000))
            self.assertEqual(agent.stats.served, 1)
        finally:
            bridge.close()

    def test_any_other_path_is_refused_at_once_not_left_to_time_out(self):
        bridge, agent, node = make(True)
        try:
            node.send(fr.Op.CALL, Call(path_hash=path_hash("potluck://lab/svc/slam")).encode(), msg_id=5)
            (f,) = node.replies()
            rep = Reply.parse(f.payload)
            self.assertEqual(rep.status, NsError.NOT_FOUND)
            self.assertEqual(rep.quality, Quality.UNAVAILABLE)
            self.assertEqual(agent.stats.refused, 1)
            node.send(fr.Op.READ, Read(path_hash=TIME).encode(), msg_id=6)  # a service is not a stored value
            (f,) = node.replies()
            self.assertEqual(Reply.parse(f.payload).status, NsError.NOT_FOUND)
        finally:
            bridge.close()

    def test_nothing_is_offered_unless_the_user_names_it(self):
        with self.assertRaises(ValueError):
            Agent(None, services=["shell"])

    def test_a_plain_bridge_still_ignores_requests(self):
        bridge, _agent, node = make(False)
        try:
            node.send(fr.Op.CALL, Call(path_hash=TIME).encode(), msg_id=9)
            self.assertEqual(node.replies(timeout=0.3), [])
            self.assertEqual(bridge.stats.requests_rx, 1)
        finally:
            bridge.close()


if __name__ == "__main__":
    unittest.main()
