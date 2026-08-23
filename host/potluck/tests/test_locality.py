"""The Locality Contract check -- ARCHITECTURE.md section 4, and section 13-M4's first sentence.

Section 13-M4 accepts on this, in full:

    "a manifest binding an L1 actor to a remote resource fails the build with a readable error
     naming both ends."

Both halves are tested here: that it fails, and that the error names both ends. The second half is
not decoration. Section 4 exists because a system that hides the seams passes every bench test and
fails in a wall; an error that says "locality violation in actor 3" hides the seam all over again.

The negative fixture is a whole broken manifest in the repository rather than a dict built here, so
the check is exercised the way a build would exercise it -- on a file, through the command line, with
an exit code.
"""

from __future__ import annotations

import contextlib
import copy
import io
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from potluck.locality import (
    SAME_NODE_CLASS,
    TRANSPORT_CLASS,
    binding_ceiling,
    check,
    describe_class,
)
from potluck.locality import main as locality_main
from potluck.manifest import parse

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
GOOD = os.path.join(REPO, "manifests", "home.json")
BROKEN = os.path.join(REPO, "manifests", "broken-locality.json")


def two_nodes(transport: str | None = "espnow") -> dict:
    """Two nodes, one resource on each, one actor -- the smallest thing a class can be wrong about."""
    doc = {
        "schema": 1,
        "system": "test",
        "min_core_version": 1,
        "nodes": [
            {
                "node_id": 0x1001,
                "label": "near",
                "power": "mains",
                "headroom_bytes": 40960,
                "owns": [
                    {
                        "path": "potluck://lab/node-1001/adc/0",
                        "unit": "volt",
                        "latency_class": 1,
                        "staleness_bound_ms": 100,
                    }
                ],
            },
            {
                "node_id": 0x1002,
                "label": "far",
                "power": "mains",
                "headroom_bytes": 40960,
                "owns": [
                    {
                        "path": "potluck://lab/node-1002/adc/0",
                        "unit": "volt",
                        "latency_class": 1,
                        "staleness_bound_ms": 100,
                    }
                ],
            },
        ],
        "actors": [
            {
                "name": "loop",
                "module": "loop.so",
                "latency_class": 1,
                "needs": ["potluck://lab/node-1001/adc/0"],
                "headroom_bytes": 1024,
                "priority": "critical",
            }
        ],
        "bindings": {},
        "links": [],
        "placement": {"loop": 0x1001},
    }
    if transport is not None:
        doc["links"] = [{"a": 0x1001, "b": 0x1002, "transport": transport}]
    return doc


class TheAcceptanceSentence(unittest.TestCase):
    def test_an_l1_actor_bound_to_a_remote_resource_is_rejected(self) -> None:
        doc = two_nodes()
        doc["actors"][0]["needs"] = ["potluck://lab/node-1002/adc/0"]  # the resource is over there
        errors = check(parse(doc))
        self.assertEqual(len(errors), 1)

    def test_the_error_names_both_ends(self) -> None:
        doc = two_nodes()
        doc["actors"][0]["needs"] = ["potluck://lab/node-1002/adc/0"]
        errors = check(parse(doc))
        text = str(errors[0])
        # The actor and the resource...
        self.assertIn("loop", text)
        self.assertIn("potluck://lab/node-1002/adc/0", text)
        # ...and both nodes, by label and by id, because "0x1002" alone is not a place a person knows.
        self.assertIn("near", text)
        self.assertIn("far", text)
        self.assertIn("0x1001", text)
        self.assertIn("0x1002", text)
        # ...and the class, since that is the rule being broken.
        self.assertIn("L1", text)

    def test_the_same_binding_on_its_own_node_is_fine(self) -> None:
        self.assertEqual(check(parse(two_nodes())), [])

    def test_l0_is_held_to_the_same_rule_as_l1(self) -> None:
        # L0 is "same silicon", which is at least as strict as "same node".
        doc = two_nodes()
        doc["actors"][0]["latency_class"] = 0
        doc["nodes"][1]["owns"][0]["latency_class"] = 0
        doc["actors"][0]["needs"] = ["potluck://lab/node-1002/adc/0"]
        self.assertEqual(len(check(parse(doc))), 1)


