"""A client for the Potluck hub's API version 1 (host/potluck/HUB-API.md), for the Home Assistant integration (M17).

Pure Python on aiohttp: nothing here imports Home Assistant, so it is tested on its own
(host/potluck/tests/test_ha_api.py, against tools/hub_mock.py and a pairing stub).

Trust: the hub's certificate is self-signed. The hub announces the SHA-256 of its certificate (DER) in its multicast
DNS record (`fp`), and every request pins it with `aiohttp.Fingerprint`, which aiohttp checks right after the TLS
handshake, before a request -- and so before the bearer token -- is sent. Plain HTTP (`tls=False`) exists only for
testing against the mock, which serves no TLS; aiohttp skips a fingerprint on a connection without TLS, so the
client refuses to combine the two.
"""

from __future__ import annotations

import asyncio
import hashlib
import ssl
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Any
from urllib.parse import quote

import aiohttp

API_VERSION = 1
HEX = set("0123456789abcdef")


class HubError(Exception):
    """The hub answered something this client cannot use, or not at all."""


class HubConnectionError(HubError):
    """The hub could not be reached (refused, timed out, reset)."""


class HubFingerprintError(HubError):
    """The certificate served is not the one pinned: not the hub that was paired, or its certificate changed."""


class HubAuthError(HubError):
    """The hub refused the token (401 or 403): revoked, or never approved."""


class HubBusyError(HubError):
    """The hub already holds the most pending pairing requests it keeps (429)."""


class PairingDenied(HubError):
    """The owner refused the pairing on the PC."""


class PairingExpired(HubError):
    """Nobody approved the pairing before it expired (or it was already fetched)."""


@dataclass(frozen=True)
class PairRequest:
    pair_id: str
    expires_in_s: int


@dataclass(frozen=True)
class PairResult:
    name: str
    right: str
    token: str

    def __repr__(self) -> str:  # never print the token
        return f"PairResult(name={self.name!r}, right={self.right!r}, token=<hidden>)"


def normalize_fingerprint(text: str | None) -> str:
    """64 lowercase hex digits from an announced or typed fingerprint (colons and spaces allowed); ValueError if not."""
    fp = (text or "").strip().lower().replace(":", "").replace(" ", "")
    if len(fp) != 64 or not set(fp) <= HEX:
        raise ValueError("a certificate fingerprint is 64 hex digits (SHA-256)")
    return fp


def normalize_cell(text: str | None) -> str:
    """The cell id: 8 lowercase hex digits; ValueError if not."""
    cell = (text or "").strip().lower()
    if len(cell) != 8 or not set(cell) <= HEX:
        raise ValueError("a cell id is 8 hex digits")
    return cell


def url_host(host: str) -> str:
    """A host as it goes in a URL: an IPv6 address in brackets."""
    return f"[{host}]" if ":" in host and not host.startswith("[") else host


def format_fingerprint(fp: str) -> str:
    """AB:CD:... -- how a person compares a fingerprint against the one the PC shows."""
    return ":".join(fp[i:i + 2] for i in range(0, len(fp), 2)).upper()


async def fetch_fingerprint(host: str, port: int, timeout: float = 10.0) -> str:
    """Connect once by TLS, without verifying, and return the SHA-256 of the certificate served (DER, hex).

    Only for the manual set-up, where no announcement says what to expect: the person confirms what this returns.
    """
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    try:
        reader, writer = await asyncio.wait_for(asyncio.open_connection(host, port, ssl=ctx), timeout)
    except (OSError, asyncio.TimeoutError, ssl.SSLError) as err:
        raise HubConnectionError(f"no TLS connection to {host}:{port}: {err}") from err
    try:
        sslobj = writer.get_extra_info("ssl_object")
        der = sslobj.getpeercert(binary_form=True) if sslobj is not None else None
    finally:
        writer.close()
        try:
            await writer.wait_closed()
        except (OSError, ssl.SSLError):
            pass
    if not der:
        raise HubError(f"{host}:{port} served no certificate")
    return hashlib.sha256(der).hexdigest()


