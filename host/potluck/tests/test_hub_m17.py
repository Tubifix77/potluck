"""M17: the hub found on the network (multicast DNS), paired by the owner's approval, and its own entities in Home
Assistant on the presenter's devices."""

from __future__ import annotations

import http.client
import json
import os
import socket
import struct
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.dirname(__file__))

from potluck import hub as hb  # noqa: E402
from potluck import hub_extras as hx  # noqa: E402
from potluck import hub_mdns as md  # noqa: E402
from potluck import hubauth as ha  # noqa: E402
from test_hub import HubFixture, free_port  # noqa: E402


def query(name: str, qtype: int) -> bytes:
    return struct.pack("!HHHHHH", 0, 0, 1, 0, 0, 0) + md.encode_name(name) + struct.pack("!HH", qtype, 1)


def parse_rrs(msg: bytes) -> list[tuple[str, int, int, bytes]]:
    _id, _fl, qd, an, ns, ar = struct.unpack("!HHHHHH", msg[:12])
    at = 12
    for _ in range(qd):
        _n, at = md.decode_name(msg, at)
        at += 4
    out = []
    for _ in range(an + ns + ar):
        name, at = md.decode_name(msg, at)
        rtype, _cls, ttl, ln = struct.unpack("!HHIH", msg[at:at + 10])
        at += 10
        out.append((name, rtype, ttl, msg[at:at + ln]))
        at += ln
    return out


class Announce(unittest.TestCase):
    def setUp(self) -> None:
        self.a = md.Announcer("lab", "ef76cc2e", 8443, "ab" * 32, "192.168.0.42")

    def test_a_browse_is_answered_with_the_instance_its_port_txt_and_address(self) -> None:
        resp = self.a.answer(query("_potluck._tcp.local.", md.TYPE_PTR))
        self.assertIsNotNone(resp)
        self.assertEqual(struct.unpack("!HH", resp[:4]), (0, 0x8400))  # ID 0; QR and AA (RFC 6762 18.1-18.4)
        rrs = {t: (n, ttl, d) for n, t, ttl, d in parse_rrs(resp)}
        self.assertEqual(md.decode_name(rrs[md.TYPE_PTR][2], 0)[0], "Potluck lab ef76cc2e._potluck._tcp.local.")
        self.assertEqual(struct.unpack("!HHH", rrs[md.TYPE_SRV][2][:6]), (0, 0, 8443))
        txt = rrs[md.TYPE_TXT][2]
        items, i = [], 0
        while i < len(txt):
            items.append(txt[i + 1:i + 1 + txt[i]].decode())
            i += 1 + txt[i]
        self.assertIn("cell=ef76cc2e", items)
        self.assertIn("fp=" + "ab" * 32, items)
        self.assertEqual(socket.inet_ntoa(rrs[md.TYPE_A][2]), "192.168.0.42")
        self.assertEqual((rrs[md.TYPE_SRV][1], rrs[md.TYPE_PTR][1]), (120, 75 * 60))  # RFC 6762 section 10

    def test_other_queries_and_responses_get_nothing_and_goodbye_is_ttl_zero(self) -> None:
        self.assertIsNone(self.a.answer(query("_googlecast._tcp.local.", md.TYPE_PTR)))
        resp = bytearray(query("_potluck._tcp.local.", md.TYPE_PTR))
        resp[2] |= 0x80  # a response, not a query
        self.assertIsNone(self.a.answer(bytes(resp)))
        self.assertTrue(all(ttl == 0 for _n, _t, ttl, _d in parse_rrs(self.a.full(ttl_scale=0))))

    def test_a_one_shot_resolver_gets_its_id_its_question_and_short_ttls(self) -> None:
        q = bytearray(query("_potluck._tcp.local.", md.TYPE_PTR))
        q[0:2] = b"\x12\x34"
        resp = self.a.answer(bytes(q), legacy=True)  # RFC 6762 6.7: a query from a port other than 5353
        self.assertEqual(struct.unpack("!HHH", resp[:6]), (0x1234, 0x8400, 1))
        self.assertTrue(all(ttl <= 10 for _n, _t, ttl, _d in parse_rrs(resp)))

    def test_a_name_with_a_pointer_loop_is_refused_not_followed(self) -> None:
        bad = struct.pack("!HHHHHH", 0, 0, 1, 0, 0, 0) + bytes([0xC0, 12]) + struct.pack("!HH", 12, 1)
        with self.assertRaises(ValueError):
            self.a.answer(bad)


