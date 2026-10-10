"""M11: the firmware trailer and the FW_* payloads. The golden signature is verified by tests/test_fw.cpp."""

import struct
import unittest

from potluck import fw


class TestFirmwareSignature(unittest.TestCase):
    # The deploy certificate is test_deploy.py's (the same test CA and deploy key); the signature is over
    # FW_DOMAIN + counter 7 + length 1500 + SHA-512 of GOLDEN_IMAGE.
    GOLDEN_SIG = (
        "7f884e0776c11171cd0ae58cf0f13367c2bf2a4bfa871a13afdf231673e67b9a"
        "d73ae11a0bbbbc6bc414f89a7ea11e5e2e56395e52b4365144961bce97751a0a"
    )

    @staticmethod
    def golden_image() -> bytes:
        return bytes(i * 7 & 0xFF for i in range(1500))

    def _keys(self):
        from potluck import ed25519_ref as ed
        from potluck import enrol as en
        from potluck.signing import KeyPair

        ca = KeyPair("ed25519", "ca", "test-ca", ed.public_key(bytes(range(32))), bytes(range(32)))
        dsk = bytes(range(64, 96))
        bcert = en.build_cert(ca, 0, ed.public_key(dsk), issued=1790000000, role=en.ROLE_DEPLOY)
        return ca, dsk, bcert

    def test_the_golden_trailer_is_what_the_signer_makes(self):
        _, dsk, bcert = self._keys()
        t = fw.fw_trailer(self.golden_image(), 7, dsk, bcert)
        self.assertEqual(len(t), fw.TRAILER_LEN)
        self.assertEqual(t[:112], bcert)
        self.assertEqual(t[112:].hex(), self.GOLDEN_SIG)

    def test_the_counter_and_the_length_are_signed(self):
        from potluck import ed25519_ref as ed
        import hashlib

        _, dsk, bcert = self._keys()
        img = self.golden_image()
        sig = fw.fw_trailer(img, 7, dsk, bcert)[112:]
        d = hashlib.sha512(img).digest()
        pub = ed.public_key(dsk)
        self.assertTrue(ed.verify(pub, fw.signed_message(7, len(img), d), sig))
        self.assertFalse(ed.verify(pub, fw.signed_message(8, len(img), d), sig))
        self.assertFalse(ed.verify(pub, fw.signed_message(7, len(img) + 1, d), sig))

    def test_a_short_certificate_is_refused(self):
        _, dsk, bcert = self._keys()
        with self.assertRaises(fw.FwError):
            fw.fw_trailer(b"x", 1, dsk, bcert[:-1])


class TestPayloads(unittest.TestCase):
    def test_begin_and_chunk_layouts(self):
        p = fw.begin_payload(0x7368, 1500, 7, bytes(176))
        self.assertEqual(len(p), 186)
        self.assertEqual(struct.unpack_from("<HII", p), (0x7368, 1500, 7))
        c = fw.chunk_payload(0x8160, 400, b"abc")
        self.assertEqual(c, struct.pack("<HIH", 0x8160, 400, 3) + b"abc")
        self.assertEqual(fw.target_payload(0x6300), b"\x00\x63")

    def test_a_reply_reads_back(self):
        raw = struct.pack("<HHIIIB", 2, 0x7368, 10, 5, 6, 1) + b"m11-test" + bytes(24)
        r = fw.parse_reply(raw)
        self.assertEqual(r, {"status": "downgrade", "node": 0x7368, "received": 10, "running": 5, "floor": 6,
                             "state": "on_trial", "version": "m11-test"})
        with self.assertRaises(fw.FwError):
            fw.parse_reply(raw[:-1])


if __name__ == "__main__":
    unittest.main()
