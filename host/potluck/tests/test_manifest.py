"""The deploy manifest -- ARCHITECTURE.md section 7.4, and the checks a build has to make.

The manifest is the one document that carries every policy the runtime deliberately refuses to
guess: which resources are strict about staleness, what each actor does when the host goes away,
which nodes may be borrowed for background work, and where every actor runs. A silently accepted
mistake in it is a policy that reverts to a default nobody chose -- so the validator's job is to be
loud, and this file's job is to prove it is.

Two checks here exist because of properties of the *firmware* rather than of the document:

  * a path hash collision, which the node cannot possibly detect (it only ever sees the hash), so
    the build is the last place it can be caught;
  * the 128-entry namespace cap, which otherwise fails at the 129th declare, at runtime, on one node.
"""

from __future__ import annotations

import copy
import json
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from potluck.manifest import (
    MAX_PATH_LEN,
    MAX_RESOURCES_PER_NODE,
    Manifest,
    ManifestErrors,
    load,
    parse,
)
from potluck.paths import path_hash

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
EXAMPLE = os.path.join(REPO, "manifests", "home.json")

# Two paths that really do collide under FNV-1a/32, found by search rather than asserted. The node
# stores only the hash, so to it these are one resource.
COLLIDE_A = "potluck://home/node-1001/x/aguzx"
COLLIDE_B = "potluck://home/node-1001/x/a52ad"


def minimal() -> dict:
    """The smallest manifest that passes, as a starting point for breaking one thing at a time."""
    return {
        "schema": 1,
        "system": "test",
        "min_core_version": 1,
        "nodes": [
            {
                "node_id": 0x1001,
                "label": "one",
                "power": "mains",
                "headroom_bytes": 40960,
                "owns": [
                    {
                        "path": "potluck://lab/node-1001/adc/0",
                        "unit": "volt",
                        "kind": "sampled",
                        "access": "read",
                        "latency_class": 1,
                        "staleness_bound_ms": 1000,
                        "staleness_policy": "informative",
                    }
                ],
            }
        ],
        "actors": [
            {
                "name": "reader",
                "module": "reader.so",
                "latency_class": 1,
                "needs": ["potluck://lab/node-1001/adc/0"],
                "headroom_bytes": 1024,
                "priority": "normal",
            }
        ],
        "bindings": {},
        "placement": {"reader": 0x1001},
    }


class Helpers(unittest.TestCase):
    def errors_from(self, doc: dict, **kw) -> list[str]:
        """Parse and return the error locations, so a test can name the field it broke."""
        with self.assertRaises(ManifestErrors) as cm:
            parse(doc, **kw)
        return [e.where for e in cm.exception.errors]

    def messages_from(self, doc: dict, **kw) -> str:
        with self.assertRaises(ManifestErrors) as cm:
            parse(doc, **kw)
        return cm.exception.report()


class TheExample(Helpers):
    def test_the_shipped_example_is_valid_and_resolved(self) -> None:
        m = load(EXAMPLE, require_placement=True)
        self.assertEqual(m.system, "home")
        self.assertEqual(len(m.nodes), 3)
        self.assertEqual(len(m.actors), 4)
        self.assertTrue(m.is_resolved())

    def test_a_round_trip_through_the_dict_form_keeps_the_digest(self) -> None:
        # The digest is what gets signed (section 9.3), so re-emitting a manifest must not change
        # its identity. If this breaks, a signature stops surviving a formatting pass.
        m = load(EXAMPLE)
        again = parse(m.to_dict())
        self.assertEqual(m.digest(), again.digest())
        self.assertEqual(m.canonical_bytes(), again.canonical_bytes())

    def test_whitespace_and_key_order_do_not_change_the_digest(self) -> None:
        with open(EXAMPLE, encoding="utf-8") as f:
            doc = json.load(f)
        reordered = json.loads(json.dumps(doc, sort_keys=True, indent=8))
        self.assertEqual(parse(doc).digest(), parse(reordered).digest())

    def test_the_binding_resolves_to_the_node_that_owns_the_pwm(self) -> None:
        m = load(EXAMPLE)
        owner = m.owner_of("potluck://home/vents/hallway")
        self.assertIsNotNone(owner)
        assert owner is not None
        self.assertEqual(owner.label, "hallway")
        # Section 7.2: the logical name is a binding, so the resource behind it keeps its own policy.
        r = m.resource("potluck://home/vents/hallway")
        self.assertIsNotNone(r)
        assert r is not None
        self.assertEqual(r.staleness_policy, "strict")