class RuleOne(unittest.TestCase):
    def test_an_actor_may_not_bind_a_looser_resource(self) -> None:
        # Same node, so nothing crosses; the resource is simply declared looser than the loop that
        # reads it, and a tight loop reading a loose resource is a deadline nobody wrote down.
        doc = two_nodes()
        doc["nodes"][0]["owns"][0]["latency_class"] = 3
        errors = check(parse(doc))
        self.assertEqual(len(errors), 1)
        self.assertIn("L3", str(errors[0]))
        self.assertIn("L1", str(errors[0]))

    def test_a_looser_actor_may_bind_a_tighter_resource(self) -> None:
        # The permitted direction: an L4 reporter reading an L1 sensor is fine. It gets the value
        # late; the sensor does not get slower.
        doc = two_nodes()
        doc["actors"][0]["latency_class"] = 4
        self.assertEqual(check(parse(doc)), [])


class RuleThree(unittest.TestCase):
    """Class is transport-derived, not aspirational."""

    def test_an_l2_actor_may_not_reach_across_espnow(self) -> None:
        doc = two_nodes("espnow")
        doc["actors"][0]["latency_class"] = 2
        doc["nodes"][1]["owns"][0]["latency_class"] = 2
        doc["actors"][0]["needs"] = ["potluck://lab/node-1002/adc/0"]
        errors = check(parse(doc))
        self.assertEqual(len(errors), 1)
        self.assertIn("espnow", str(errors[0]))
        self.assertIn("L3", str(errors[0]))

    def test_the_same_binding_over_a_wired_hop_passes(self) -> None:
        # Nothing about the actor or the resource changed. The fabric did, and that is the point.
        for wired in ("uart", "can"):
            doc = two_nodes(wired)
            doc["actors"][0]["latency_class"] = 2
            doc["nodes"][1]["owns"][0]["latency_class"] = 2
            doc["actors"][0]["needs"] = ["potluck://lab/node-1002/adc/0"]
            self.assertEqual(check(parse(doc)), [], f"rejected over {wired}")

    def test_an_l4_actor_crosses_anything(self) -> None:
        doc = two_nodes("host")
        doc["actors"][0]["latency_class"] = 4
        doc["actors"][0]["needs"] = ["potluck://lab/node-1002/adc/0"]
        self.assertEqual(check(parse(doc)), [])

    def test_two_nodes_with_no_transport_between_them_cannot_bind_at_all(self) -> None:
        doc = two_nodes(None)
        doc["actors"][0]["latency_class"] = 4
        doc["actors"][0]["needs"] = ["potluck://lab/node-1002/adc/0"]
        errors = check(parse(doc))
        self.assertEqual(len(errors), 1)
        self.assertIn("cannot reach", str(errors[0]))

    def test_a_two_hop_route_is_not_a_route_in_v1(self) -> None:
        # Section 4's boundary condition: "two L3 hops at <500 ms each compose to <1 s, which is not
        # L3", so v1 paths are single-hop and forwarding is v2 work needing budget-summing.
        doc = two_nodes("espnow")
        doc["nodes"].append({"node_id": 0x1003, "label": "middle", "power": "mains", "owns": []})
        doc["links"] = [
            {"a": 0x1001, "b": 0x1003, "transport": "espnow"},
            {"a": 0x1003, "b": 0x1002, "transport": "espnow"},
        ]
        doc["actors"][0]["latency_class"] = 4
        doc["actors"][0]["needs"] = ["potluck://lab/node-1002/adc/0"]
        errors = check(parse(doc))
        self.assertEqual(len(errors), 1)
        self.assertIn("single-hop", str(errors[0]))
        self.assertIn("2 hops", str(errors[0]))

    def test_the_ceiling_helper_agrees_with_section_4s_table(self) -> None:
        m = parse(two_nodes("espnow"))
        self.assertEqual(binding_ceiling(m, 0x1001, 0x1001), (SAME_NODE_CLASS, "same node, no hop"))
        self.assertEqual(binding_ceiling(m, 0x1001, 0x1002)[0], TRANSPORT_CLASS["espnow"])
        self.assertEqual(TRANSPORT_CLASS["uart"], 2)
        self.assertEqual(TRANSPORT_CLASS["can"], 2)
        self.assertEqual(TRANSPORT_CLASS["espnow"], 3)
        self.assertEqual(TRANSPORT_CLASS["host"], 4)


