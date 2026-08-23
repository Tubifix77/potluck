"""potluck-capture -- M0 host tooling for Potluck.

Reads a node's serial stream, decodes Potluck Frames, records them in
ARCHITECTURE.md section 7.6's capture format, and reports the PDR and delay
histogram that section 13-M0's acceptance test asks for.

Two tools live here:

  potluck-capture  reads a node's *console* -- its JSON statistics lines -- and summarises a soak.
  potctl      talks the *frame link* (section 5.3's COBS/CRC UART), driving a `Bridge`, and reads
               and writes the namespace. This is M2's `potluck-bridge`, as a library plus a CLI.

The bridge is what makes the host an ordinary peer rather than a console: it has a MAC, a node id, it
says HELLO to be admitted and it heartbeats. Section 8.1 says a mode transition is a non-event, which
only holds if the firmware never needs to know a frame came from a host.

The frame decoder in `frame.py` is a second implementation of the codec in
firmware/components/pot_frame, written from ARCHITECTURE.md section 5 rather than
from the C++. Two independent implementations checked against the same golden
bytes is what makes the wire format tested rather than merely self-consistent. `serial_framing.py`
and `ns_payloads.py` exist for the same reason, and `fake_node.py` is the known far end that lets the
bridge be tested with no hardware and no emulator.
"""

from __future__ import annotations

__all__ = [
    "bridge",
    "capture",
    "ctl",
    "fake_node",
    "frame",
    "link_stats",
    "live",
    "locality",
    "manifest",
    "ns_payloads",
    "payloads",
    "records",
    "serial_framing",
    "source",
    "sys_paths",
    "transport",
    "value",
]

__version__ = "0.1.0"  # M0
