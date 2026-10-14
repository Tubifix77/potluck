"""M17: what only the hub knows, in Home Assistant -- on the same devices the cell's presenting role creates.

In Home Assistant a device belongs to one config entry, and the same identifier from two config entries is two devices
(the ledger, 2026-10-14). The boards are the MQTT devices the cell's presenting role (M13) creates, live even with the
PC off; so the hub adds to them through MQTT discovery too, with the presenter's own device identifiers
(`potluck_<cell>_<board>`, `potluck_<cell>_cell`) and device blocks, never a second device:

  per board   state (alive / asleep / dead), internal RAM free, uptime, signal strength, worst heartbeat delivery --
              diagnostic sensors -- and its firmware as an update entity (installed: the board's; latest: the newest in
              the cell; no command topic, so Home Assistant offers no install: HA 2026.10.1's mqtt/update.py)
  the cell    apps working, and one problem binary sensor per app ("the room sensor is down")

All available while the hub is (`potluck/<cell>/hub`: `online`, its will `offline`). States every 10 s, configs at
each connect, at Home Assistant's birth message and every 5 minutes; nothing retained (section 13).
"""

from __future__ import annotations

import base64
import json
import re
import threading
import time

from . import mqtt_link

STATE_EVERY_S, CONFIG_EVERY_S = 10.0, 300.0


def _slug(s: str) -> str:
    return re.sub(r"[^A-Za-z0-9_-]", "_", s)[:48]