class Headroom(unittest.TestCase):
    def test_actors_that_cannot_fit_on_paper_are_rejected(self) -> None:
        doc = two_nodes()
        doc["nodes"][0]["headroom_bytes"] = 4096
        doc["actors"][0]["headroom_bytes"] = 8192
        errors = check(parse(doc))
        self.assertEqual(len(errors), 1)
        self.assertIn("headroom", str(errors[0]))

    def test_a_node_that_declares_no_figure_is_not_checked(self) -> None:
        # A missing figure is not a claim of zero. The runtime reports the real number in every
        # heartbeat; inventing a limit here would reject placements that are perfectly fine.
        doc = two_nodes()
        doc["nodes"][0]["headroom_bytes"] = 0
        doc["actors"][0]["headroom_bytes"] = 1 << 20
        self.assertEqual(check(parse(doc)), [])

    def test_the_check_is_over_the_sum_of_what_is_placed_there(self) -> None:
        doc = two_nodes()
        doc["nodes"][0]["headroom_bytes"] = 10000
        doc["actors"][0]["headroom_bytes"] = 6000
        second = copy.deepcopy(doc["actors"][0])
        second["name"] = "loop2"
        doc["actors"].append(second)
        doc["placement"]["loop2"] = 0x1001
        errors = check(parse(doc))
        self.assertEqual(len(errors), 1)
        self.assertIn("12000", str(errors[0]))


class Unresolved(unittest.TestCase):
    def test_an_unplaced_actor_is_not_a_locality_error(self) -> None:
        # Constraints without a resolution are the input to the build tool. Rejecting them here
        # would mean a manifest could never be written before it was placed.
        doc = two_nodes()
        doc["placement"] = {}
        doc["actors"][0]["needs"] = ["potluck://lab/node-1002/adc/0"]
        self.assertEqual(check(parse(doc)), [])


class Fixtures(unittest.TestCase):
    def test_the_shipped_example_satisfies_the_contract(self) -> None:
        from potluck.manifest import load

        self.assertEqual(check(load(GOOD)), [])

    def test_the_broken_example_is_rejected_and_says_why(self) -> None:
        from potluck.manifest import load

        m = load(BROKEN)
        errors = check(m)
        # One of each kind the checker knows how to find. If a rule is ever dropped, this drops with
        # it rather than quietly passing.
        by_actor = {e.actor for e in errors}
        self.assertIn("vent_loop", by_actor)      # L1 actor, remote resource
        self.assertIn("pressure_trend", by_actor)  # actor tighter than the resource it binds
        self.assertIn("flow_limit", by_actor)      # same, on its own node
        self.assertIn("valve_mirror", by_actor)    # class the transport cannot carry
        self.assertIn("door_log", by_actor)        # no transport at all
        self.assertGreaterEqual(len(errors), 5)

    def test_the_command_line_exits_nonzero_on_the_broken_one(self) -> None:
        # How a build would use it. The output is captured rather than printed: this is the one test
        # that deliberately produces a page of error text, and a gate log is easier to read without
        # it. The text itself is checked above, from the errors rather than from the console.
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            good = locality_main(["check", GOOD])
            bad = locality_main(["check", BROKEN])
        self.assertEqual(good, 0)
        self.assertEqual(bad, 1)
        self.assertIn("satisfy the locality contract", out.getvalue())
        self.assertIn("violation", err.getvalue())


class Descriptions(unittest.TestCase):
    def test_every_class_has_words_a_person_can_read(self) -> None:
        for c in range(5):
            text = describe_class(c)
            self.assertIn(f"L{c}", text)
            self.assertIn("(", text)  # the budget, not just the label


if __name__ == "__main__":
    unittest.main()
