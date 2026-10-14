"""Constants for the Potluck integration (M17)."""

from __future__ import annotations

from datetime import timedelta
from typing import Final

DOMAIN: Final = "potluck"

# Config entry data, besides Home Assistant's own CONF_HOST, CONF_PORT and CONF_TOKEN.
CONF_FINGERPRINT: Final = "fingerprint"  # SHA-256 of the hub's certificate (DER), 64 lowercase hex digits
CONF_CELL: Final = "cell"  # the cell id, 8 hex digits: the entry's unique id
CONF_SYSTEM: Final = "system"  # the cluster's name, e.g. "lab"
CONF_RIGHT: Final = "right"  # the right the owner approved: "look" (this integration asks for no more)

DEFAULT_PORT: Final = 8443  # the --listen port HUB-API.md's example uses; the announcement carries the real one

PAIR_NAME: Final = "Home Assistant"  # what the owner sees on the PC (HUB-API.md, "Announced on the network, and paired")
PAIR_RIGHT: Final = "look"
PAIR_POLL_S: Final = 2.0

UPDATE_INTERVAL: Final = timedelta(seconds=10)

MANUFACTURER: Final = "Potluck"
DASHBOARD_ON_PC: Final = "http://127.0.0.1:8470/"  # the hub's own address on the PC, where pairing is approved
