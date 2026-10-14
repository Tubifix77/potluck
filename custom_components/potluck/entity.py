"""The hub's service device, shared by its entities."""

from __future__ import annotations

from homeassistant.const import CONF_HOST, CONF_PORT
from homeassistant.helpers.device_registry import DeviceEntryType, DeviceInfo
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .api import url_host
from .const import CONF_CELL, CONF_SYSTEM, DOMAIN, MANUFACTURER
from .coordinator import PotluckCoordinator


class PotluckHubEntity(CoordinatorEntity[PotluckCoordinator]):
    """An entity on the hub's device: identifiers {(potluck, <cell id>)}, a service, not a board."""

    _attr_has_entity_name = True

    def __init__(self, coordinator: PotluckCoordinator, key: str) -> None:
        super().__init__(coordinator)
        data = coordinator.config_entry.data
        cell = data[CONF_CELL]
        self._attr_translation_key = key
        self._attr_unique_id = f"{cell}_{key}"
        self._attr_device_info = DeviceInfo(
            identifiers={(DOMAIN, cell)},
            name=f"Potluck hub ({data[CONF_SYSTEM]})",
            entry_type=DeviceEntryType.SERVICE,
            manufacturer=MANUFACTURER,
            model="Hub",
            sw_version=(coordinator.data.summary.get("hub_version") if coordinator.data else None),
            configuration_url=f"https://{url_host(data[CONF_HOST])}:{data[CONF_PORT]}/",
        )