class Structure(Helpers):
    def test_the_minimal_manifest_passes(self) -> None:
        m = parse(minimal(), require_placement=True)
        self.assertEqual(len(m.all_paths()), 1)

    def test_an_unknown_key_is_an_error_and_not_a_warning(self) -> None:
        # A misspelled policy key is a policy that silently reverts to a default. That is the exact
        # failure section 4 rule 2 exists to prevent, so it cannot be tolerated at the top level or
        # anywhere inside.
        doc = minimal()
        doc["nodes"][0]["owns"][0]["stalenes_policy"] = "strict"
        self.assertIn("nodes[0].owns[0].stalenes_policy", self.errors_from(doc))

        doc = minimal()
        doc["actors"][0]["prioirty"] = "background"
        self.assertIn("actors[0].prioirty", self.errors_from(doc))

        doc = minimal()
        doc["systen"] = "typo"
        self.assertIn("<manifest>.systen", self.errors_from(doc))

    def test_a_missing_required_key_names_the_object(self) -> None:
        doc = minimal()
        del doc["actors"][0]["module"]
        self.assertIn("actors[0]", self.errors_from(doc))

    def test_every_problem_is_reported_not_just_the_first(self) -> None:
        doc = minimal()
        doc["actors"][0]["priority"] = "urgent"
        doc["nodes"][0]["power"] = "nuclear"
        doc["nodes"][0]["owns"][0]["access"] = "rw"
        report = self.messages_from(doc)
        self.assertIn("actors[0].priority", report)
        self.assertIn("nodes[0].power", report)
        self.assertIn("nodes[0].owns[0].access", report)

    def test_a_boolean_where_a_count_belongs_is_refused(self) -> None:
        # bool is an int in Python, so `"headroom_bytes": true` would otherwise sail through as 1.
        doc = minimal()
        doc["nodes"][0]["headroom_bytes"] = True
        self.assertIn("nodes[0].headroom_bytes", self.errors_from(doc))

    def test_a_node_id_must_be_addressable(self) -> None:
        for bad in (0, 0xFFFF, -1, 0x10000):
            doc = minimal()
            doc["nodes"][0]["node_id"] = bad
            self.assertIn("nodes[0].node_id", self.errors_from(doc), f"accepted {bad}")

    def test_a_future_schema_is_refused_rather_than_guessed_at(self) -> None:
        doc = minimal()
        doc["schema"] = 2
        self.assertIn("schema", self.errors_from(doc))


class Paths(Helpers):
    def test_a_path_must_be_a_potluck_uri(self) -> None:
        cases = {
            "lab/node-1001/adc/0": "no scheme",
            "potluck://lab": "no segment after the cluster",
            "potluck://lab/node-1001/": "trailing slash",
            "potluck://lab//adc/0": "empty segment",
            "potluck://lab/node 1001/adc/0": "whitespace",
            "potluck://lab/nøde/adc/0": "not ASCII",
            "potluck://lab/" + "a" * MAX_PATH_LEN: "too long",
        }
        for bad, why in cases.items():
            doc = minimal()
            doc["nodes"][0]["owns"][0]["path"] = bad
            self.assertIn("nodes[0].owns[0].path", self.errors_from(doc), f"accepted {why}: {bad}")

    def test_two_paths_that_hash_alike_are_a_build_error(self) -> None:
        # The check that cannot be made anywhere else. The node stores a 32-bit hash and never the
        # string, so to it these two paths are one resource; NsError::HashCollision exists to say so.
        self.assertEqual(path_hash(COLLIDE_A), path_hash(COLLIDE_B))
        self.assertNotEqual(COLLIDE_A, COLLIDE_B)

        doc = minimal()
        doc["nodes"][0]["owns"] = [
            {"path": COLLIDE_A, "unit": "volt"},
            {"path": COLLIDE_B, "unit": "volt"},
        ]
        doc["actors"][0]["needs"] = [COLLIDE_A]
        report = self.messages_from(doc)
        self.assertIn("hash to", report)
        self.assertIn(COLLIDE_B, report)

    def test_a_collision_between_a_logical_name_and_a_resource_is_caught_too(self) -> None:
        # A binding is hashed exactly like a canonical path, so it can collide with one.
        doc = minimal()
        doc["nodes"][0]["owns"] = [{"path": COLLIDE_A, "unit": "volt"}]
        doc["bindings"] = {COLLIDE_B: COLLIDE_A}
        doc["actors"][0]["needs"] = [COLLIDE_A]
        self.assertIn("hash to", self.messages_from(doc))

    def test_the_same_path_twice_on_one_node_is_refused(self) -> None:
        doc = minimal()
        doc["nodes"][0]["owns"].append(dict(doc["nodes"][0]["owns"][0]))
        self.assertIn("nodes[0].owns[1].path", self.errors_from(doc))

    def test_one_resource_has_one_owner(self) -> None:
        doc = minimal()
        second = copy.deepcopy(doc["nodes"][0])
        second["node_id"] = 0x1002
        second["label"] = "two"
        doc["nodes"].append(second)
        self.assertIn("nodes[1].owns", self.errors_from(doc))

    def test_more_resources_than_the_node_table_holds_is_refused(self) -> None:
        doc = minimal()
        doc["nodes"][0]["owns"] = [
            {"path": f"potluck://lab/node-1001/adc/{i}", "unit": "volt"}
            for i in range(MAX_RESOURCES_PER_NODE + 1)
        ]
        doc["actors"][0]["needs"] = ["potluck://lab/node-1001/adc/0"]
        report = self.messages_from(doc)
        self.assertIn("nodes[0].owns", report)
        self.assertIn(str(MAX_RESOURCES_PER_NODE), report)

    def test_exactly_the_cap_is_allowed(self) -> None:
        doc = minimal()
        doc["nodes"][0]["owns"] = [
            {"path": f"potluck://lab/node-1001/adc/{i}", "unit": "volt"}
            for i in range(MAX_RESOURCES_PER_NODE)
        ]
        doc["actors"][0]["needs"] = ["potluck://lab/node-1001/adc/0"]
        m = parse(doc)
        self.assertEqual(len(m.nodes[0].owns), MAX_RESOURCES_PER_NODE)

    def test_a_binding_must_land_on_something_real(self) -> None:
        doc = minimal()
        doc["bindings"] = {"potluck://lab/sensors/main": "potluck://lab/node-1001/adc/9"}
        self.assertIn("bindings['potluck://lab/sensors/main']", self.errors_from(doc))

    def test_a_logical_name_may_not_shadow_a_declared_resource(self) -> None:
        doc = minimal()
        doc["bindings"] = {"potluck://lab/node-1001/adc/0": "potluck://lab/node-1001/adc/0"}
        self.assertIn("bindings['potluck://lab/node-1001/adc/0']", self.errors_from(doc))

    def test_an_actor_cannot_need_a_path_nobody_declares(self) -> None:
        doc = minimal()
        doc["actors"][0]["needs"] = ["potluck://lab/node-1001/imaginary/0"]
        self.assertIn("actors[0].needs[0]", self.errors_from(doc))


