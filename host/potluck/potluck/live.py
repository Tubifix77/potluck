"""Live PDR and RTT display, and the end-of-run summary.

What this refuses to do is as important as what it does. It never prints a
one-way delay, because there is no measurement of one -- see
firmware/components/pot_link/include/pot/link_stats.hpp for the full argument.
It never prints a percentile as a single number, because the node measures a
histogram and a histogram knows an interval. And it never prints 0 for something
unmeasured: an unmeasured PDR shows as "--".

The summary at the end is the thing the M0 acceptance check reads (section 13-M0:
"a published delay histogram and PDR figure measured on your bench, not cited from
this document"), so it prints the histogram as counts per bucket rather than as a
sparkline that would look decisive and say less.
"""

from __future__ import annotations

import hashlib
import json
import sys
import time
from dataclasses import dataclass, field
from typing import Any

from .link_stats import RTT_BUCKET_EDGE_US, describe_bucket
from .records import ppm_to_percent
from .sys_paths import describe, sys_path_for_hash


def _fmt_pct(value: float | None) -> str:
    return "  --  " if value is None else f"{value:6.2f}"


def _fmt_us(value: int | None) -> str:
    if value is None:
        return "    --"
    if value < 10_000:
        return f"{value:5d}u"
    return f"{value / 1000:5.1f}m"


def _fmt_interval(pair: list[Any] | None) -> str:
    """Render a percentile interval the way the node measured it."""
    if not pair or len(pair) != 2:
        return "        --"
    lo, hi = pair
    if hi is None:
        return f">{lo / 1000:.0f}ms"
    return f"{lo / 1000:.0f}-{hi / 1000:.0f}ms"


@dataclass(slots=True)
class PeerView:
    """The most recent link record for one (node, peer) pair, plus derived views."""

    node: int
    peer: int
    mac: str = ""
    state: str = "?"
    espnow_ver: int = 0
    mtu: int = 0
    misses: int = 0
    rssi: int = 0
    epoch: int = 0
    tx: dict[str, Any] = field(default_factory=dict)
    rx: dict[str, Any] = field(default_factory=dict)
    rtt: dict[str, Any] = field(default_factory=dict)
    last_seen: float = 0.0

    @property
    def histogram(self) -> list[int]:
        h = self.rtt.get("hist")
        return list(h) if isinstance(h, list) else []


