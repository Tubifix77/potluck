"""M10: pot_hostnode, the PC as a placement node -- the parts that need no board.

The native node is built with the C++ suite (tests/CMakeLists.txt). This checks what a bench run
cannot show cheaply: that it refuses an identity the cluster CA did not certify, that an enrolled one
boots from a compiled package image with the image's portable actor, and that, alone on a silent
link, it settles and claims the actor itself -- the M10 rule that a placement host is a node like any.
Skipped when the binary has not been built.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from potluck import deploy as dp
from potluck import enrol as en
from potluck import signing as sg
from potluck.manifest import parse

HERE = os.path.dirname(__file__)
EXE = os.path.join(HERE, "..", "..", "..", "build", "tests", "pot_hostnode.exe" if os.name == "nt" else "pot_hostnode")
M10 = os.path.join(HERE, "..", "..", "..", "manifests", "m10-ticker.json")


@unittest.skipUnless(os.path.exists(EXE), "pot_hostnode not built (run the C++ suite first)")
class HostNode(unittest.TestCase):
    def setUp(self) -> None:
        self.dir = tempfile.mkdtemp(prefix="pot_hostnode_test_")
        self.ident = os.path.join(self.dir, "host.id")
        pub = subprocess.run([EXE, "--keygen", self.ident], capture_output=True, text=True, check=True).stdout.strip()
        self.pub = bytes.fromhex(pub)
        self.ca = sg.generate("ca", "test-ca")
        with open(M10, encoding="utf-8") as f:
            img = dp.compile_image(parse(json.load(f)), 1)
        self.image = os.path.join(self.dir, "m10.img")
        with open(self.image, "wb") as f:
            f.write(img)

    def enrol(self, node: int = 0x00FE) -> None:
        cert = en.build_cert(self.ca, node, self.pub)
        with open(self.ident, "a", encoding="ascii") as f:
            f.write(f"ca {self.ca.public.hex()}\ncert {cert.hex()}\n")

    def run_node(self, seconds: float) -> tuple[int | None, list[dict], str]:
        p = subprocess.Popen([EXE, "--identity", self.ident, "--image", self.image],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        time.sleep(seconds)
        p.terminate()
        _, err = p.communicate(timeout=5)
        text = err.decode("utf-8", "replace")
        recs = [json.loads(line) for line in text.splitlines() if line.startswith("{")]
        return p.returncode, recs, text

    def test_an_identity_the_ca_did_not_certify_is_refused(self) -> None:
        r = subprocess.run([EXE, "--identity", self.ident, "--image", self.image], capture_output=True, text=True,
                           timeout=10, stdin=subprocess.DEVNULL)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("not enrolled", r.stderr)
        self.enrol(node=0x00FD)  # certified, but for another node id
        r = subprocess.run([EXE, "--identity", self.ident, "--image", self.image], capture_output=True, text=True,
                           timeout=10, stdin=subprocess.DEVNULL)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("certificate refused", r.stderr)

    def test_an_enrolled_host_boots_settles_alone_and_claims_the_portable_actor(self) -> None:
        self.enrol()
        _, recs, text = self.run_node(8.0)  # settle_max_ms is 6 s
        boot = [r for r in recs if r["t"] == "host_boot"]
        self.assertEqual(len(boot), 1, text)
        self.assertEqual(boot[0]["portable"], 1)
        host = [r for r in recs if r["t"] == "host"]
        self.assertTrue(host and host[-1]["settled"] == 1, text)
        rec = [r for r in recs if r["t"] == "rec"]
        self.assertTrue(rec and rec[-1]["running"] == 1 and rec[-1]["owner"] == 0x00FE, text)


if __name__ == "__main__":
    unittest.main()
