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


if __name__ == "__main__":
    unittest.main()