class Pair(HubFixture):
    def setUp(self) -> None:
        super().setUp()
        self.tmp = tempfile.TemporaryDirectory()
        self.users = ha.Users(Path(self.tmp.name) / "users.json")
        self.users.save()
        self.pairing = ha.Pairing(self.users)
        self.model.pairing = self.pairing
        self.local_addr = f"127.0.0.1:{free_port()}"
        self.lan_addr = f"127.0.0.1:{free_port()}"
        self.local = hb.serve_api(self.model, self.local_addr, pairing=self.pairing)
        self.lan = hb.serve_api(self.model, self.lan_addr, sessions=ha.Sessions(self.users), pairing=self.pairing)

    def tearDown(self) -> None:
        self.local.shutdown()
        self.lan.shutdown()
        self.tmp.cleanup()
        super().tearDown()

    def req(self, addr, method, path, body=None, headers=None):
        host, _, port = addr.rpartition(":")
        c = http.client.HTTPConnection(host, int(port), timeout=5)
        h = {"Content-Type": "application/json", "X-Potluck-Request": "1", **(headers or {})}
        c.request(method, path, body=json.dumps(body) if body is not None else None, headers=h)
        r = c.getresponse()
        data = r.read()
        c.close()
        return r.status, json.loads(data) if data[:1] == b"{" else data

    def test_a_program_is_paired_only_when_the_owner_approves_on_the_pc(self) -> None:
        code, r = self.req(self.lan_addr, "POST", "/api/pair", {"name": "Home Assistant", "right": "look"})
        self.assertEqual(code, 202)
        pid = r["pair_id"]
        self.assertEqual(self.req(self.lan_addr, "GET", f"/api/pair/{pid}")[1]["state"], "pending")
        self.assertEqual([p["name"] for p in self.req(self.local_addr, "GET", "/api/model")[1]["hub"]["pairing"]],
                         ["Home Assistant"])
        # Never answered from the network side
        self.assertEqual(self.req(self.lan_addr, "POST", "/api/pairing", {"pair_id": pid, "approve": True})[0], 404)
        self.assertEqual(self.req(self.local_addr, "POST", "/api/pairing", {"pair_id": pid, "approve": True})[0], 200)
        code, r = self.req(self.lan_addr, "GET", f"/api/pair/{pid}")
        self.assertEqual((r["state"], r["right"]), ("approved", "look"))
        token = r["token"]
        self.assertEqual(self.req(self.lan_addr, "GET", f"/api/pair/{pid}")[0], 404)  # handed over once
        auth = {"Authorization": f"Bearer {token}"}
        self.assertEqual(self.req(self.lan_addr, "GET", "/api/model", headers=auth)[0], 200)
        self.assertEqual(self.req(self.lan_addr, "GET", "/api/whoami", headers=auth)[1]["right"], "look")
        code, r = self.req(self.lan_addr, "POST", "/api/write", {"path": "x", "value": 1}, headers=auth)
        self.assertEqual(code, 403)  # look only
        self.assertEqual(self.req(self.lan_addr, "GET", "/api/model", headers={"Authorization": "Bearer nope"})[0], 401)
        # Revoked (as `potluck.hubauth revoke` does, on the file): refused from the next request
        u = ha.Users(self.users.path)
        u.tokens.pop("Home Assistant")
        os.utime(self.users.path, (1, 1))  # a different mtime even within the same second
        u.save()
        self.assertEqual(self.req(self.lan_addr, "GET", "/api/model", headers=auth)[0], 401)

    def test_a_denied_request_gets_no_token_and_too_many_are_refused(self) -> None:
        _, r = self.req(self.lan_addr, "POST", "/api/pair", {"name": "someone"})
        self.req(self.local_addr, "POST", "/api/pairing", {"pair_id": r["pair_id"], "approve": False})
        self.assertEqual(self.req(self.lan_addr, "GET", f"/api/pair/{r['pair_id']}")[1], {"state": "denied"})
        for _ in range(ha.Pairing.MAX_PENDING):
            self.assertEqual(self.req(self.lan_addr, "POST", "/api/pair", {"name": "x"})[0], 202)
        self.assertEqual(self.req(self.lan_addr, "POST", "/api/pair", {"name": "x"})[0], 429)


