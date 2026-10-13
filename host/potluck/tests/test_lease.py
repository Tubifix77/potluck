"""M14: the cell's leases, the host's half -- the wire format the boards read (tests/test_lease.cpp parses the same
bytes), and a holder that takes, renews, is refused by name, and releases."""

from __future__ import annotations

import os
import struct
import sys
import threading
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from potluck import frame as fr  # noqa: E402
from potluck import lease as ls  # noqa: E402

#: encode_request(KIND_DEPLOY, OP_TAKE, 0xA1B2C3D4, ttl_s=90, name="apply on desk-pc", target=0x7368).
REQUEST_GOLDEN = "687301015a00d4c3b2a1106170706c79206f6e206465736b2d7063"
#: A board's refusal: held, the hub's lease, board 0x6300, 12345 ms left, token 9, holder "hub".
REPLY_GOLDEN = "01020063393000000900000003687562"


class Wire(unittest.TestCase):
    def test_the_request_is_the_boards_format(self) -> None:
        self.assertEqual(ls.encode_request(ls.KIND_DEPLOY, ls.OP_TAKE, 0xA1B2C3D4, ttl_s=90, name="apply on desk-pc",
                                           target=0x7368).hex(), REQUEST_GOLDEN)

    def test_a_refusal_names_the_holder(self) -> None:
        r = ls.LeaseReply.parse(bytes.fromhex(REPLY_GOLDEN))
        self.assertEqual((r.status, r.kind, r.arbiter, r.remaining_ms, r.token, r.name),
                         (ls.STATUS_HELD, ls.KIND_HUB, 0x6300, 12345, 9, "hub"))
        self.assertIn('"hub"', str(ls.LeaseRefused(ls.KIND_HUB, r)))
        with self.assertRaises(ValueError):
            ls.LeaseReply.parse(bytes.fromhex(REPLY_GOLDEN)[:-1])

    def test_a_holder_name_is_printable_and_short(self) -> None:
        n = ls.holder_name("apply")
        self.assertTrue(n.startswith("apply on "))
        self.assertLessEqual(len(n), ls.NAME_MAX)
        self.assertTrue(all(0x20 <= ord(c) <= 0x7E and c not in '"\\' for c in n))


class FakeArbiter:
    """A bridge whose deploy_step answers LEASE the way a board's table does (pot/lease.hpp), on a fake clock."""

    def __init__(self, answers: bool = True) -> None:
        self.answers = answers
        self.unreachable = False  # the cabled board answers, the arbiter behind it does not
        self.token = 0
        self.name = ""
        self.expires = 0.0
        self.now = 0.0
        self.calls: list[tuple[int, int]] = []
        self.lock = threading.Lock()

    def deploy_step(self, opcode, payload, *, timeout=2.0, dst=None):
        assert opcode == fr.Op.LEASE
        if not self.answers:
            raise TimeoutError("no REPLY")
        target, kind, op, ttl, token, n = struct.unpack_from("<HBBHIB", payload, 0)
        name = payload[11:11 + n].decode()
        if self.unreachable:
            return struct.pack("<BBHIIB", ls.STATUS_UNREACHABLE, kind, 0x6300, 0, 0, 0)
        with self.lock:
            self.calls.append((op, token))
            if self.token and self.now >= self.expires:
                self.token = 0
            mine = self.token == token
            if op == ls.OP_TAKE and (self.token == 0 or mine):
                self.token, self.name, self.expires, status = token, name, self.now + ttl, ls.STATUS_OK
            elif op == ls.OP_RELEASE and (self.token == 0 or mine):
                self.token, status = 0, ls.STATUS_OK
            elif op == ls.OP_QUERY:
                status = ls.STATUS_OK
            else:
                status = ls.STATUS_HELD
            raw = self.name.encode() if self.token else b""
            left = int((self.expires - self.now) * 1000) if self.token else 0
            return struct.pack("<BBHIIB", status, kind, 0x6300, left, self.token, len(raw)) + raw


class Holder(unittest.TestCase):
    def test_a_second_claimant_is_refused_naming_the_first_and_takes_it_after_release(self) -> None:
        arb = FakeArbiter()
        first = ls.CellLease.take(arb, ls.KIND_DEPLOY, "apply on desk-pc", log=lambda m: None)
        self.assertTrue(first.supported)
        with self.assertRaises(ls.LeaseRefused) as ctx:
            ls.CellLease.take(arb, ls.KIND_DEPLOY, "apply on laptop", log=lambda m: None)
        self.assertIn('"apply on desk-pc"', str(ctx.exception))
        self.assertIn("0x6300", str(ctx.exception))
        first.release()
        self.assertEqual(arb.calls[-1][0], ls.OP_RELEASE)
        with ls.CellLease.take(arb, ls.KIND_DEPLOY, "apply on laptop", log=lambda m: None) as second:
            self.assertTrue(second.supported)
        self.assertEqual(arb.token, 0)

    def test_the_holder_renews_and_learns_when_another_took_it(self) -> None:
        arb = FakeArbiter()
        lease = ls.CellLease.take(arb, ls.KIND_HUB, "hub on desk-pc", ttl_s=3, log=lambda m: None)  # renews every 1 s
        try:
            time.sleep(1.2)
            self.assertGreaterEqual(lease.renewals, 1)
            # The arbiter reboots (forgets), another hub takes it there first: the next renewal finds it held.
            with arb.lock:
                arb.token, arb.name, arb.expires = 0xBEEF, "hub on laptop", arb.now + 60
            deadline = time.monotonic() + 3.0
            while lease.lost is None and time.monotonic() < deadline:
                time.sleep(0.02)
            self.assertIsNotNone(lease.lost)
            self.assertEqual(lease.lost.name, "hub on laptop")
        finally:
            lease.release()
        self.assertNotEqual(arb.calls[-1][0], ls.OP_RELEASE)  # a lost lease is not ours to release

    def test_an_arbiter_that_does_not_answer_is_not_old_firmware(self) -> None:
        arb = FakeArbiter()
        arb.unreachable = True
        with self.assertRaises(ls.LeaseUnavailable) as ctx:
            ls.CellLease.take(arb, ls.KIND_DEPLOY, "apply on desk-pc", backoff_s=0.01, log=lambda m: None)
        self.assertIn("0x6300", str(ctx.exception))
        notes: list[str] = []
        lease = ls.CellLease.take(arb, ls.KIND_HUB, "hub on desk-pc", ttl_s=3, strict=False, backoff_s=0.01,
                                  log=notes.append)
        try:
            self.assertTrue(lease.supported)
            self.assertIsNone(lease.reply)
            self.assertIn("does not answer", notes[0])
            arb.unreachable = False  # the arbiter is back: the renewal takes the lease
            deadline = time.monotonic() + 3.0
            while lease.reply is None and time.monotonic() < deadline:
                time.sleep(0.05)
            self.assertIsNotNone(lease.reply)
            self.assertEqual(arb.name, "hub on desk-pc")
        finally:
            lease.release()

    def test_a_cell_before_m14_gives_no_lease_and_says_so(self) -> None:
        notes: list[str] = []
        lease = ls.CellLease.take(FakeArbiter(answers=False), ls.KIND_DEPLOY, "apply", log=notes.append)
        self.assertFalse(lease.supported)
        self.assertIn("firmware before M14", notes[0])
        lease.release()


if __name__ == "__main__":
    unittest.main()
