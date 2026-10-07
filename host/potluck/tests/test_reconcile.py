"""M6, section 7.7: the host's half of the reconciler -- portability, eligibility, gravity, ranking.

The golden numbers are also asserted by tests/test_reconcile.cpp, so the Python twin of the node's
weight function and ticker encoding cannot drift from the firmware without one suite failing.
"""

from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from potluck import deploy as dp
from potluck import reconcile as rc
from potluck.manifest import parse
from potluck.paths import path_hash

HERE = os.path.dirname(__file__)
M6 = os.path.join(HERE, "..", "..", "..", "manifests", "m6-ticker.json")


def doc(actor: dict, owns_c: list[dict] | None = None) -> dict:
    return {
        "schema": 1,
        "system": "lab",
        "min_core_version": 1,
        "nodes": [
            {"node_id": 0x6300, "label": "A"},
            {"node_id": 0x7368, "label": "B"},
            {"node_id": 0x8160, "label": "C", "owns": owns_c or []},
        ],
        "actors": [actor],
    }


UPTIME_C = {"path": "potluck://lab/node-8160/sys/uptime", "unit": "millisecond", "latency_class": 4,
            "staleness_bound_ms": 5000}


class Golden(unittest.TestCase):
    def test_weights_match_the_node(self):
        key = path_hash("potluck://lab/act/ticker/out")
        self.assertEqual(key, 1245650351)
        self.assertEqual(rc.hrw_weight(key, 0x6300), 411316836)
        self.assertEqual(rc.hrw_weight(key, 0x7368), 117789623)
        self.assertEqual(rc.hrw_weight(key, 0x8160), 48773440)

    def test_the_bench_manifest_compiles_to_the_bytes_the_node_test_parses(self):
        from potluck.manifest import load

        m = load(M6, require_placement=True)
        img = dp.compile_image(m, 5)
        self.assertEqual(img[24:].hex(), "feff0310" "af1d3f4a640003006301687301608102")


class Portability(unittest.TestCase):
    def actor(self, **kw) -> dict:
        a = {"name": "ticker", "module": "builtin:ticker", "latency_class": 3, "config": {"period_ms": 100}}
        a.update(kw)
        return a

    def test_derived_not_declared(self):
        m = parse(doc(self.actor(needs=[UPTIME_C["path"]]), [UPTIME_C]))
        self.assertEqual(rc.portability(m, m.actors[0]), (True, "portable"))

    def test_a_tight_binding_pins_it(self):
        tight = dict(UPTIME_C, latency_class=1)
        m = parse(doc(self.actor(needs=[tight["path"]]), [tight]))
        ok, why = rc.portability(m, m.actors[0])
        self.assertFalse(ok)
        self.assertIn("pinned by physics", why)

    def test_an_actuator_owner_is_never_portable(self):
        pwm = {"path": "potluck://lab/node-8160/pwm/0", "unit": "ratio", "access": "read_write",
               "latency_class": 3, "staleness_bound_ms": 1000}
        m = parse(doc(self.actor(needs=[pwm["path"]], on_host_loss="hold"), [pwm]))
        ok, why = rc.portability(m, m.actors[0])
        self.assertFalse(ok)
        self.assertIn("ADR-006", why)
        with self.assertRaises(dp.DeployError):
            dp.compile_image(m, 1)

    def test_gravity_puts_it_where_its_data_is(self):
        m = parse(doc(self.actor(needs=[UPTIME_C["path"]]), [UPTIME_C]))
        (p,) = rc.plan(m)
        self.assertEqual(p.order[0], 0x8160)
        self.assertEqual(dict(p.nodes)[0x8160], 2)
        # C dies: the next in line is what every node computes too.
        self.assertEqual(rc.rank(p.key, list(p.nodes), live={0x6300, 0x7368})[0], p.order[1])

    def test_a_pinned_ticker_has_one_eligible_node(self):
        m = parse(doc(self.actor(pin=0x7368)))
        self.assertEqual(rc.eligible(m, m.actors[0]), [(0x7368, 1)])
        img = dp.compile_image(m, 1)
        self.assertEqual(img[24:28], bytes([0xFE, 0xFF, 3, 7 + 3]))


if __name__ == "__main__":
    unittest.main()
