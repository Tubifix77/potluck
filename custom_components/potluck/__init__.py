"""Potluck for Home Assistant (M17): the hub's own health, found on the network and paired with the owner's approval.

The boards and their resources are not created here. They are the MQTT devices the cell's presenting role creates
(identifiers potluck_<cell>_<board> and potluck_<cell>_cell), live with or without the PC, and the hub adds its
diagnostics and firmware entities to those same devices over MQTT. In Home Assistant a device belongs to one config
entry, so this integration making its own board devices would show every board twice. It makes one service device:
the hub.
"""

from __future__ import annotations

from homeassistant.const import CONF_HOST, CONF_PORT, CONF_TOKEN, Platform
from homeassistant.core import HomeAssistant
from homeassistant.helpers.aiohttp_client import async_get_clientsession

from .api import HubClient
from .const import CONF_FINGERPRINT
from .coordinator import PotluckConfigEntry, PotluckCoordinator

PLATFORMS: list[Platform] = [Platform.BINARY_SENSOR, Platform.SENSOR]


async def async_setup_entry(hass: HomeAssistant, entry: PotluckConfigEntry) -> bool:
    """Set up the hub from a config entry."""
    client = HubClient(
        async_get_clientsession(hass),
        entry.data[CONF_HOST],
        entry.data[CONF_PORT],
        fingerprint=entry.data[CONF_FINGERPRINT],
        token=entry.data[CONF_TOKEN],
    )
    coordinator = PotluckCoordinator(hass, entry, client)
    await coordinator.async_config_entry_first_refresh()
    entry.runtime_data = coordinator
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    return True


async def async_unload_entry(hass: HomeAssistant, entry: PotluckConfigEntry) -> bool:
    """Unload a config entry."""
    return await hass.config_entries.async_unload_platforms(entry, PLATFORMS)
