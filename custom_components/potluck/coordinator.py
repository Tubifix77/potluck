"""Polls the hub's model every 10 s (GET /api/model) for the hub's own entities."""

from __future__ import annotations

from dataclasses import dataclass
import logging

from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import ConfigEntryAuthFailed
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, UpdateFailed

from .api import HubAuthError, HubClient, HubError, HubFingerprintError, problems, summarize
from .const import DOMAIN, UPDATE_INTERVAL

_LOGGER = logging.getLogger(__name__)


@dataclass
class HubSnapshot:
    """One pull: the model as the hub sent it, and what the entities show from it."""

    model: dict
    summary: dict
    problems: list[dict]


type PotluckConfigEntry = ConfigEntry[PotluckCoordinator]


class PotluckCoordinator(DataUpdateCoordinator[HubSnapshot]):
    """The hub's model, pulled on a fixed period."""

    config_entry: PotluckConfigEntry

    def __init__(self, hass: HomeAssistant, entry: PotluckConfigEntry, client: HubClient) -> None:
        super().__init__(
            hass,
            _LOGGER,
            config_entry=entry,
            name=f"{DOMAIN} {entry.unique_id}",
            update_interval=UPDATE_INTERVAL,
        )
        self.client = client

    async def _async_update_data(self) -> HubSnapshot:
        try:
            model = await self.client.model()
        except HubAuthError as err:
            # The token was revoked (python -m potluck.hubauth revoke): pair again.
            raise ConfigEntryAuthFailed(str(err)) from err
        except HubFingerprintError as err:
            # Not the certificate that was paired: never send the token to it. Pairing again shows the person
            # the new fingerprint to confirm, and the owner approves on the PC.
            raise ConfigEntryAuthFailed(str(err)) from err
        except HubError as err:
            raise UpdateFailed(str(err)) from err
        return HubSnapshot(model=model, summary=summarize(model), problems=problems(model))