class Entities(unittest.TestCase):
    class FakeModel:
        cell_id = "ef76cc2e"

        class cluster:
            system = "lab"
            present = {"broker": "192.168.0.77:8883", "keepalive_s": 10, "tls_ca": "AAAA"}

    SNAP = {"boards": [
        {"node": "6300", "state": "alive", "up_ms": 61_000, "rssi": -22, "heap": {"internal_free": 207319},
         "firmware": {"version": "1c7b6b6-a1", "counter": 34},
         "links": [{"peer": "7368", "delivery": 0.9997, "cable": False}, {"peer": "00fe", "delivery": None, "cable": True}]},
        {"node": "7368", "state": "asleep", "up_ms": None, "rssi": None, "heap": None,
         "firmware": {"version": "fb1dabd-a1", "counter": 33}, "links": []}],
        "apps": [{"name": "passive-sensor", "working": False}, {"name": "clock", "working": True}]}

    def test_the_hubs_entities_land_on_the_presenters_own_devices_and_install_nothing(self) -> None:
        x = hx.Extras(self.FakeModel())
        cfgs = dict(x.configs(self.SNAP))
        st = cfgs["homeassistant/sensor/potluck_ef76cc2e/hub_6300_state/config"]
        # The presenter's device block, byte for byte (present_core.cpp: identifiers, "<system> node-<id>", Potluck)
        self.assertEqual(st["device"], {"identifiers": ["potluck_ef76cc2e_6300"], "name": "lab node-6300",
                                        "manufacturer": "Potluck"})
        self.assertEqual(st["entity_category"], "diagnostic")
        fw = cfgs["homeassistant/update/potluck_ef76cc2e/hub_6300_firmware/config"]
        self.assertNotIn("command_topic", fw)  # so Home Assistant offers no install (mqtt/update.py)
        self.assertIn("homeassistant/binary_sensor/potluck_ef76cc2e/hub_app_passive-sensor/config", cfgs)
        self.assertEqual(cfgs["homeassistant/sensor/potluck_ef76cc2e/hub_cell_apps_working/config"]["device"]
                         ["identifiers"], ["potluck_ef76cc2e_cell"])
        self.assertTrue(x.cfg["tls"])

    def test_states_say_asleep_the_newest_firmware_and_which_app_is_down(self) -> None:
        st = dict(hx.Extras(self.FakeModel()).states(self.SNAP))
        a = st["potluck/ef76cc2e/hub/6300/state"]
        self.assertEqual((a["state"], a["internal_free"], a["up_s"], a["delivery_pct"]), ("alive", 207319, 61, 99.97))
        self.assertEqual(st["potluck/ef76cc2e/hub/7368/state"]["state"], "asleep")
        self.assertEqual(st["potluck/ef76cc2e/hub/7368/firmware"],
                         {"installed_version": "fb1dabd-a1", "latest_version": "1c7b6b6-a1", "title": "Potluck firmware"})
        cell = st["potluck/ef76cc2e/hub/cell/state"]
        self.assertEqual((cell["apps_working"], cell["problems"]["passive-sensor"]), (1, True))

    def test_an_app_not_yet_seen_while_the_hub_starts_is_no_problem_in_home_assistant(self) -> None:
        import copy
        snap = copy.deepcopy(self.SNAP)
        for a in snap["apps"]:
            a["working"] = None  # hub.starting: not seen working yet
        cell = dict(hx.Extras(self.FakeModel()).states(snap))["potluck/ef76cc2e/hub/cell/state"]
        self.assertFalse(any(cell["problems"].values()))
        self.assertEqual((cell["apps_working"], cell["attrs"]["not_working"]), (0, []))