class Policy(Helpers):
    def test_strict_without_a_bound_is_refused(self) -> None:
        # "Strict" means deliver an error past the staleness bound. With no bound there is nothing
        # to be past, so the policy would never fire and the manifest would read as if it did.
        doc = minimal()
        doc["nodes"][0]["owns"][0]["staleness_policy"] = "strict"
        doc["nodes"][0]["owns"][0]["staleness_bound_ms"] = 0
        self.assertIn("nodes[0].owns[0]", self.errors_from(doc))

    def test_a_staleness_policy_on_an_event_is_refused(self) -> None:
        # Section 7.2: event entries have queue semantics and READ is not how you get them.
        doc = minimal()
        doc["nodes"][0]["owns"][0]["kind"] = "event"
        doc["nodes"][0]["owns"][0]["staleness_policy"] = "strict"
        doc["nodes"][0]["owns"][0]["staleness_bound_ms"] = 100
        self.assertIn("nodes[0].owns[0]", self.errors_from(doc))

    def test_an_actor_that_can_write_must_say_what_it_does_when_the_host_goes(self) -> None:
        # Section 8.3: an actuator fed host-computed setpoints and left to `continue` keeps
        # integrating a dead host's last command. There is no safe default, so it must be stated.
        doc = minimal()
        doc["nodes"][0]["owns"][0]["access"] = "read_write"
        report = self.messages_from(doc)
        self.assertIn("actors[0]", report)
        self.assertIn("on_host_loss", report)

    def test_a_read_only_actor_needs_no_such_declaration(self) -> None:
        doc = minimal()
        parse(doc)  # access is "read"; no on_host_loss required, and none given

    def test_declaring_it_satisfies_the_requirement(self) -> None:
        doc = minimal()
        doc["nodes"][0]["owns"][0]["access"] = "read_write"
        doc["actors"][0]["on_host_loss"] = "hold"
        m = parse(doc)
        self.assertEqual(m.actors[0].on_host_loss, "hold")

    def test_the_requirement_follows_a_binding(self) -> None:
        # The actor names a logical path; the writable resource is behind it. Section 7.2's whole
        # point is that application code does not know which node it is on -- but the check must.
        doc = minimal()
        doc["nodes"][0]["owns"][0]["access"] = "write"
        doc["bindings"] = {"potluck://lab/vents/main": "potluck://lab/node-1001/adc/0"}
        doc["actors"][0]["needs"] = ["potluck://lab/vents/main"]
        self.assertIn("actors[0]", self.errors_from(doc))


