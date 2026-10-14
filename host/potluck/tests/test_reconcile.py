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


class HostPlacement(unittest.TestCase):
    """M10: a host that opts in to placement is eligible like any node; one that does not stays out."""

    def with_host(self, placement: bool | None) -> dict:
        d = doc({"name": "ticker", "module": "builtin:ticker", "latency_class": 3,
                 "needs": ["potluck://lab/node-00fe/sys/uptime"], "config": {"period_ms": 100}})
        host = {"node_id": 0x00FE, "label": "pc", "kind": "host",
                "owns": [dict(UPTIME_C, path="potluck://lab/node-00fe/sys/uptime")]}
        if placement is not None:
            host["placement"] = placement
        d["nodes"].append(host)
        return d

    def test_a_placement_host_is_eligible_and_its_gravity_ranks_it_first(self):
        m = parse(self.with_host(True))
        self.assertEqual(rc.eligible(m, m.actors[0])[-1], (0x00FE, 2))
        self.assertEqual(rc.plan(m)[0].order[0], 0x00FE)

    def test_a_services_only_host_stays_out(self):
        for placement in (None, False):
            m = parse(self.with_host(placement))
            self.assertNotIn(0x00FE, [n for n, _ in rc.eligible(m, m.actors[0])])

    def present(self, hub: bool | None, host: bool = True) -> dict:
        cfg = {"broker": "192.168.0.77:1883", "keepalive_s": 10, "period_ms": 2000}
        if hub is not None:
            cfg["hub"] = hub
        d = doc({"name": "present", "module": "builtin:present", "latency_class": 4, "config": cfg})
        if host:
            d["nodes"].append({"node_id": 0x00FE, "label": "pc", "kind": "host", "placement": True})
        return d

    def test_the_hub_presents_only_when_the_cluster_says_so_and_then_first(self):
        # M14: present.hub lists the PC's node for the presenting role, first by gravity; without it the PC never is.
        m = parse(self.present(True))
        self.assertEqual(rc.eligible(m, m.actors[0])[-1], (0x00FE, 255))
        self.assertEqual(rc.plan(m)[0].order[0], 0x00FE)
        for hub in (None, False):
            m = parse(self.present(hub))
            self.assertNotIn(0x00FE, [n for n, _ in rc.eligible(m, m.actors[0])])
            dp.compile_image(m, 1)  # the hub key changes who is eligible, nothing on the wire

    def test_apply_counts_every_portable_actor_and_knows_which_the_pc_may_hold(self):
        # Every board's NODE_STATUS counts each portable actor of the package wherever it runs, so apply's verify does
        # too; what the PC may hold only widens the count running on the boards (M0-LOG session 42: counted the other
        # way, no board ever confirmed a package with such an actor).
        from potluck import apply as ap

        boards = [0x6300, 0x7368, 0x8160]
        for hub, pc in ((True, 1), (False, 0)):
            m = parse(self.present(hub))
            self.assertEqual(ap.expected(m, boards)[1], 1)
            self.assertEqual(ap.pc_runnable(m), pc)

    def test_the_hub_presenting_with_no_placement_host_is_refused(self):
        m = parse(self.present(True, host=False))
        with self.assertRaisesRegex(dp.DeployError, "needs a placement host"):
            dp.compile_image(m, 1)

    def test_placement_is_for_hosts_only(self):
        from potluck.manifest import ManifestErrors
        d = self.with_host(True)
        d["nodes"][0]["placement"] = True
        with self.assertRaises(ManifestErrors):
            parse(d)


if __name__ == "__main__":
    unittest.main()
