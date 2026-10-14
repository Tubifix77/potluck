"""Set up a Potluck hub: found by multicast DNS, confirmed, then paired with the owner's approval on the PC.

Trust, step by step (host/potluck/HUB-API.md, "Announced on the network, and paired"):

- The announcement carries the SHA-256 of the hub's certificate (`fp`). Every request pins it, so the token is only
  ever sent to the certificate that was paired.
- A later announcement may move the entry's host and port (a new DHCP address), never its fingerprint: an
  announcement is unauthenticated, and following its `fp` would hand the token to anyone on the network who announces
  the same cell id. A hub whose certificate really changed fails its next pull; Home Assistant then asks to
  re-authenticate, which shows the new fingerprint to confirm and asks the owner to approve a new pairing.
- Set up by hand (discovery does not reach), the fingerprint comes from a first TLS connection and the person confirms
  it before anything is sent.
"""

from __future__ import annotations

import asyncio
from collections.abc import Mapping
from ipaddress import IPv4Address
import logging
from typing import Any

from homeassistant.config_entries import SOURCE_REAUTH, ConfigFlow, ConfigFlowResult
from homeassistant.const import CONF_HOST, CONF_PORT, CONF_TOKEN
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.service_info.zeroconf import ZeroconfServiceInfo

try:  # Home Assistant 2026.10 validates flow forms with probatio, a drop-in for voluptuous
    import probatio as vol
except ImportError:  # pragma: no cover - an older Home Assistant
    import voluptuous as vol

from .api import (
    HubBusyError,
    HubClient,
    HubConnectionError,
    HubError,
    HubFingerprintError,
    PairingDenied,
    PairingExpired,
    PairResult,
    fetch_fingerprint,
    format_fingerprint,
    normalize_cell,
    normalize_fingerprint,
)
from .const import (
    CONF_CELL,
    CONF_FINGERPRINT,
    CONF_RIGHT,
    CONF_SYSTEM,
    DASHBOARD_ON_PC,
    DEFAULT_PORT,
    DOMAIN,
    PAIR_NAME,
    PAIR_POLL_S,
    PAIR_RIGHT,
)

_LOGGER = logging.getLogger(__name__)


def _pick_host(info: ZeroconfServiceInfo) -> str:
    """The announced address to use: IPv4 when there is one (the hub's --listen is usually 0.0.0.0)."""
    if isinstance(info.ip_address, IPv4Address):
        return str(info.ip_address)
    for ip in info.ip_addresses:
        if isinstance(ip, IPv4Address):
            return str(ip)
    return str(info.ip_address)


