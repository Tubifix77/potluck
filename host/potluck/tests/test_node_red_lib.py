"""M18: the Node-RED integration (integrations/node-red) -- its client library's tests, run with plain node.

The tests (integrations/node-red/test/client.test.js) start tools/hub_mock.py with this Python and run the client,
the stream parser, the reconnect and fallback, the write and the state report against it, and against a small
server of their own for what the mock does not serve. Skipped cleanly when node is not installed.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
PACKAGE = os.path.join(ROOT, "integrations", "node-red")
NODE = shutil.which("node")


@unittest.skipUnless(NODE, "node is not installed")
class NodeRedLib(unittest.TestCase):
    def test_client_tests_pass(self):
        env = dict(os.environ, POTLUCK_PYTHON=sys.executable)
        proc = subprocess.run(
            [NODE, "--test", os.path.join("test", "client.test.js")],
            cwd=PACKAGE, env=env, capture_output=True, text=True, timeout=240,
        )
        out = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, out[-4000:])
        self.assertIn("against the hub mock", out)
        self.assertNotIn("the hub mock did not start", out)

    def test_package_declares_its_nodes(self):
        import json
        with open(os.path.join(PACKAGE, "package.json"), encoding="utf-8") as f:
            pkg = json.load(f)
        self.assertEqual(pkg["name"], "node-red-contrib-potluck")
        self.assertEqual(pkg.get("dependencies", {}), {})
        for name, path in pkg["node-red"]["nodes"].items():
            self.assertTrue(os.path.isfile(os.path.join(PACKAGE, path)), path)
            self.assertTrue(os.path.isfile(os.path.join(PACKAGE, path[:-3] + ".html")), name)


if __name__ == "__main__":
    unittest.main()