@dataclass(slots=True)
class Monitor:
    """Accumulates records and renders them."""

    peers: dict[tuple[int, int], PeerView] = field(default_factory=dict)
    nodes: dict[int, dict[str, Any]] = field(default_factory=dict)
    boots: dict[int, dict[str, Any]] = field(default_factory=dict)
    events: list[tuple[float, dict[str, Any]]] = field(default_factory=list)
    #: (node_id, path_hash) -> the latest `ns` record. Latest only: at one line per entry per
    #: statistics interval, keeping the history would grow without bound over a 24-hour soak, and the
    #: capture file already has every line for anyone who wants the history.
    ns: dict[tuple[int, int], dict[str, Any]] = field(default_factory=dict)
    frames_seen: int = 0
    started: float = field(default_factory=time.time)

    #: Membership transitions worth surfacing the moment they happen: these are
    #: section 13-M0's evidence that the heartbeat timers behave, and they are also
    #: the events that a scrolling statistics stream would bury.
    LOUD_EVENTS = frozenset(
        {"peer_dead", "peer_revived", "peer_rebooted", "peer_left", "version_pinned"}
    )

    def on_record(self, kind: str, data: dict[str, Any], *, host_ts: float | None = None) -> str | None:
        """Fold in one record. Returns a line to print immediately, or None."""
        ts = host_ts if host_ts is not None else time.time()

        if kind == "boot":
            node = int(data.get("node", 0))
            self.boots[node] = data
            return self._boot_line(node, data)

        if kind == "link":
            node = int(data.get("node", 0))
            peer = int(data.get("peer", 0))
            view = self.peers.setdefault((node, peer), PeerView(node=node, peer=peer))
            view.mac = str(data.get("mac", ""))
            view.state = str(data.get("state", "?"))
            view.espnow_ver = int(data.get("espnow_ver", 0))
            view.mtu = int(data.get("mtu", 0))
            view.misses = int(data.get("misses", 0))
            view.rssi = int(data.get("rssi", 0))
            view.epoch = int(data.get("epoch", 0))
            view.tx = dict(data.get("tx", {}))
            view.rx = dict(data.get("rx", {}))
            view.rtt = dict(data.get("rtt", {}))
            view.last_seen = ts
            return None

        if kind == "node":
            self.nodes[int(data.get("node", 0))] = data
            return None

        if kind == "event":
            self.events.append((ts, data))
            evt = str(data.get("kind", "?"))
            if evt in self.LOUD_EVENTS:
                return (
                    f"  ! {evt:<15} node 0x{int(data.get('node', 0)):04x} "
                    f"peer 0x{int(data.get('peer', 0)):04x} "
                    f"at {int(data.get('at_ms', 0)) / 1000:.1f}s "
                    f"a={data.get('a')} b={data.get('b')}"
                )
            return None

        if kind == "ns":
            # One line per namespace entry, every statistics interval. Far too many to print, so only
            # the latest state per (node, hash) is kept -- and a quality that is not GOOD is surfaced
            # immediately, because a resource that has gone STALE or UNAVAILABLE mid-soak is precisely
            # what section 4 rule 2 wants seen rather than averaged away.
            node = int(data.get("node", 0))
            h = int(data.get("hash", 0))
            key = (node, h)
            prev = self.ns.get(key)
            self.ns[key] = data
            quality = str(data.get("quality", "?"))
            was = str(prev.get("quality", "?")) if prev else None
            if quality != "GOOD" and quality != was:
                label = describe(sys_path_for_hash(node, h) or f"0x{h:08x}")
                return (
                    f"  ~ ns {label} on node 0x{node:04x} became {quality} "
                    f"(age {data.get('age_ms')} ms, bound {data.get('bound_ms')} ms)"
                )
            return None

        return None

    def _boot_line(self, node: int, data: dict[str, Any]) -> str:
        dram = data.get("dram", {})
        wifi = dram.get("wifi_stack")
        espnow = dram.get("espnow")
        # section 6's [MEASURE] item, surfaced the moment a board reports it -- with
        # section 6's own trigger evaluated rather than left to the reader.
        note = ""
        if isinstance(wifi, int):
            note = f", wifi stack {wifi / 1024:.1f} KB"
            if wifi > 40 * 1024:
                note += " (ABOVE section 6's ~40 KB: the RX ring shrinks first)"
        if isinstance(espnow, int):
            note += f", esp-now {espnow / 1024:.1f} KB"
        return (
            f"  * boot node 0x{node:04x} epoch {data.get('epoch')} "
            f"mac {data.get('mac')} esp-now v{data.get('espnow_ver')} "
            f"ch {data.get('chan')} hb {data.get('hb_period_ms')}ms x{data.get('hb_miss_limit')} "
            f"idf {data.get('idf')}{note}"
        )

    # -- rendering ----------------------------------------------------------
    def table(self) -> str:
        """The live table. One row per link, in a fixed column order."""
        if not self.peers:
            return "  (no links yet)"

        rows = [
            "  node   peer    state   v  rssi   tx-pdr%  rx-pdr%   rtt-min  rtt-max"
            "   rtt-p50     rtt-p99   samples  lost-tx  lost-rx"
        ]
        for (node, peer), v in sorted(self.peers.items()):
            tx_pdr = ppm_to_percent(v.tx.get("pdr_ppm"))
            rx_pdr = ppm_to_percent(v.rx.get("pdr_ppm"))
            rows.append(
                f"  0x{node:04x} 0x{peer:04x}  {v.state:<7} {v.espnow_ver}  "
                f"{v.rssi:4d}   {_fmt_pct(tx_pdr)}   {_fmt_pct(rx_pdr)}   "
                f"{_fmt_us(v.rtt.get('min_us'))}   {_fmt_us(v.rtt.get('max_us'))}  "
                f"{_fmt_interval(v.rtt.get('p50_us')):>10}  {_fmt_interval(v.rtt.get('p99_us')):>10}  "
                f"{int(v.rtt.get('samples', 0)):8d} "
                f"{int(v.tx.get('cb_fail', 0)):8d} "
                f"{int(v.rx.get('lost_seqgap', 0)):8d}"
            )
        return "\n".join(rows)

    def on_reply_frame(self, src: int, payload: bytes) -> None:
        """Fold a decoded REPLY into namespace state.

        This is what makes a *frame* capture replayable, not only a console capture. A REPLY carries
        §4 rule 2's tuple but not the declaration around it — there is no `owner`, `kind`, `bound_ms`
        or `updates` on the wire, because the requester already knows what it asked for. Those stay
        None rather than being invented, so an entry learnt from a frame is honestly narrower than one
        learnt from an `ns` record.
        """
        from .ns_payloads import Reply

        try:
            rep = Reply.parse(payload)
        except ValueError:
            return
        self.ns[(src, rep.path_hash)] = {
            "node": src,
            "hash": rep.path_hash,
            "owner": None,
            "kind": None,
            "unit": rep.unit,
            "class": rep.latency_class,
            "bound_ms": None,
            "quality": self._quality_name(rep.quality),
            "age_ms": rep.age_ms,
            "ts": rep.timestamp_ms,
            "updates": None,
            "value": rep.value.json() if rep.ok else None,
        }

    @staticmethod
    def _quality_name(q: int) -> str:
        from .value import Quality

        return Quality.name_of(q)

    # -- namespace state, and its digest -----------------------------------------------------------
    #
    # §13-M2's acceptance test is "a captured 10-minute session replays and produces byte-identical
    # namespace state". That needs a *canonical* serialisation, because "byte-identical" is otherwise
    # a claim about dict ordering rather than about the capture being a faithful record.
    #
    # What goes in: only fields the *node* reported. What stays out: host timestamps, wall-clock
    # anything, and the order records arrived in. A digest that moved because the host was busier on
    # the replay would test nothing.

    #: The §4 rule 2 tuple, plus the declaration that gives it meaning. Fixed order, because the order
    #: is part of the canonical form.
    NS_STATE_FIELDS = (
        "owner", "kind", "unit", "class", "bound_ms",
        "quality", "age_ms", "ts", "updates", "value",
    )

    def namespace_state(self) -> list[dict[str, Any]]:
        """Every namespace entry's last known state, in a deterministic order."""
        out = []
        for (node, h) in sorted(self.ns.keys()):
            rec = self.ns[(node, h)]
            entry: dict[str, Any] = {"node": node, "hash": h}
            for f in self.NS_STATE_FIELDS:
                entry[f] = rec.get(f)
            out.append(entry)
        return out

    def namespace_canonical(self) -> bytes:
        """The canonical byte form the digest is taken over.

        `sort_keys` plus a fixed separator plus the sorted entry order above makes this reproducible
        across runs and Python versions. Floats are left as JSON writes them — the values that reach
        here came from `json.loads` of the node's own output, so they round-trip exactly.
        """
        return json.dumps(
            self.namespace_state(), sort_keys=True, separators=(",", ":"), default=str
        ).encode("utf-8")

    def namespace_digest(self) -> str:
        return hashlib.sha256(self.namespace_canonical()).hexdigest()

    def summary(self) -> str:
        """The end-of-run report: what section 13-M0's acceptance check reads."""
        out: list[str] = []
        elapsed = time.time() - self.started
        out.append("")
        out.append("=" * 78)
        out.append(f"Potluck M0 capture summary  --  {elapsed / 3600:.2f} h "
                   f"({elapsed:.0f} s), {self.frames_seen} frames teed")
        out.append("=" * 78)

        for node, boot in sorted(self.boots.items()):
            out.append("")
            out.append(f"node 0x{node:04x}  epoch {boot.get('epoch')}  mac {boot.get('mac')}  "
                       f"esp-now v{boot.get('espnow_ver')}  idf {boot.get('idf')}")
            dram = boot.get("dram", {})
            if dram:
                out.append("  section 6 [MEASURE] -- free internal DRAM at bring-up:")
                for key in ("at_boot", "after_nvs", "after_netif", "after_wifi_init",
                            "after_wifi_start", "after_espnow"):
                    if key in dram:
                        out.append(f"    {key:<20}{dram[key] / 1024:9.1f} KB free")
                for key, label in (("wifi_stack", "Wi-Fi stack cost"),
                                   ("espnow", "ESP-NOW cost"),
                                   ("total_to_radio", "boot to radio, total")):
                    if key in dram:
                        out.append(f"    {label:<20}{dram[key] / 1024:9.1f} KB")
                w = dram.get("wifi_stack")
                if isinstance(w, int):
                    verdict = "within" if w <= 40 * 1024 else "ABOVE -- section 6 says the RX ring shrinks first"
                    out.append(f"    vs section 6's ~40 KB expectation: {verdict}")

        for (node, peer), v in sorted(self.peers.items()):
            out.append("")
            out.append(f"link 0x{node:04x} -> 0x{peer:04x}  ({v.mac})  state={v.state}  "
                       f"esp-now v{v.espnow_ver}  payload cap {v.mtu} B  last rssi {v.rssi}")

            tx_pdr = ppm_to_percent(v.tx.get("pdr_ppm"))
            rx_pdr = ppm_to_percent(v.rx.get("pdr_ppm"))
            out.append(f"  outbound: {int(v.tx.get('frames', 0))} submitted, "
                       f"{int(v.tx.get('cb_ok', 0))} acked, {int(v.tx.get('cb_fail', 0))} failed, "
                       f"{int(v.tx.get('enqueue_err', 0))} never queued")
            out.append(f"            PDR {'unmeasured' if tx_pdr is None else f'{tx_pdr:.4f} %'}"
                       "   (from the ESP-NOW send callback, i.e. the 802.11 MAC-layer ACK)")
            out.append(f"  inbound:  {int(v.rx.get('frames', 0))} accepted, "
                       f"{int(v.rx.get('lost_seqgap', 0))} lost, "
                       f"{int(v.rx.get('reorder_dup', 0))} duplicate/reordered, "
                       f"{int(v.rx.get('dropped_bad', 0))} unparseable")
            out.append(f"            PDR {'unmeasured' if rx_pdr is None else f'{rx_pdr:.4f} %'}"
                       "   (inferred from seq gaps)")

            samples = int(v.rtt.get("samples", 0))
            out.append(f"  round-trip time, {samples} samples")
            if samples:
                out.append(f"    min {v.rtt.get('min_us')} us   max {v.rtt.get('max_us')} us   "
                           f"p50 {_fmt_interval(v.rtt.get('p50_us'))}   "
                           f"p99 {_fmt_interval(v.rtt.get('p99_us'))}")
                out.append(f"    local tx queue+ack: last {v.rtt.get('txq_last_us')} us, "
                           f"max {v.rtt.get('txq_max_us')} us")
                out.append(f"    remote turnaround (peer's own clock): "
                           f"last {v.rtt.get('remote_turnaround_us')} us, "
                           f"max {v.rtt.get('remote_turnaround_max_us')} us")
                out.append(f"    unanswered probes: {v.rtt.get('timeouts')}")
                out.append("")
                out.append("    delay histogram (round-trip; there is no one-way measurement --")
                out.append("    clocks are unsynchronised and ESP-NOW's retry path is not symmetric)")
                hist = v.histogram
                total = sum(hist) if hist else 0
                for i, count in enumerate(hist):
                    if count == 0:
                        continue
                    share = 100.0 * count / total if total else 0.0
                    bar = "#" * min(40, int(round(share * 0.4)))
                    out.append(f"      {describe_bucket(i):>17}  {count:8d}  {share:5.1f}%  {bar}")
            else:
                out.append("    no samples -- nothing was measured, so nothing is reported")

        for node, nd in sorted(self.nodes.items()):
            out.append("")
            out.append(f"node 0x{node:04x} counters")
            rx, tx, mem = nd.get("rx", {}), nd.get("tx", {}), nd.get("membership", {})
            out.append(f"  rx total {rx.get('total')}, bad_frame {rx.get('bad_frame')}, "
                       f"unknown_peer {rx.get('unknown_peer')}, wrong_dst {rx.get('wrong_dst')}, "
                       f"short_payload {rx.get('short_payload')}, "
                       f"unknown_opcode {rx.get('unknown_opcode')}")
            out.append(f"  rx queue overruns (ours, not the radio's): {rx.get('queue_dropped')}")
            out.append(f"  tx total {tx.get('total')}, enqueue_err {tx.get('enqueue_err')}, "
                       f"send-cb queue dropped {tx.get('done_queue_dropped')}")
            out.append(f"  membership: {mem.get('deaths')} deaths, {mem.get('revivals')} revivals, "
                       f"{mem.get('reboots_seen')} peer reboots seen, "
                       f"{mem.get('table_full')} table-full refusals, "
                       f"{mem.get('events_dropped')} events dropped")
            out.append(f"  free internal DRAM now {int(nd.get('free_dram', 0)) / 1024:.1f} KB, "
                       f"largest block {int(nd.get('largest_free_block', 0)) / 1024:.1f} KB")

        if self.ns:
            out.append("")
            out.append("namespace, last state of each entry")
            out.append("  section 4 rule 2's tuple in full. A value with no age is not a reading.")
            by_node: dict[int, list[dict[str, Any]]] = {}
            for (node, _h), rec in self.ns.items():
                by_node.setdefault(node, []).append(rec)
            for node in sorted(by_node):
                out.append(f"  node 0x{node:04x}")
                rows = sorted(by_node[node], key=lambda r: int(r.get("hash", 0)))
                for r in rows:
                    h = int(r.get("hash", 0))
                    path = sys_path_for_hash(node, h)
                    label = describe(path) if path else f"0x{h:08x}"
                    value = r.get("value")
                    shown = "-" if value is None else str(value)
                    # An entry learnt from a REPLY frame has no bound and no update count: the wire
                    # does not carry the declaration, only the reading. Print "?" for those rather
                    # than a zero, which would read as a measured fact that nobody measured.
                    bound = r.get("bound_ms")
                    updates = r.get("updates")
                    out.append(
                        f"    {label:<20} {shown:>12}  {str(r.get('quality')):<12} "
                        f"age {r.get('age_ms')} ms / bound "
                        f"{'?' if bound is None else bound} ms, "
                        f"L{r.get('class')}, "
                        f"{'?' if updates is None else updates} updates"
                    )
            # A resource whose staleness bound is smaller than the statistics interval reports GOOD
            # only because it is refreshed on the same timer, which makes the bound decoration rather
            # than a check. Worth saying out loud, since section 4 rule 2 is only as good as the bounds.
            #
            # `updates is None` means "not reported on this transport", which is not the same as zero
            # and must not be counted as never-written. Getting this wrong crashed the replay summary,
            # because `.get("updates", 0)` returns None when the key is present *and* None.
            never = [r for r in self.ns.values()
                     if r.get("updates") is not None and int(r["updates"]) == 0]
            if never:
                out.append(f"  {len(never)} entr{'y' if len(never) == 1 else 'ies'} never written")

        if self.events:
            loud = [(t, e) for t, e in self.events if str(e.get("kind")) in self.LOUD_EVENTS]
            out.append("")
            out.append(f"membership events: {len(self.events)} total, {len(loud)} notable")
            for t, e in loud[-20:]:
                out.append(f"  {time.strftime('%H:%M:%S', time.localtime(t))} "
                           f"{e.get('kind'):<15} node 0x{int(e.get('node', 0)):04x} "
                           f"peer 0x{int(e.get('peer', 0)):04x} a={e.get('a')} b={e.get('b')}")

        out.append("")
        out.append("Bucket edges, in microseconds: " + ", ".join(
            str(e) for e in RTT_BUCKET_EDGE_US[:-1]) + ", then overflow")
        out.append("")
        return "\n".join(out)

    def print_summary(self, stream=sys.stdout) -> None:
        print(self.summary(), file=stream)