class Setup(Pair):
    """M20's setup wizard on the hub: owner actions on the PC's own address only."""

    def wait(self, jid):
        for _ in range(300):
            j = self.req(self.local_addr, "GET", f"/api/setup/jobs/{jid}")[1]
            if j["state"] != "running":
                return j
            time.sleep(0.02)
        self.fail("job still running")

    def test_setup_lives_on_the_pcs_own_address_only(self) -> None:
        self.assertEqual(self.req(self.local_addr, "GET", "/api/setup/ports")[0], 200)
        self.assertIn(self.req(self.lan_addr, "GET", "/api/setup/ports")[0], (401, 404))
        code = self.req(self.lan_addr, "POST", "/api/setup/probe", {"port": "COM99"})[0]
        self.assertIn(code, (401, 404))  # never served to the network side

    def test_a_probe_of_a_port_with_no_board_fails_as_a_job_and_a_foreign_build_is_refused(self) -> None:
        code, r = self.req(self.local_addr, "POST", "/api/setup/probe", {"port": "COM99-not-there"})
        self.assertEqual(code, 202)
        self.assertEqual(self.wait(r["job"])["state"], "failed")
        code, r = self.req(self.local_addr, "POST", "/api/setup/flash", {"port": "COM98", "build": self.tmp.name})
        self.assertEqual(code, 403)

    def test_a_new_board_and_its_room_go_into_the_cluster_file_and_show_at_once(self) -> None:
        from potluck.apply import load_cluster
        p = Path(self.tmp.name) / "home.json"
        p.write_text(json.dumps({"schema": "potluck-cluster/1", "system": "home", "firmware": None,
                                 "boards": [{"node": "6300", "label": "A"}]}), encoding="utf-8")
        saved = self.model.cluster
        self.model.cluster = load_cluster(str(p))
        try:
            code, r = self.req(self.local_addr, "POST", "/api/setup/board", {"node": "8160", "room": "Kitchen"})
            self.assertEqual((code, r["board"]["label"]), (200, "node-8160"))
            self.assertEqual(self.model.room_of(0x8160), "Kitchen")
            self.assertEqual(self.req(self.local_addr, "POST", "/api/setup/board", {"node": "8160", "label": "A"})[0],
                             409)
            self.assertIn(self.req(self.lan_addr, "POST", "/api/setup/board", {"node": "8161"})[0], (401, 404))
            self.assertEqual(len(json.loads(p.read_text(encoding="utf-8"))["boards"]), 2)
        finally:
            self.model.cluster = saved


    def test_the_wizard_proposes_a_tree_from_what_the_boards_measure_and_writes_it(self) -> None:
        # M19/M20: three buildings of eight boards; two chargers in each hear the others' across the yard
        from potluck.apply import check_peers, load_cluster
        groups = [[0x100 + 0x100 * g + i for i in range(8)] for g in range(3)]
        chargers = [g[0] for g in groups] + [g[1] for g in groups]
        boards = []
        for g in groups:
            for n in g:
                links = [{"peer": f"{q:04x}", "delivery": 1.0, "rssi": -50} for q in g if q != n]
                if n in chargers:
                    links += [{"peer": f"{q:04x}", "delivery": 0.97, "rssi": -80} for q in chargers if q not in g]
                boards.append({"node": f"{n:04x}", "links": links})
        p = Path(self.tmp.name) / "farm.json"
        p.write_text(json.dumps({"schema": "potluck-cluster/1", "system": "farm", "firmware": None,
                                 "boards": [{"node": b["node"], "label": "b" + b["node"]} for b in boards]}),
                     encoding="utf-8")
        saved, saved_snap = self.model.cluster, self.model.snapshot
        self.model.cluster = load_cluster(str(p))
        self.model.snapshot = lambda: {"boards": boards}
        try:
            code, r = self.req(self.local_addr, "POST", "/api/setup/tree", {"chargers": [f"{n:04x}" for n in chargers]})
            self.assertEqual(code, 200)
            self.assertEqual(sorted(len(v) for v in r["proposal"]["cells"].values()), [8, 8, 8])
            self.assertEqual(r["proposal"]["notes"], [])
            self.assertFalse(r["written"])
            code, r = self.req(self.local_addr, "POST", "/api/setup/tree",
                               {"chargers": [f"{n:04x}" for n in chargers], "write": True})
            self.assertEqual((code, r["written"]), (200, True))
            cl = load_cluster(str(p))
            self.assertEqual(len(cl.parents), 3)
            check_peers(cl, 0x100)  # within every board's nineteen peers
            self.assertIn(self.req(self.lan_addr, "POST", "/api/setup/tree", {})[0], (401, 404))
        finally:
            self.model.cluster, self.model.snapshot = saved, saved_snap


