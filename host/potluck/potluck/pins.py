"""The GPIO pins a firmware build uses, from that build's own configuration (config/sdkconfig.json).

Asked for by the Potluck Desk (2026-10-20) for its Wiring window, so it reads what a board's build claims instead of
carrying a copy of the bench datasheet. Only what this firmware itself drives is named here: the frame link to the PC,
CAN when built in, the status LED (and the other board revision's LED pin) and the leave button. What the module and
board reserve on their own -- flash, PSRAM, USB -- is the board's datasheet's to say, not a build's.

    {"frame_link": {"tx": 17, "rx": 18} | null, "can": {"tx": 4, "rx": 5} | null,
     "status_led": {"gpio": 48, "alt": 38}, "button": 0}
"""

from __future__ import annotations

import json
from pathlib import Path


def from_sdkconfig(cfg: dict) -> dict:
    """The pin map from a parsed sdkconfig.json (ESP-IDF's build/config/sdkconfig.json: names without CONFIG_)."""
    def gpio(name: str):
        v = cfg.get(name)
        return v if isinstance(v, int) and not isinstance(v, bool) else None

    link = {"tx": gpio("POT_SERIAL_TX_GPIO"), "rx": gpio("POT_SERIAL_RX_GPIO")} if cfg.get("POT_SERIAL_LINK") else None
    can = {"tx": gpio("POT_CAN_TX_GPIO"), "rx": gpio("POT_CAN_RX_GPIO")} if cfg.get("POT_CAN") else None
    led = gpio("POT_STATUS_LED_GPIO")
    return {"frame_link": link, "can": can,
            "status_led": {"gpio": led, "alt": gpio("POT_STATUS_LED_GPIO_ALT")} if led is not None else None,
            "button": gpio("POT_BYE_BUTTON_GPIO")}


def from_build(build_dir: str | Path) -> dict | None:
    """The pin map of an ESP-IDF build folder, or None when it has no config/sdkconfig.json."""
    try:
        cfg = json.loads((Path(build_dir) / "config" / "sdkconfig.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    return from_sdkconfig(cfg) if isinstance(cfg, dict) else None


def used(pins: dict | None) -> set[int]:
    """Every GPIO the map names: what an app's parts may not be wired to."""
    out: set[int] = set()
    if not pins:
        return out
    for v in pins.values():
        if isinstance(v, int):
            out.add(v)
        elif isinstance(v, dict):
            out |= {x for x in v.values() if isinstance(x, int)}
    return out


def for_version(version: str | None, cache: Path | None = None) -> dict | None:
    """The pin map of firmware `version` ("<commit>-<key>"): from the release the installed package carries (its
    release.json "pins"), else from that build in apply's cache. None when neither has it."""
    if not version:
        return None
    from .setup import packaged_firmware
    root = packaged_firmware()
    if root is not None:
        try:
            meta = json.loads((root / version / "release.json").read_text(encoding="utf-8"))
            if isinstance(meta.get("pins"), dict):
                return meta["pins"]
        except (OSError, ValueError):
            pass
    key = version.rsplit("-", 1)[-1]
    if cache is None:
        from .apply import default_cache
        cache = default_cache()
    for d in sorted((cache / "fw").glob(f"{key}*/build-*")) if key else []:
        got = from_build(d)
        if got is not None:
            return got
    return None
