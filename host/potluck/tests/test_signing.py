"""Signed deploy packages -- ARCHITECTURE.md section 9.3, and the host half of M5.

Two things are being tested and they deserve separating.

**The algorithm.** `ed25519_ref.py` is a from-the-spec implementation, so it is checked against the
specification's own published test vectors -- all three of RFC 8032 section 7.1, taken verbatim from
https://www.rfc-editor.org/rfc/rfc8032.txt (looked up 2026-08-23). Same discipline as the frame
codec's golden bytes: an implementation checked only against itself is an implementation that agrees
with its own mistakes. These vectors are also what would let a vetted library replace this file
without anybody having to trust the swap.

**The format.** A signature says *who wrote this*, and nothing else. So the tests below insist that a
signed manifest is still fully re-validated, that the rollback counter is inside the signed bytes
rather than beside them, and that a certificate cannot be replayed as a package signature.
"""

from __future__ import annotations

import copy
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from potluck import ed25519_ref as ed
from potluck.manifest import load as load_manifest
from potluck.signing import (
    CERT_TYPE,
    PACKAGE_TYPE,
    KeyPair,
    SigningError,
    certify,
    generate,
    key_id,
    package_preimage,
    read_key,
    sign_manifest,
    verify_cert,
    verify_package,
    write_keypair,
)

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
EXAMPLE = os.path.join(REPO, "manifests", "home.json")


def hexb(s: str) -> bytes:
    return bytes.fromhex(s)


#: RFC 8032 section 7.1, tests 1-3, verbatim.
RFC8032_VECTORS = [
    (
        "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
        "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
        "",
        "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
        "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
    ),
    (
        "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
        "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
        "72",
        "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
        "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00",
    ),
    (
        "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
        "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
        "af82",
        "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
        "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a",
    ),
]


class Rfc8032(unittest.TestCase):
    def test_every_published_vector_matches(self) -> None:
        for i, (sk, pk, msg, sig) in enumerate(RFC8032_VECTORS, start=1):
            with self.subTest(vector=i):
                self.assertEqual(ed.public_key(hexb(sk)).hex(), pk)
                self.assertEqual(ed.sign(hexb(sk), hexb(msg)).hex(), sig)
                self.assertTrue(ed.verify(hexb(pk), hexb(msg), hexb(sig)))

    def test_a_changed_message_does_not_verify(self) -> None:
        _, pk, _, sig = RFC8032_VECTORS[2]
        self.assertTrue(ed.verify(hexb(pk), hexb("af82"), hexb(sig)))
        self.assertFalse(ed.verify(hexb(pk), hexb("af83"), hexb(sig)))

    def test_a_changed_signature_does_not_verify(self) -> None:
        _, pk, msg, sig = RFC8032_VECTORS[2]
        raw = bytearray(hexb(sig))
        raw[0] ^= 0x01
        self.assertFalse(ed.verify(hexb(pk), hexb(msg), bytes(raw)))

    def test_another_key_does_not_verify(self) -> None:
        _, _, msg, sig = RFC8032_VECTORS[2]
        other = RFC8032_VECTORS[1][1]
        self.assertFalse(ed.verify(hexb(other), hexb(msg), hexb(sig)))

    def test_a_scalar_at_or_above_the_group_order_is_refused(self) -> None:
        # The same signature re-encoded with s + L is arithmetically equivalent and must still be
        # refused, so "the signature bytes" are a stable identity rather than one of many.
        _, pk, msg, sig = RFC8032_VECTORS[2]
        raw = hexb(sig)
        s = int.from_bytes(raw[32:], "little")
        malleable = raw[:32] + int.to_bytes(s + ed.L, 32, "little")
        self.assertFalse(ed.verify(hexb(pk), hexb(msg), malleable))

    def test_wrong_lengths_are_refused_rather_than_raising(self) -> None:
        # A verifier has one safe answer for malformed input and it is False.
        self.assertFalse(ed.verify(b"", b"x", b""))
        self.assertFalse(ed.verify(b"\x00" * 32, b"x", b"\x00" * 63))