class LibraryRoutes(Pair):
    """M20's library on the hub: apps on and off, what Home Assistant may see -- the PC's own address only."""

    def test_the_library_reads_and_writes_the_cluster_file(self) -> None:
        from potluck.apply import load_cluster
        p = Path(self.tmp.name) / "home.json"
        p.write_text(json.dumps({"schema": "potluck-cluster/1", "system": "home", "firmware": None,
                                 "boards": [{"node": "6300", "label": "A"}],
                                 "apps": [{"name": "passive-sensor", "git": ".", "components": "c"}]}), encoding="utf-8")
        saved = self.model.cluster
        self.model.cluster = load_cluster(str(p))
        try:
            code, lib = self.req(self.local_addr, "GET", "/api/setup/library")
            self.assertEqual((code, lib["apps"][0]["enabled"], lib["home_assistant"]["on"]), (200, True, False))
            code, lib = self.req(self.local_addr, "POST", "/api/setup/library", {"app": "passive-sensor", "enabled": False})
            self.assertEqual((code, lib["apps"][0]["enabled"]), (200, False))
            code, lib = self.req(self.local_addr, "POST", "/api/setup/library",
                                 {"outside": {"potluck://home/act/rf_fusion/out": "read"}})
            self.assertEqual(code, 200)
            self.assertIn({"path": "potluck://home/act/rf_fusion/out", "name": "act/rf_fusion/out", "mode": "read",
                           "controllable": False, "missing": True}, lib["resources"])
            self.assertIn(self.req(self.lan_addr, "GET", "/api/setup/library")[0], (401, 404))
            self.assertEqual(self.req(self.local_addr, "POST", "/api/setup/broker", {"broker": "127.0.0.1:1"})[1]["ok"],
                             False)
        finally:
            self.model.cluster = saved

    def test_the_store_is_off_until_turned_on_and_an_install_certifies_and_writes_the_cell(self) -> None:
        import hashlib
        import os
        from unittest import mock
        from potluck import ed25519_ref as ed, guest as gu, signing as sg, store as so
        from potluck.apply import load_cluster
        d = Path(self.tmp.name) / "storecell"
        d.mkdir()
        ca = sg.generate("ca", "test")
        (d / "ca.pub").write_text(json.dumps(ca.public_dict()))
        (d / "ca.key").write_text(json.dumps(ca.private_dict()))
        p = d / "home.json"
        p.write_text(json.dumps({"schema": "potluck-cluster/1", "system": "home", "firmware": None,
                                 "boards": [{"node": "6300", "label": "A"}], "keys": {"ca": "ca.pub"}}), encoding="utf-8")
        wasm = (Path(__file__).resolve().parents[3] / "guests" / "build" / "overheat_alarm.wasm").read_bytes()
        sk = os.urandom(32)
        app = so.parse_app({"type": so.APP_TYPE, "name": "overheat_alarm", "author_key": ed.public_key(sk).hex(),
                            "module": "a.wasm", "sha256": hashlib.sha256(wasm).hexdigest(),
                            "sig": ed.sign(sk, gu.signed_message(wasm)).hex(),
                            "inputs": [{"name": "t", "type": "f32"}], "outputs": "out:f32"})
        saved = self.model.cluster
        self.model.cluster = load_cluster(str(p))
        try:
            with mock.patch.object(so, "fetch", return_value=(app, wasm)):
                self.assertEqual(self.req(self.local_addr, "GET", "/api/setup/store")[1]["on"], False)
                code, r = self.req(self.local_addr, "POST", "/api/setup/store/look", {"repo": "acme/alarm"})
                self.assertEqual((code, r["ok"]), (409, False))  # off: nothing is fetched
                self.assertEqual(self.req(self.local_addr, "POST", "/api/setup/store", {"on": True})[1]["on"], True)
                code, r = self.req(self.local_addr, "POST", "/api/setup/store/look", {"repo": "acme/alarm"})
                self.assertEqual((code, r["author"], r["inputs"][0]["name"]), (200, "new", "t"))
                code, r = self.req(self.local_addr, "POST", "/api/setup/store/install",
                                   {"repo": "acme/alarm", "bindings": ["potluck://home/node-6300/x"]})
                self.assertEqual((code, r["installed"]["author"], [a["name"] for a in r["apps"]]),
                                 (200, so.FIRST_AUTHOR, ["overheat_alarm"]))
                self.assertEqual(gu.verify_bundle(gu.load_bundle(str(d / "store" / "overheat_alarm.guest.json")),
                                                  ca.public), so.FIRST_AUTHOR)
                self.assertIn(self.req(self.lan_addr, "POST", "/api/setup/store/install",
                                       {"repo": "acme/alarm", "bindings": []})[0], (401, 403, 404))
                code, r = self.req(self.local_addr, "POST", "/api/setup/store/remove", {"name": "overheat_alarm"})
                self.assertEqual((code, r["apps"]), (200, []))
        finally:
            self.model.cluster = saved