class PotluckConfigFlow(ConfigFlow, domain=DOMAIN):
    """Discover, confirm, pair."""

    VERSION = 1

    def __init__(self) -> None:
        self._host: str | None = None
        self._port: int | None = None
        self._fp: str | None = None
        self._cell: str | None = None  # from the announcement; learnt from the model after pairing otherwise
        self._system: str | None = None
        self._pair_task: asyncio.Task[PairResult] | None = None
        self._pair: PairResult | None = None
        self._pair_error: str = "pairing_failed"
        self._pair_detail: str = ""

    def _client(self, token: str | None = None) -> HubClient:
        assert self._host is not None and self._port is not None and self._fp is not None
        return HubClient(async_get_clientsession(self.hass), self._host, self._port, fingerprint=self._fp,
                         token=token)

    def _placeholders(self) -> dict[str, str]:
        return {
            "system": self._system or "?",
            "cell": self._cell or "?",
            "address": f"{self._host}:{self._port}",
            "fingerprint": format_fingerprint(self._fp) if self._fp else "?",
            "name": PAIR_NAME,
            "dashboard": DASHBOARD_ON_PC,
        }

    # ---- discovered ---------------------------------------------------------------------------------------------

    async def async_step_zeroconf(self, discovery_info: ZeroconfServiceInfo) -> ConfigFlowResult:
        """The hub announced itself: _potluck._tcp.local. with TXT cell, system, api, tls, fp."""
        props = discovery_info.properties
        try:
            cell = normalize_cell(props.get("cell"))
            fp = normalize_fingerprint(props.get("fp"))
        except ValueError:
            return self.async_abort(reason="invalid_discovery")
        if props.get("api") != "1":
            return self.async_abort(reason="unsupported_api")
        if props.get("tls") != "1":
            return self.async_abort(reason="no_tls")
        if not discovery_info.port:
            return self.async_abort(reason="invalid_discovery")
        host = _pick_host(discovery_info)

        await self.async_set_unique_id(cell)
        # Host and port follow the announcement; the fingerprint does not (see the module's docstring).
        self._abort_if_unique_id_configured(updates={CONF_HOST: host, CONF_PORT: discovery_info.port})

        self._host, self._port, self._fp, self._cell = host, discovery_info.port, fp, cell
        self._system = props.get("system") or cell
        self.context["title_placeholders"] = {"name": f"{self._system} ({cell})"}
        self._set_confirm_only()
        return await self.async_step_confirm()

    async def async_step_confirm(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        """Ask before pairing: a pairing puts a request on the owner's PC."""
        if user_input is not None:
            return await self.async_step_pair()
        return self.async_show_form(step_id="confirm", description_placeholders=self._placeholders())

    # ---- by hand ------------------------------------------------------------------------------------------------

    async def async_step_user(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        """The hub's address typed in, for when its announcement does not reach Home Assistant."""
        errors: dict[str, str] = {}
        if user_input is not None:
            host = str(user_input[CONF_HOST]).strip().strip("[]")
            port = int(user_input[CONF_PORT])
            try:
                fp = await fetch_fingerprint(host, port)
            except HubError as err:
                _LOGGER.debug("No TLS connection to %s:%s: %s", host, port, err)
                errors["base"] = "cannot_connect"
            else:
                self._host, self._port, self._fp = host, port, fp
                return await self.async_step_fingerprint()
        schema = vol.Schema({
            vol.Required(CONF_HOST, default=(user_input or {}).get(CONF_HOST, "")): str,
            vol.Required(CONF_PORT, default=(user_input or {}).get(CONF_PORT, DEFAULT_PORT)):
                vol.All(vol.Coerce(int), vol.Range(min=1, max=65535)),
        })
        return self.async_show_form(step_id="user", data_schema=schema, errors=errors)

    async def async_step_fingerprint(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        """Show the certificate's fingerprint and ask the person to confirm it is the hub's."""
        if user_input is not None:
            return await self.async_step_pair()
        return self.async_show_form(step_id="fingerprint", description_placeholders=self._placeholders())

    # ---- the token was revoked, or the certificate changed ------------------------------------------------------

    async def async_step_reauth(self, entry_data: Mapping[str, Any]) -> ConfigFlowResult:
        """The hub refused the token, or served another certificate: pair again."""
        self._host = entry_data[CONF_HOST]
        self._port = entry_data[CONF_PORT]
        self._cell = entry_data[CONF_CELL]
        self._system = entry_data.get(CONF_SYSTEM)
        self._fp = entry_data[CONF_FINGERPRINT]
        return await self.async_step_reauth_confirm()

    async def async_step_reauth_confirm(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        errors: dict[str, str] = {}
        if user_input is not None:
            assert self._host is not None and self._port is not None
            try:
                fp = await fetch_fingerprint(self._host, self._port)
            except HubError:
                errors["base"] = "cannot_connect"
            else:
                if fp == self._fp:  # the same certificate: only the token is new
                    return await self.async_step_pair()
                self._fp = fp
                return await self.async_step_fingerprint()
        return self.async_show_form(step_id="reauth_confirm", description_placeholders=self._placeholders(),
                                    errors=errors)

    # ---- pairing ------------------------------------------------------------------------------------------------

    async def _async_pair(self) -> PairResult:
        client = self._client()
        request = await client.request_pairing(PAIR_NAME, PAIR_RIGHT)
        return await client.wait_for_pairing(request, interval=PAIR_POLL_S)

    async def async_step_pair(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        """Wait while the owner approves the request on the PC's dashboard."""
        if self._pair_task is None:
            self._pair_task = self.hass.async_create_task(self._async_pair(), name="potluck pairing")
        if not self._pair_task.done():
            return self.async_show_progress(
                step_id="pair",
                progress_action="wait_for_approval",
                description_placeholders=self._placeholders(),
                progress_task=self._pair_task,
            )
        task, self._pair_task = self._pair_task, None
        try:
            self._pair = task.result()
        except PairingDenied:
            self._pair_error = "pairing_denied"
        except PairingExpired:
            self._pair_error = "pairing_expired"
        except HubBusyError:
            self._pair_error = "pairing_busy"
        except HubFingerprintError:
            self._pair_error = "fingerprint_mismatch"
        except HubConnectionError as err:
            self._pair_error, self._pair_detail = "cannot_connect", str(err)
        except HubError as err:
            self._pair_error, self._pair_detail = "pairing_failed", str(err)
        else:
            return self.async_show_progress_done(next_step_id="finish")
        return self.async_show_progress_done(next_step_id="pair_failed")

    async def async_step_pair_failed(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        return self.async_abort(reason=self._pair_error,
                                description_placeholders={**self._placeholders(), "error": self._pair_detail})

    async def async_step_finish(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        """Paired: read the model once with the token, check it is the cell expected, and keep the entry."""
        assert self._pair is not None
        token = self._pair.token
        try:
            model = await self._client(token).model()
            cell = normalize_cell((model.get("cell") or {}).get("cell_id"))
        except (HubError, ValueError) as err:
            _LOGGER.warning("Paired with %s:%s, but its model could not be read: %s", self._host, self._port, err)
            return self.async_abort(reason="cannot_connect", description_placeholders={"error": str(err)})
        system = str((model.get("cell") or {}).get("system") or self._system or cell)
        data = {
            CONF_HOST: self._host,
            CONF_PORT: self._port,
            CONF_FINGERPRINT: self._fp,
            CONF_TOKEN: token,
            CONF_CELL: cell,
            CONF_SYSTEM: system,
            CONF_RIGHT: self._pair.right or PAIR_RIGHT,
        }
        if self.source == SOURCE_REAUTH:
            entry = self._get_reauth_entry()
            if cell != entry.unique_id:
                return self.async_abort(reason="wrong_cell")
            return self.async_update_reload_and_abort(entry, data_updates=data)
        if self._cell is not None and cell != self._cell:
            return self.async_abort(reason="wrong_cell")
        await self.async_set_unique_id(cell, raise_on_progress=False)
        # Set up by hand for a cell that is already here: the person confirmed this certificate and the owner
        # approved this pairing, so the entry takes both.
        self._abort_if_unique_id_configured(updates=data)
        return self.async_create_entry(title=f"Potluck {system}", data=data)
