"""The hub's sensors: boards alive, apps working, the last deploy, and the cell's problems."""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass
from datetime import datetime
from typing import Any

from homeassistant.components.sensor import (
    SensorDeviceClass,
    SensorEntity,
    SensorEntityDescription,
    SensorStateClass,
)
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from .api import at_to_datetime
from .coordinator import HubSnapshot, PotluckConfigEntry, PotluckCoordinator
from .entity import PotluckHubEntity


@dataclass(frozen=True, kw_only=True)
class PotluckSensorDescription(SensorEntityDescription):
    """A sensor read from one pull of the model."""

    value: Callable[[HubSnapshot], Any]
    attributes: Callable[[HubSnapshot], dict[str, Any]]


def _deploy_time(s: HubSnapshot) -> datetime | None:
    d = s.summary["last_deploy"]
    return at_to_datetime(d.get("at")) if d else None


def _deploy_attrs(s: HubSnapshot) -> dict[str, Any]:
    d = s.summary["last_deploy"]
    if not d:
        return {}
    return {"kind": d.get("kind"), "counter": d.get("counter"), "result": d.get("result"), "detail": d.get("detail")}


SENSORS: tuple[PotluckSensorDescription, ...] = (
    PotluckSensorDescription(
        key="boards_alive",
        state_class=SensorStateClass.MEASUREMENT,
        value=lambda s: s.summary["boards_alive"],
        attributes=lambda s: {
            "boards_total": s.summary["boards_total"],
            "asleep": s.summary["boards_asleep"],
            "dead": s.summary["boards_dead"],
            "unknown": s.summary["boards_unknown"],
        },
    ),
    PotluckSensorDescription(
        key="apps_working",
        state_class=SensorStateClass.MEASUREMENT,
        value=lambda s: s.summary["apps_working"],
        attributes=lambda s: {"apps_total": s.summary["apps_total"], "not_working": s.summary["apps_not_working"]},
    ),
    PotluckSensorDescription(
        key="last_deploy",
        device_class=SensorDeviceClass.TIMESTAMP,
        value=_deploy_time,
        attributes=_deploy_attrs,
    ),
    PotluckSensorDescription(
        key="problems",
        state_class=SensorStateClass.MEASUREMENT,
        value=lambda s: sum(1 for p in s.problems if p["severity"] == "problem"),
        attributes=lambda s: {
            "warnings": sum(1 for p in s.problems if p["severity"] == "warning"),
            "problems": s.problems,
        },
    ),
)


async def async_setup_entry(
    hass: HomeAssistant, entry: PotluckConfigEntry, async_add_entities: AddConfigEntryEntitiesCallback
) -> None:
    """Add the hub's sensors."""
    coordinator = entry.runtime_data
    async_add_entities(PotluckSensor(coordinator, d) for d in SENSORS)


class PotluckSensor(PotluckHubEntity, SensorEntity):
    """One of the hub's sensors."""

    entity_description: PotluckSensorDescription

    def __init__(self, coordinator: PotluckCoordinator, description: PotluckSensorDescription) -> None:
        super().__init__(coordinator, description.key)
        self.entity_description = description

    @property
    def native_value(self) -> Any:
        return self.entity_description.value(self.coordinator.data)

    @property
    def extra_state_attributes(self) -> dict[str, Any]:
        return self.entity_description.attributes(self.coordinator.data)