class FirstRun(unittest.TestCase):
    """M20: a PC with no cell yet -- the hub makes one, then attaches to the first board's own USB port."""

    def test_no_cell_then_a_cell_then_the_first_board(self) -> None:
        from potluck import apply as ap
        from potluck import transport as tr
        from potluck.fake_node import FakeNode
        tmp = tempfile.TemporaryDirectory()
        saved = (ap.default_cache, tr.SerialTransport)
        ap.default_cache = lambda: Path(tmp.name) / "cache"
        host_side, node_side = tr.LoopbackTransport.pair()
        node = FakeNode(node_side, node_id=0x0101)
        node.start()
        opened = []
        tr.SerialTransport = lambda port, baud, **kw: opened.append((port, baud)) or host_side
        hub = hb.Hub(tr.NullTransport())
        hub.start()
        model = hb.Model(hub, pull_period_ms=200)
        addr = f"127.0.0.1:{free_port()}"
        api = hb.serve_api(model, addr)
        try:
            def post(path, body):
                c = http.client.HTTPConnection("127.0.0.1", int(addr.rpartition(":")[2]), timeout=10)
                c.request("POST", path, body=json.dumps(body),
                          headers={"Content-Type": "application/json", "X-Potluck-Request": "1"})
                r = c.getresponse()
                out = (r.status, json.loads(r.read()))
                c.close()
                return out
            self.assertEqual(post("/api/setup/attach", {"port": "COM9"})[0], 409)  # no cell yet
            code, r = post("/api/setup/newcell", {"system": "home", "passphrase": "a long enough passphrase"})
            self.assertEqual(code, 200, r)
            self.assertTrue((Path(tmp.name) / "cells" / "home" / "vault.json").exists())
            self.assertEqual(post("/api/setup/newcell", {"system": "x", "passphrase": "a long enough passphrase"})[0], 409)
            code, r = post("/api/setup/attach", {"port": "COM9"})
            self.assertEqual((code, r["node"], opened), (200, "0101", [("COM9", 115200)]))
            self.assertEqual((model.cluster.port, model.cluster.baud), ("COM9", 115200))
            self.assertEqual(post("/api/setup/attach", {"port": "COM10"})[0], 409)  # attached once
        finally:
            api.shutdown()
            hub.close()
            node.stop()
            ap.default_cache, tr.SerialTransport = saved
            tmp.cleanup()


if __name__ == "__main__":
    unittest.main()
