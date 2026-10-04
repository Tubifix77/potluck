"""Section 9.3 enrolment, host half: the node certificate, and the console conversation with a board.

The golden certificate below is checked here AND verified by tests/test_trust.cpp, so the host's
certificate builder and the node's checker cannot drift apart without one suite failing.
"""

from __future__ import annotations

import json
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from potluck import ed25519_ref as ed
from potluck import enrol as en
from potluck.signing import KeyPair

CA_SK = bytes(range(32))
NODE_SK = bytes(range(32, 64))
GOLDEN_CA_PUB = "03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8"
GOLDEN_NODE_PUB = "29acbae141bccaf0b22e1a94d34d0bc7361e526d0bfe12c89794bc9322966dd7"
GOLDEN_CERT = (
    "504e433101016873ed4242ea803bb16a29acbae141bccaf0b22e1a94d34d0bc7361e526d0bfe12c89794bc932296"
    "6dd75fc3b9d6983034e6c4a8fd64108939a15bffb2c2570cfdd4aae75d3cbc8cf41272e9d91bde77a3f3595a3c2b"
    "34fe30aac46ab9f37bf6569e71a1b8f7cb466e0c"
)


def ca() -> KeyPair:
    return KeyPair("ed25519", "ca", "test-ca", ed.public_key(CA_SK), CA_SK)


class TestCert(unittest.TestCase):
    def test_the_golden_certificate_is_what_the_builder_makes(self):
        self.assertEqual(ca().public.hex(), GOLDEN_CA_PUB)
        self.assertEqual(ed.public_key(NODE_SK).hex(), GOLDEN_NODE_PUB)
        cert = en.build_cert(ca(), 0x7368, ed.public_key(NODE_SK), issued=1790000000)
        self.assertEqual(cert.hex(), GOLDEN_CERT)
        self.assertEqual(len(cert), en.CERT_LEN)

    def test_it_verifies_and_says_what_it_binds(self):
        c = en.verify_cert(bytes.fromhex(GOLDEN_CERT), ca().public)
        self.assertEqual(c.node_id, 0x7368)
        self.assertEqual(c.node_pub.hex(), GOLDEN_NODE_PUB)
        self.assertEqual(c.issued, 1790000000)

    def test_every_byte_is_covered_by_the_signature_or_the_signature_itself(self):
        raw = bytearray.fromhex(GOLDEN_CERT)
        for i in range(len(raw)):
            bad = bytearray(raw)
            bad[i] ^= 0x01
            with self.assertRaises(en.EnrolError, msg=f"byte {i} flipped and still accepted"):
                en.verify_cert(bytes(bad), ca().public)

    def test_another_ca_is_refused(self):
        other = ed.public_key(bytes(32))
        with self.assertRaises(en.EnrolError):
            en.verify_cert(bytes.fromhex(GOLDEN_CERT), other)

    def test_a_signature_from_another_context_does_not_pass_as_a_certificate(self):
        # The same CA signing the same 48 bytes WITHOUT the domain prefix must not verify.
        raw = bytes.fromhex(GOLDEN_CERT)
        forged = raw[:48] + ed.sign(CA_SK, raw[:48])
        with self.assertRaises(en.EnrolError):
            en.verify_cert(forged, ca().public)

    def test_only_a_ca_secret_can_build_one(self):
        pub_only = KeyPair("ed25519", "ca", "x", ca().public, None)
        with self.assertRaises(en.EnrolError):
            en.build_cert(pub_only, 1, bytes(32))
        deploy = KeyPair("ed25519", "deploy", "x", ca().public, CA_SK)
        with self.assertRaises(en.EnrolError):
            en.build_cert(deploy, 1, bytes(32))


class FakeBoard:
    """The firmware's side of the console conversation, for testing the host's side."""

    def __init__(self, node_id: int, refuse: str | None = None):
        self.node_id = node_id
        self.pub = ed.public_key(NODE_SK)
        self.refuse = refuse
        self.enrolled = False
        self.ca_fp = ""
        self.out = b""
        self.lines: list[str] = []

    def reset_input_buffer(self):
        self.out = b""

    def write(self, data: bytes):
        line = data.decode().strip()
        self.lines.append(line)
        parts = line.split(" ")
        if parts[1] == "id":
            reply = {"t": "enrol", "node": self.node_id, "pub": self.pub.hex(),
                     "enrolled": int(self.enrolled), "ca_fp": self.ca_fp}
        else:
            ca_pub, cert = bytes.fromhex(parts[2]), bytes.fromhex(parts[3])
            try:
                c = en.verify_cert(cert, ca_pub)
                ok = c.node_id == self.node_id and c.node_pub == self.pub and self.refuse is None
            except en.EnrolError:
                ok = False
            if ok:
                self.enrolled, self.ca_fp = True, en.ca_fingerprint(ca_pub).hex()
            reply = {"t": "enrol", "result": "ok" if ok else (self.refuse or "bad_cert")}
        self.out += b"I (123) pot.m0: some log line\n" + json.dumps(reply, separators=(",", ":")).encode() + b"\n"

    def read(self, n: int) -> bytes:
        chunk, self.out = self.out[:n], self.out[n:]
        return chunk

    def close(self):
        pass


class TestConversation(unittest.TestCase):
    def setUp(self):
        self._open = en._open

    def tearDown(self):
        en._open = self._open

    def test_enrol_asks_signs_installs_and_confirms(self):
        board = FakeBoard(0x8160)
        en._open = lambda port: board
        info, cert = en.enrol("COMX", ca(), issued=1)
        self.assertTrue(info["enrolled"])
        self.assertEqual(en.verify_cert(cert, ca().public).node_id, 0x8160)
        self.assertEqual([l.split(" ")[1] for l in board.lines], ["id", "cert", "id"])

    def test_a_refusal_is_reported_not_swallowed(self):
        board = FakeBoard(0x8160, refuse="wrong_node")
        en._open = lambda port: board
        with self.assertRaises(en.EnrolError) as cm:
            en.enrol("COMX", ca(), issued=1)
        self.assertIn("wrong_node", str(cm.exception))


if __name__ == "__main__":
    unittest.main()