class Keys(unittest.TestCase):
    def setUp(self) -> None:
        self.dir = tempfile.mkdtemp(prefix="pot-keys-")

    def test_a_generated_pair_is_a_pair(self) -> None:
        kp = generate("deploy", "test")
        assert kp.secret is not None
        self.assertEqual(ed.public_key(kp.secret), kp.public)
        self.assertEqual(kp.id, key_id(kp.alg, kp.public))

    def test_the_key_id_covers_the_algorithm_as_well_as_the_bytes(self) -> None:
        # So the day a second algorithm exists, the same 32 bytes under it is a different key.
        pub = b"\x01" * 32
        self.assertNotEqual(key_id("ed25519", pub), key_id("p256", pub))

    def test_a_written_pair_round_trips_and_the_public_file_holds_no_secret(self) -> None:
        kp = generate("ca", "lab-ca")
        priv, pub = write_keypair(kp, os.path.join(self.dir, "ca"))
        again = read_key(priv)
        self.assertEqual(again.public, kp.public)
        self.assertEqual(again.secret, kp.secret)

        public_only = read_key(pub)
        self.assertIsNone(public_only.secret)
        with open(pub, encoding="ascii") as f:
            self.assertNotIn("secret", f.read())

    def test_an_edited_key_id_is_refused(self) -> None:
        # Every trust decision downstream is made on the id, so it may not disagree with the bytes.
        kp = generate("deploy", "test")
        priv, _ = write_keypair(kp, os.path.join(self.dir, "d"))
        with open(priv, encoding="ascii") as f:
            doc = json.load(f)
        doc["key_id"] = "0000000000000000"
        with open(priv, "w", encoding="ascii") as f:
            json.dump(doc, f)
        with self.assertRaises(SigningError) as cm:
            read_key(priv)
        self.assertIn("key_id", str(cm.exception))

    def test_a_mismatched_secret_and_public_half_is_refused(self) -> None:
        a = generate("deploy", "a")
        b = generate("deploy", "b")
        assert b.secret is not None
        frankenstein = KeyPair(alg=a.alg, role=a.role, label=a.label, public=a.public,
                               secret=b.secret)
        priv, _ = write_keypair(frankenstein, os.path.join(self.dir, "f"))
        with self.assertRaises(SigningError) as cm:
            read_key(priv)
        self.assertIn("not a pair", str(cm.exception))


class Certificates(unittest.TestCase):
    def setUp(self) -> None:
        self.ca = generate("ca", "lab-ca")
        self.deploy = generate("deploy", "lab-deploy")
        self.cert = certify(self.ca, self.deploy)

    def test_a_certificate_authorises_its_subject(self) -> None:
        subject = verify_cert(self.cert, self.ca.public)
        self.assertEqual(subject.public, self.deploy.public)
        self.assertEqual(subject.id, self.deploy.id)

    def test_another_ca_does_not_authorise_it(self) -> None:
        other = generate("ca", "someone-else")
        with self.assertRaises(SigningError) as cm:
            verify_cert(self.cert, other.public)
        self.assertIn("not signed by this CA", str(cm.exception))

    def test_a_deploy_key_cannot_certify(self) -> None:
        # Section 9.3 separates the CA from the deploy key so the two compromises are recoverable
        # independently. A deploy key that could mint certificates would erase the separation.
        with self.assertRaises(SigningError):
            certify(self.deploy, self.deploy)

    def test_a_ca_cannot_certify_another_ca_as_a_deploy_key(self) -> None:
        second_ca = generate("ca", "second")
        with self.assertRaises(SigningError):
            certify(self.ca, second_ca)

    def test_a_public_only_ca_key_cannot_sign(self) -> None:
        pub_only = KeyPair(alg=self.ca.alg, role="ca", label="pub", public=self.ca.public,
                           secret=None)
        with self.assertRaises(SigningError):
            certify(pub_only, self.deploy)

    def test_an_expired_certificate_is_refused(self) -> None:
        cert = certify(self.ca, self.deploy, not_after=1000)
        verify_cert(cert, self.ca.public, now=999)  # still valid
        with self.assertRaises(SigningError) as cm:
            verify_cert(cert, self.ca.public, now=1001)
        self.assertIn("expired", str(cm.exception))

    def test_a_tampered_subject_key_is_refused(self) -> None:
        attacker = generate("deploy", "lab-deploy")
        forged = copy.deepcopy(self.cert)
        forged["subject"]["public"] = attacker.public.hex()
        forged["subject"]["key_id"] = attacker.id
        with self.assertRaises(SigningError):
            verify_cert(forged, self.ca.public)

    def test_a_tampered_expiry_is_refused(self) -> None:
        # The expiry is inside the signed preimage, so extending it invalidates the certificate.
        forged = copy.deepcopy(certify(self.ca, self.deploy, not_after=1000))
        forged["not_after"] = 1 << 40
        with self.assertRaises(SigningError):
            verify_cert(forged, self.ca.public)