class HubClient:
    """One hub, reached at host:port, its certificate pinned; with a token once paired."""

    def __init__(self, session: aiohttp.ClientSession, host: str, port: int, *, fingerprint: str | None = None,
                 token: str | None = None, tls: bool = True, timeout: float = 10.0) -> None:
        self._session = session
        self.host = host
        self.port = int(port)
        self._token = token
        self._timeout = aiohttp.ClientTimeout(total=timeout)
        if tls:
            if fingerprint is None:
                raise ValueError("TLS without a pinned fingerprint is refused: the hub's certificate is self-signed")
            self.fingerprint = normalize_fingerprint(fingerprint)
            self._ssl: Any = aiohttp.Fingerprint(bytes.fromhex(self.fingerprint))
        else:
            if fingerprint is not None:
                raise ValueError("a fingerprint means nothing without TLS")
            self.fingerprint = None
            self._ssl = True
        self.base_url = f"{'https' if tls else 'http'}://{url_host(host)}:{self.port}"

    def __repr__(self) -> str:
        return f"HubClient({self.base_url}, paired={self._token is not None})"

    def set_token(self, token: str | None) -> None:
        self._token = token

    async def _request(self, method: str, path: str, *, body: Any = None, auth: bool = True) -> tuple[int, Any]:
        headers = {"Accept": "application/json"}
        if method != "GET":
            headers["X-Potluck-Request"] = "1"  # every POST carries it (HUB-API.md, logins)
        if auth and self._token:
            headers["Authorization"] = f"Bearer {self._token}"
        try:
            async with self._session.request(method, self.base_url + path, json=body, headers=headers,
                                             ssl=self._ssl, timeout=self._timeout, allow_redirects=False) as resp:
                status = resp.status
                try:
                    data = await resp.json(content_type=None)
                except ValueError:
                    data = None
        except aiohttp.ServerFingerprintMismatch as err:
            raise HubFingerprintError(
                f"{self.host}:{self.port} served a certificate with SHA-256 {err.got.hex()}, "
                f"not the pinned {self.fingerprint}") from err
        except (aiohttp.ClientError, asyncio.TimeoutError) as err:
            raise HubConnectionError(f"{self.base_url}: {err or type(err).__name__}") from err
        return status, data

    async def model(self) -> dict:
        """GET /api/model: the whole model, one JSON object."""
        status, data = await self._request("GET", "/api/model")
        if status in (401, 403):
            raise HubAuthError(f"the hub refused the token ({status})")
        if status != 200 or not isinstance(data, dict):
            raise HubError(f"GET /api/model answered {status}")
        if data.get("api") != API_VERSION:
            raise HubError(f"the hub speaks API version {data.get('api')!r}; this integration knows {API_VERSION}")
        return data

    async def report(self, name: str, state: str, detail: str = "") -> bool:
        """POST /api/integrations (M18): tell the hub how this integration is, so its dashboard shows it. Never
        raises: a report that does not arrive only makes the hub show this integration silent after 60 s."""
        try:
            status, _ = await self._request("POST", "/api/integrations",
                                            body={"name": name, "state": state, "detail": detail})
        except HubError:
            return False
        return status == 200

    async def request_pairing(self, name: str, right: str = "look") -> PairRequest:
        """POST /api/pair: ask the owner for a token. No session or token needed."""
        status, data = await self._request("POST", "/api/pair", body={"name": name, "right": right}, auth=False)
        if status == 429:
            raise HubBusyError("the hub already holds the most pending pairing requests it keeps")
        if status == 404:
            raise HubError("this hub offers no pairing (an older hub, or not started with --listen)")
        if status not in (200, 201, 202) or not isinstance(data, dict) or not data.get("pair_id"):
            raise HubError(f"POST /api/pair answered {status}")
        return PairRequest(str(data["pair_id"]), int(data.get("expires_in_s") or 300))

    async def pairing_status(self, pair_id: str) -> dict:
        """GET /api/pair/<id>: {"state": "pending" | "approved" | "denied" | "expired", ...}; 404 is expired."""
        status, data = await self._request("GET", "/api/pair/" + quote(pair_id, safe=""), auth=False)
        if status == 404:
            return {"state": "expired"}
        if status != 200 or not isinstance(data, dict) or "state" not in data:
            raise HubError(f"GET /api/pair answered {status}")
        return data

    async def wait_for_pairing(self, request: PairRequest, *, interval: float = 2.0) -> PairResult:
        """Poll until the owner answers. A dropped connection is retried until the request's own expiry passes."""
        loop = asyncio.get_running_loop()
        deadline = loop.time() + request.expires_in_s + 10
        while True:
            try:
                st = await self.pairing_status(request.pair_id)
            except HubConnectionError:
                st = {"state": "pending"}
            state = st.get("state")
            if state == "approved":
                token = st.get("token")
                if not isinstance(token, str) or not token:
                    raise HubError("the hub approved the pairing but sent no token")
                return PairResult(str(st.get("name") or ""), str(st.get("right") or ""), token)
            if state == "denied":
                raise PairingDenied("the owner refused the pairing on the PC")
            if state == "expired":
                raise PairingExpired("nobody approved the pairing in time")
            if state != "pending":
                raise HubError(f"unknown pairing state {state!r}")
            if loop.time() >= deadline:
                raise PairingExpired("nobody approved the pairing in time")
            await asyncio.sleep(interval)


