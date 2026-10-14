"""ARCHITECTURE.md section 15, question 1: does the Locality Contract survive real application code, or does everything
end up L4? Three applications the question names -- a vent controller, a joint controller, a building monitor --
written as their authors would declare them (manifests/q1-*.json), checked by the contract, their classes counted,
and two mistakes an author would plausibly make refused with the reason.

    python test_q1_apps.py            # the checks
    python test_q1_apps.py --census   # the class distribution, per application and in all
"""

from __future__ import annotations

import collections
import copy
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from potluck import locality  # noqa: E402
from potluck.manifest import load  # noqa: E402

MANIFESTS = os.path.join(os.path.dirname(__file__), "..", "..", "..", "manifests")
APPS = ("vent", "joint", "building")


def manifest_path(app: str) -> str:
    return os.path.join(MANIFESTS, f"q1-{app}.json")


def census(m) -> dict:
    """Classes of the resources, the actors and the bindings (each binding counted at its resource's class)."""
    res = collections.Counter(r.latency_class for n in m.nodes for r in n.owns)
    act = collections.Counter(a.latency_class for a in m.actors)
    bind = collections.Counter(m.resource(p).latency_class for a in m.actors for p in a.needs if m.resource(p))
    return {"resources": res, "actors": act, "bindings": bind}


def load_doc(app: str) -> dict:
    import json
    with open(manifest_path(app), encoding="utf-8") as f:
        return json.load(f)


def check_doc(doc: dict, tmpdir: str) -> list:
    import json
    p = os.path.join(tmpdir, "m.json")
    with open(p, "w", encoding="utf-8") as f:
        json.dump(doc, f)
    return locality.check(load(p))


class Q1(unittest.TestCase):
    def test_all_three_satisfy_the_contract(self) -> None:
        for app in APPS:
            with self.subTest(app=app):
                self.assertEqual(locality.check(load(manifest_path(app))), [])

    def test_the_classes_carry_weight_not_everything_is_l4(self) -> None:
        total = collections.Counter()
        for app in APPS:
            total.update(census(load(manifest_path(app)))["resources"])
        self.assertGreater(sum(v for k, v in total.items() if k <= 2), 0)  # tight classes are used
        self.assertGreater(len(total), 3)                                   # and most classes, not one

    def test_a_servo_loop_moved_off_its_joint_is_refused_naming_both_ends(self) -> None:
        import tempfile
        doc = load_doc("joint")
        bad = copy.deepcopy(doc)
        for a in bad["actors"]:
            if a["name"] == "shoulder_servo":
                a["pin"] = 0x2203
        bad["placement"]["shoulder_servo"] = 0x2203
        with tempfile.TemporaryDirectory() as d:
            errs = check_doc(bad, d)
        self.assertTrue(errs)
        text = " ".join(str(e) for e in errs)
        self.assertIn("shoulder", text)
        self.assertIn("coordinator", text)

    def test_the_trajectory_moved_to_the_pc_is_refused_by_the_transport(self) -> None:
        import tempfile
        bad = load_doc("joint")
        for a in bad["actors"]:
            if a["name"] == "trajectory":
                a["pin"] = 0x00FE
        bad["placement"]["trajectory"] = 0x00FE
        with tempfile.TemporaryDirectory() as d:
            errs = check_doc(bad, d)
        self.assertTrue(errs)
        self.assertIn("L2", " ".join(str(e) for e in errs))


def print_census() -> None:
    total = {"resources": collections.Counter(), "actors": collections.Counter(), "bindings": collections.Counter()}
    for app in APPS:
        c = census(load(manifest_path(app)))
        for k in total:
            total[k].update(c[k])
        print(app, {k: dict(sorted(v.items())) for k, v in c.items()})
    print("all", {k: dict(sorted(v.items())) for k, v in total.items()})


if __name__ == "__main__":
    if "--census" in sys.argv:
        print_census()
    else:
        unittest.main()