class BackgroundWork(Helpers):
    def test_background_work_on_a_solar_node_needs_an_opt_in(self) -> None:
        # Section 7.8: "harvesting idle cycles denies sleep". Battery and solar nodes are out of the
        # background pool unless the manifest says otherwise -- the garden node stays sleepy.
        doc = minimal()
        doc["nodes"][0]["power"] = "solar"
        doc["actors"][0]["priority"] = "background"
        report = self.messages_from(doc)
        self.assertIn("actors[0]", report)
        self.assertIn("allow_background", report)

    def test_the_opt_in_makes_it_legal(self) -> None:
        doc = minimal()
        doc["nodes"][0]["power"] = "solar"
        doc["nodes"][0]["allow_background"] = True
        doc["actors"][0]["priority"] = "background"
        m = parse(doc)
        self.assertTrue(m.nodes[0].background_allowed())

    def test_a_mains_node_is_in_the_pool_by_default(self) -> None:
        doc = minimal()
        doc["actors"][0]["priority"] = "background"
        m = parse(doc)
        self.assertTrue(m.nodes[0].background_allowed())

    def test_a_mains_node_can_opt_out(self) -> None:
        doc = minimal()
        doc["nodes"][0]["allow_background"] = False
        doc["actors"][0]["priority"] = "background"
        self.assertIn("actors[0]", self.errors_from(doc))


class Placement(Helpers):
    def test_a_pin_must_name_a_declared_node(self) -> None:
        doc = minimal()
        doc["actors"][0]["pin"] = 0x2222
        del doc["placement"]["reader"]
        self.assertIn("actors[0].pin", self.errors_from(doc))

    def test_a_pin_and_a_frozen_placement_may_not_disagree(self) -> None:
        doc = minimal()
        second = {"node_id": 0x1002, "label": "two", "power": "mains", "owns": []}
        doc["nodes"].append(second)
        doc["actors"][0]["pin"] = 0x1002
        doc["placement"]["reader"] = 0x1001
        self.assertIn("placement['reader']", self.errors_from(doc))

    def test_placement_of_an_actor_that_does_not_exist(self) -> None:
        doc = minimal()
        doc["placement"]["ghost"] = 0x1001
        self.assertIn("placement['ghost']", self.errors_from(doc))

    def test_placement_onto_a_node_that_does_not_exist(self) -> None:
        doc = minimal()
        doc["placement"]["reader"] = 0x3333
        self.assertIn("placement['reader']", self.errors_from(doc))

    def test_an_unresolved_manifest_parses_but_is_not_deployable(self) -> None:
        # Constraints without a resolution are the *input* to the build tool, so this must parse.
        # It only becomes an error when something asks for a manifest that could be deployed.
        doc = minimal()
        doc["placement"] = {}
        m = parse(doc)
        self.assertFalse(m.is_resolved())
        self.assertIn("actors[0]", self.errors_from(doc, require_placement=True))

    def test_a_pin_counts_as_a_resolution(self) -> None:
        doc = minimal()
        doc["placement"] = {}
        doc["actors"][0]["pin"] = 0x1001
        m = parse(doc, require_placement=True)
        self.assertEqual(m.placement_of(m.actors[0]), 0x1001)


class Duplicates(Helpers):
    def test_two_nodes_with_one_id(self) -> None:
        doc = minimal()
        second = {"node_id": 0x1001, "label": "clone", "power": "mains", "owns": []}
        doc["nodes"].append(second)
        self.assertIn("nodes[1].node_id", self.errors_from(doc))

    def test_two_actors_with_one_name(self) -> None:
        doc = minimal()
        doc["actors"].append(copy.deepcopy(doc["actors"][0]))
        self.assertIn("actors[1].name", self.errors_from(doc))


class Digest(unittest.TestCase):
    def test_the_canonical_form_is_sorted_ascii_with_one_newline(self) -> None:
        m = parse(minimal())
        raw = m.canonical_bytes()
        self.assertTrue(raw.endswith(b"\n"))
        self.assertEqual(raw.count(b"\n"), 1)
        self.assertNotIn(b", ", raw)  # no incidental whitespace
        text = raw.decode("ascii")
        self.assertLess(text.index('"actors"'), text.index('"nodes"'))  # sorted keys

    def test_changing_one_policy_changes_the_digest(self) -> None:
        # If it did not, a signature would not cover the policy, which is the point of signing it.
        a = parse(minimal())
        doc = minimal()
        doc["nodes"][0]["owns"][0]["staleness_policy"] = "strict"
        b = parse(doc)
        self.assertNotEqual(a.digest(), b.digest())


if __name__ == "__main__":
    unittest.main()
