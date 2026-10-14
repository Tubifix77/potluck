"""The hub's link to the cell: on while the hub hears the cell through its cable."""

from __future__ import annotations

from typing import Any

from homeassistant.components.binary_sensor import BinarySensorDeviceClass, BinarySensorEntity
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from .coordinator import PotluckConfigEntry, PotluckCoordinator
from .entity import PotluckHubEntity


async def async_setup_entry(
    hass: HomeAssistant, entry: PotluckConfigEntry, async_add_entities: AddConfigEntryEntitiesCallback
) -> None:
    """Add the hub's link sensor."""
    async_add_entities([PotluckHubLink(entry.runtime_data)])


class PotluckHubLink(PotluckHubEntity, BinarySensorEntity):
    """`hub.link` is "up". The hub not answering Home Assistant at all makes the entity unavailable instead."""

    _attr_device_class = BinarySensorDeviceClass.CONNECTIVITY

    def __init__(self, coordinator: PotluckCoordinator) -> None:
        super().__init__(coordinator, "hub_link")

    @property
    def is_on(self) -> bool:
        return self.coordinator.data.summary["link"] == "up"

    @property
    def extra_state_attributes(self) -> dict[str, Any]:
        hub = self.coordinator.data.model.get("hub") or {}
        return {"link": hub.get("link"), "detail": hub.get("link_detail") or "", "port": hub.get("port")}