class Packages(unittest.TestCase):
    def setUp(self) -> None:
        self.ca = generate("ca", "lab-ca")
        self.deploy = generate("deploy", "lab-deploy")
        self.cert = certify(self.ca, self.deploy)
        self.manifest = load_manifest(EXAMPLE, require_placement=True)
        self.pkg = sign_manifest(self.manifest, self.deploy, self.cert, 7)

    def test_a_signed_package_verifies(self) -> None:
        v = verify_package(self.pkg, self.ca.public, min_counter=7)
        self.assertEqual(v.manifest.digest(), self.manifest.digest())
        self.assertEqual(v.rollback_counter, 7)
        self.assertEqual(v.signer.id, self.deploy.id)

    def test_the_package_carries_no_secret(self) -> None:
        text = json.dumps(self.pkg)
        assert self.deploy.secret is not None
        self.assertNotIn(self.deploy.secret.hex(), text)
        assert self.ca.secret is not None
        self.assertNotIn(self.ca.secret.hex(), text)

    def test_changing_one_policy_in_the_manifest_breaks_the_signature(self) -> None:
        forged = copy.deepcopy(self.pkg)
        # From informative to strict, which is a real change and a legal one -- so the only thing
        # standing between an edited policy and a deployed system is the signature.
        self.assertEqual(forged["manifest"]["nodes"][0]["owns"][0]["staleness_policy"],
                         "informative")
        forged["manifest"]["nodes"][0]["owns"][0]["staleness_policy"] = "strict"
        with self.assertRaises(SigningError) as cm:
            verify_package(forged, self.ca.public)
        self.assertIn("does not check out", str(cm.exception))

    def test_changing_the_rollback_counter_breaks_the_signature(self) -> None:
        # The point of putting the counter inside the preimage: a downgrade is not a valid signature
        # with a suspicious number beside it, it is an invalid signature.
        forged = copy.deepcopy(self.pkg)
        forged["rollback_counter"] = 1
        with self.assertRaises(SigningError) as cm:
            verify_package(forged, self.ca.public)
        self.assertIn("does not check out", str(cm.exception))

    def test_a_correctly_signed_downgrade_is_still_refused(self) -> None:
        # Signed properly at counter 3, offered to something that has already accepted 7. This is
        # the whole attack the counter exists for, so it is refused before the signature is even
        # checked.
        old = sign_manifest(self.manifest, self.deploy, self.cert, 3)
        verify_package(old, self.ca.public, min_counter=3)
        with self.assertRaises(SigningError) as cm:
            verify_package(old, self.ca.public, min_counter=7)
        self.assertIn("rollback", str(cm.exception))

    def test_a_key_the_ca_never_certified_is_refused(self) -> None:
        rogue = generate("deploy", "rogue")
        rogue_ca = generate("ca", "rogue-ca")
        rogue_cert = certify(rogue_ca, rogue)
        pkg = sign_manifest(self.manifest, rogue, rogue_cert, 9)
        with self.assertRaises(SigningError) as cm:
            verify_package(pkg, self.ca.public)
        self.assertIn("not signed by this CA", str(cm.exception))

    def test_a_signature_whose_key_id_disagrees_with_the_certificate_is_refused(self) -> None:
        forged = copy.deepcopy(self.pkg)
        forged["signature"]["key_id"] = "ffffffffffffffff"
        with self.assertRaises(SigningError) as cm:
            verify_package(forged, self.ca.public)
        self.assertIn("authorises", str(cm.exception))

    def test_a_certificate_cannot_be_signed_by_a_key_it_does_not_authorise(self) -> None:
        other = generate("deploy", "other")
        with self.assertRaises(SigningError) as cm:
            sign_manifest(self.manifest, other, self.cert, 1)
        self.assertIn("does not authorise this key", str(cm.exception))

    def test_an_invalid_manifest_inside_a_valid_signature_is_still_refused(self) -> None:
        # A signature says who wrote it, never that it is well-formed. So the manifest is re-parsed
        # in full rather than trusted for being signed.
        broken = copy.deepcopy(self.manifest.to_dict())
        broken["actors"][0]["priority"] = "urgent"
        doc = copy.deepcopy(self.pkg)
        doc["manifest"] = broken
        # The signature is irrelevant here on purpose: the manifest is re-parsed before the
        # signature is ever checked, so an invalid one is refused whoever signed it.
        doc["signature"]["sig"] = ed.sign(
            self.deploy.secret,  # type: ignore[arg-type]
            b"whatever-this-is-not-what-is-checked",
        ).hex()
        with self.assertRaises(SigningError) as cm:
            verify_package(doc, self.ca.public)
        self.assertIn("not valid", str(cm.exception))

    def test_a_certificate_signature_cannot_be_replayed_as_a_package_signature(self) -> None:
        # Domain separation, checked rather than asserted: the two preimages start with different
        # type strings, so bytes signed as one can never verify as the other.
        forged = copy.deepcopy(self.pkg)
        forged["signature"]["sig"] = self.cert["signature"]
        with self.assertRaises(SigningError):
            verify_package(forged, self.ca.public)
        self.assertTrue(package_preimage(self.manifest, 7).startswith(PACKAGE_TYPE.encode()))
        self.assertNotIn(CERT_TYPE.encode(), package_preimage(self.manifest, 7)[:64])

    def test_a_negative_counter_is_refused_at_both_ends(self) -> None:
        with self.assertRaises(SigningError):
            sign_manifest(self.manifest, self.deploy, self.cert, -1)
        forged = copy.deepcopy(self.pkg)
        forged["rollback_counter"] = -1
        with self.assertRaises(SigningError):
            verify_package(forged, self.ca.public)

    def test_a_boolean_counter_is_not_an_integer(self) -> None:
        # bool is an int in Python; `true` would otherwise pass as 1.
        forged = copy.deepcopy(self.pkg)
        forged["rollback_counter"] = True
        with self.assertRaises(SigningError):
            verify_package(forged, self.ca.public)

    def test_something_that_is_not_a_package_is_refused(self) -> None:
        for junk in ({}, {"type": "something-else"}, [], "text", None):
            with self.assertRaises(SigningError):
                verify_package(junk, self.ca.public)


if __name__ == "__main__":
    unittest.main()