# ---- what the integration shows, from the model -----------------------------------------------------------------

def board_name(board: dict) -> str:
    node = str(board.get("node", "?"))
    return f"{board['label']} {node}" if board.get("label") else node


def at_to_datetime(at: Any) -> datetime | None:
    """The hub's clock (ms since 1970) as an aware datetime."""
    if not isinstance(at, (int, float)) or isinstance(at, bool):
        return None
    return datetime.fromtimestamp(at / 1000.0, tz=timezone.utc)


def last_deploy(model: dict) -> dict | None:
    deploys = [d for d in (model.get("deploys") or []) if isinstance(d, dict) and isinstance(d.get("at"), (int, float))]
    return max(deploys, key=lambda d: d["at"]) if deploys else None


def summarize(model: dict) -> dict:
    """The numbers the hub's entities show: boards, apps, the last deploy, the hub's link."""
    boards = [b for b in (model.get("boards") or []) if isinstance(b, dict)]
    apps = [a for a in (model.get("apps") or []) if isinstance(a, dict)]
    by_state: dict[str, list[str]] = {}
    for b in boards:
        by_state.setdefault(str(b.get("state")), []).append(board_name(b))
    hub = model.get("hub") or {}
    cell = model.get("cell") or {}
    return {
        "boards_total": len(boards),
        "boards_alive": len(by_state.get("alive", [])),
        "boards_asleep": by_state.get("asleep", []),
        "boards_dead": by_state.get("dead", []),
        "boards_unknown": by_state.get("unknown", []),
        "apps_total": len(apps),
        "apps_working": sum(1 for a in apps if a.get("working") is True),
        # working null: the hub is starting and has not seen the app working yet -- not a fault
        "apps_not_working": [{"name": a.get("name"), "detail": a.get("detail") or ""}
                             for a in apps if a.get("working") is False],
        "starting": bool(hub.get("starting")),
        "last_deploy": last_deploy(model),
        "link": hub.get("link"),
        "link_detail": hub.get("link_detail") or "",
        "hub_version": hub.get("version"),
        "system": cell.get("system"),
        "cell_id": cell.get("cell_id"),
    }