class Extras:
    def __init__(self, model, log=lambda m: None, login: tuple[str, str] | None = None) -> None:
        self.model = model
        self.log = log
        p = model.cluster.present
        host, _, port = str(p["broker"]).partition(":")
        self.cell = model.cell_id
        self.system = model.cluster.system
        tls = bool(p.get("tls_ca"))
        self.cfg = {"host": host, "port": int(port or (8883 if tls else 1883)), "tls": 1 if tls else 0,
                    "client_id": f"potluck-{self.cell}-hub", "keepalive_s": int(p.get("keepalive_s", 10)),
                    "will_topic": f"potluck/{self.cell}/hub",
                    "will_b64": base64.b64encode(b"offline").decode(),
                    "cmd_filter": f"potluck/{self.cell}/hub/+/set",  # nothing is writable here; the filter is required
                    "status_topic": "homeassistant/status",
                    "ca_b64": p.get("tls_ca") or ""}
        if login is not None:  # --broker-login: a broker that wants a user name and password
            self.cfg["user"], self.cfg["pass"] = login
        self.lock = threading.Lock()
        self.connected = False
        self.need_configs = False
        self.next_state = 0.0
        self.next_configs = 0.0
        self.known: set[str] = set()  # what configs have gone out for: a new board or app sends them again
        self.link = mqtt_link.PresenterLink(self._control, log)

    def start(self) -> None:
        self.link.start(self.cfg)

    def stop(self) -> None:
        self.link.stop()  # "offline" on the hub's availability, then a clean DISCONNECT

    def _control(self, text: str) -> None:
        with self.lock:
            if text == "connected 1":
                self.connected = True
                self.need_configs = True
                self.next_state = 0.0
            elif text == "connected 0":
                self.connected = False
            elif text == "discovery":
                self.need_configs = True  # Home Assistant restarted and forgot every entity

    def _pub(self, topic: str, obj) -> None:
        self.link.publish(topic, (obj if isinstance(obj, str) else json.dumps(obj)).encode())

    # ---- what goes out ----
    def _avail(self) -> list[dict]:
        return [{"topic": f"potluck/{self.cell}/hub", "payload_available": "online", "payload_not_available": "offline"}]

    def board_device(self, node: str) -> dict:
        # Byte for byte the presenter's device block (pot_present/present_core.cpp config_payload), so neither side
        # rewrites the device's name.
        return {"identifiers": [f"potluck_{self.cell}_{node}"], "name": f"{self.system} node-{node}",
                "manufacturer": "Potluck"}

    def cell_device(self) -> dict:
        return {"identifiers": [f"potluck_{self.cell}_cell"], "name": f"{self.system} cell", "manufacturer": "Potluck"}

    def configs(self, snap: dict) -> list[tuple[str, dict]]:
        out: list[tuple[str, dict]] = []
        base = f"potluck/{self.cell}/hub"
        for b in snap.get("boards", []):
            n = b["node"]
            st = f"{base}/{n}/state"
            common = {"state_topic": st, "availability": self._avail(), "device": self.board_device(n),
                      "entity_category": "diagnostic"}
            sensors = [
                ("state", {"name": "State", "device_class": "enum", "options": ["alive", "asleep", "dead", "unknown"],
                           "value_template": "{{ value_json.state }}"}),
                ("internal_free", {"name": "Internal RAM free", "device_class": "data_size", "unit_of_measurement": "B",
                                   "state_class": "measurement", "value_template": "{{ value_json.internal_free }}"}),
                ("uptime", {"name": "Uptime", "device_class": "duration", "unit_of_measurement": "s",
                            "state_class": "measurement", "value_template": "{{ value_json.up_s }}"}),
                ("rssi", {"name": "Signal", "device_class": "signal_strength", "unit_of_measurement": "dBm",
                          "state_class": "measurement", "value_template": "{{ value_json.rssi }}"}),
                ("delivery", {"name": "Worst link delivery", "unit_of_measurement": "%", "state_class": "measurement",
                              "value_template": "{{ value_json.delivery_pct }}"}),
            ]
            for kind, extra in sensors:
                obj = f"hub_{n}_{kind}"
                out.append((f"homeassistant/sensor/potluck_{self.cell}/{obj}/config",
                            {**common, **extra, "unique_id": f"potluck_{self.cell}_{obj}"}))
            obj = f"hub_{n}_firmware"
            out.append((f"homeassistant/update/potluck_{self.cell}/{obj}/config",
                        {"name": "Firmware", "unique_id": f"potluck_{self.cell}_{obj}",
                         "state_topic": f"{base}/{n}/firmware", "availability": self._avail(),
                         "device": self.board_device(n), "entity_category": "diagnostic", "title": "Potluck firmware"}))
        cst = f"{base}/cell/state"
        out.append((f"homeassistant/sensor/potluck_{self.cell}/hub_cell_apps_working/config",
                    {"name": "Apps working", "unique_id": f"potluck_{self.cell}_hub_cell_apps_working",
                     "state_topic": cst, "value_template": "{{ value_json.apps_working }}",
                     "json_attributes_topic": cst, "json_attributes_template": "{{ value_json.attrs | tojson }}",
                     "state_class": "measurement", "availability": self._avail(), "device": self.cell_device()}))
        for a in snap.get("apps", []):
            obj = f"hub_app_{_slug(a['name'])}"
            out.append((f"homeassistant/binary_sensor/potluck_{self.cell}/{obj}/config",
                        {"name": f"{a['name']} problem", "unique_id": f"potluck_{self.cell}_{obj}",
                         "device_class": "problem", "state_topic": cst,
                         "value_template": "{{ 'ON' if value_json.problems.get('" + a["name"].replace("'", "") +
                                           "') else 'OFF' }}",
                         "availability": self._avail(), "device": self.cell_device()}))
        return out

    def states(self, snap: dict) -> list[tuple[str, dict]]:
        out: list[tuple[str, dict]] = []
        base = f"potluck/{self.cell}/hub"
        boards = snap.get("boards", [])
        newest = max(boards, key=lambda b: ((b.get("firmware") or {}).get("counter") or 0), default=None)
        latest = ((newest or {}).get("firmware") or {}).get("version")
        for b in boards:
            n = b["node"]
            links = [k for k in (b.get("links") or []) if not k.get("cable") and k.get("delivery") is not None]
            worst = min((k["delivery"] for k in links), default=None)
            heap = b.get("heap") or {}
            out.append((f"{base}/{n}/state", {
                "state": b.get("state", "unknown"), "internal_free": heap.get("internal_free"),
                "up_s": (b.get("up_ms") or 0) // 1000 if b.get("up_ms") is not None else None,
                "rssi": b.get("rssi"), "delivery_pct": round(worst * 100, 2) if worst is not None else None}))
            installed = (b.get("firmware") or {}).get("version")
            if installed:
                out.append((f"{base}/{n}/firmware", {"installed_version": installed, "latest_version": latest or installed,
                                                     "title": "Potluck firmware"}))
        apps = snap.get("apps", [])
        not_working = [a["name"] for a in apps if not a.get("working")]
        out.append((f"{base}/cell/state", {
            "apps_working": sum(1 for a in apps if a.get("working")),
            "problems": {a["name"].replace("'", ""): not a.get("working") for a in apps},
            "attrs": {"apps_total": len(apps), "not_working": not_working,
                      "boards_alive": sum(1 for b in boards if b.get("state") == "alive"), "boards_total": len(boards)}}))
        return out

    def tick(self, snap: dict) -> None:
        """Once a pull period, from the hub's main loop."""
        now = time.monotonic()
        with self.lock:
            if not self.connected:
                return
            seen = {f"b:{b['node']}" for b in snap.get("boards", [])} | {f"a:{a['name']}" for a in snap.get("apps", [])}
            send_configs = self.need_configs or now >= self.next_configs or not seen <= self.known
            self.need_configs = False
        if send_configs:
            self._pub(self.cfg["will_topic"], "online")
            for topic, payload in self.configs(snap):
                self._pub(topic, payload)
            with self.lock:
                self.known = seen
                self.next_configs = now + CONFIG_EVERY_S
                self.next_state = now + 2.0  # time for Home Assistant to create the entities before their states
        if now >= self.next_state:
            self._pub(self.cfg["will_topic"], "online")
            for topic, payload in self.states(snap):
                self._pub(topic, payload)
            self.next_state = now + STATE_EVERY_S