def problems(model: dict) -> list[dict]:
    """The cell's problems, as the dashboard's first screen lists them (host/potluck/potluck/dashboard/app.js).

    Each is {"severity": "problem" | "warning", "text": ..., "since": <ISO time or None>}, problems first. The text
    carries no ticking age, so it changes only when the problem does.
    """
    out: list[dict] = []

    def add(sev: str, text: str, since_at: Any = None) -> None:
        dt = at_to_datetime(since_at)
        if dt is not None:  # whole seconds: an age that jitters by a few ms would rewrite the attribute every poll
            dt = dt.replace(microsecond=0)
        out.append({"severity": sev, "text": text, "since": dt.isoformat() if dt else None})

    boards = {str(b.get("node")): b for b in (model.get("boards") or []) if isinstance(b, dict)}
    hub = model.get("hub") or {}
    starting = bool(hub.get("starting"))  # the hub's first look at the cell: nothing is a fault for lack of a look
    if hub and hub.get("link") != "up":
        add("problem", f"The hub's link to the cell is {hub.get('link')}"
            + (f": {hub['link_detail']}" if hub.get("link_detail") else "") + "; board states are as last heard")
    for b in boards.values():
        name = board_name(b)
        state = b.get("state")
        if state == "dead":
            add("problem", f"Board {name} is dead", b.get("since_at"))
        elif state == "unknown" and not starting:
            add("warning", f"Board {name} has not been heard since the hub started")
        for part in ("firmware", "package"):
            p = b.get(part) or {}
            if p.get("state") and p.get("state") != "confirmed" and state != "dead":
                add("warning", f"Board {name}: {part} {p.get('counter')} {p.get('state')}")
        for md in b.get("modules") or []:
            if md.get("state") == "failed":
                add("problem", f"Module {md.get('name')} failed on {name}"
                    + (f": {md['reason']}" if md.get("reason") else ""))
    res = {r.get("path"): r for r in (model.get("resources") or []) if isinstance(r, dict)}
    shown: set = set()
    at = model.get("at")
    for a in model.get("apps") or []:
        outs = [res[p] for p in (a.get("outputs") or []) if p in res]
        # the settled quality (it held for two reads); STARTING: not read twice yet since the hub started
        bad = [r for r in outs if r.get("settled_quality", r.get("quality")) not in ("GOOD", "STARTING")
               and r.get("path") not in shown
               and not (boards.get(str(r.get("owner")), {}).get("state") == "dead")]  # the dead board is the headline
        running = a.get("running_on") or []
        placed = (len(running) == 1 if a.get("placement") == "portable"
                  else all(n in running for n in (a.get("nodes") or [])))
        if a.get("working") is False and placed and bad:
            r = bad[0]
            shown.add(r.get("path"))
            since = at - r["age_ms"] if isinstance(at, int) and isinstance(r.get("age_ms"), int) else None
            add("problem", f"App {a.get('name')} not working: output {r.get('name')} {r.get('quality')}", since)
        elif a.get("working") is False:
            add("problem", f"App {a.get('name')} not working" + (f": {a['detail']}" if a.get("detail") else ""))
        for r in bad:
            if r.get("path") in shown:
                continue
            shown.add(r.get("path"))
            since = at - r["age_ms"] if isinstance(at, int) and isinstance(r.get("age_ms"), int) else None
            add("warning" if r.get("quality") == "STALE" else "problem",
                f"Output {r.get('name')} {r.get('quality')}", since)
    for i in model.get("integrations") or []:
        if i.get("state") == "disconnected":
            add("problem", f"Integration {i.get('name')} disconnected" + (f": {i['detail']}" if i.get("detail") else ""),
                i.get("since_at"))
    d = last_deploy(model)
    if d and d.get("result") in ("reverted", "refused", "failed"):
        add("problem", f"Last deploy ({d.get('kind')} {d.get('counter')}) {d.get('result')}"
            + (f": {d['detail']}" if d.get("detail") else ""), d.get("at"))
    elif d and d.get("result") == "running":
        add("warning", f"Deploy in progress ({d.get('kind')} {d.get('counter')})", d.get("at"))
    cell = model.get("cell") or {}
    if (cell.get("other_cell_frames") or 0) > 0:
        add("warning", f"Another cell shares channel {cell.get('channel')}")
    out.sort(key=lambda p: 0 if p["severity"] == "problem" else 1)
    return out
