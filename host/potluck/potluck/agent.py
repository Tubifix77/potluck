"""potluck-agent: a host as a mega-node, offering named services -- ARCHITECTURE.md section 7.5, M8.

    python -m potluck.agent --port COM6 --service time
    python -m potluck.agent --port COM6 --service time --seconds 120

A host does not advertise raw capacity ("cores:16, ram:32000"). It offers NAMED SERVICES, each an
actor path in the namespace -- `potluck://<cluster>/svc/<name>` -- that any node calls exactly as it
calls any other remote actor: a CALL to the path, answered by a REPLY carrying a Value. The class is
L4 by definition, and the caller must cope with the host being off (section 7.5); the firmware's
built-in `svc_client` actor is that caller.

WHAT IS OFFERED IS THE MACHINE'S USER'S CHOICE. Section 7.1: what a host donates is capped by a local
donation config owned by the machine's user. Here that is the command line: nothing is served unless
named with --service. A CALL for any other path is answered NOT_FOUND at once, never left to time out.

WHERE THE SERVICES ARE ANNOUNCED. As with everything else in v1 (section 7.4), placement is frozen in
the signed manifest: the host is a node in the package, owning its `svc/*` paths, and the build
compiles each caller with the provider's node id. The agent serves; it does not need to be found.

THE SERVICES THIS FILE KNOWS, and why each is a host's job rather than a microcontroller's:

  time   Unix time in milliseconds (U64). A board has no battery-backed clock and, in Potluck's
         cell, no internet: wall-clock time is something only a host can hand it.

The agent rides on the same bridge as potctl (one serial cable to one board, section 7.1's
`potluck-agent <-> potluck-bridge <-> wire`), so while it runs, the host is a member of the cell: it
heartbeats, and when it stops, the board it is cabled to declares it dead within the host's declared
window -- which is what a calling actor's degradation hangs on.
"""

from __future__ import annotations

import argparse
import sys
import time
from dataclasses import dataclass, field
from typing import Callable

from . import frame as fr
from .ns_payloads import REPLY_TO_READ, Call, Read, Reply
from .paths import path_hash
from .value import NsError, Quality, Value

LATENCY_L4 = 4


def svc_time(_args: bytes) -> Value:
    return Value.of_u64(int(time.time() * 1000))


#: name -> (what it is, the function). Extending the agent is adding a line here.
SERVICES: dict[str, tuple[str, Callable[[bytes], Value]]] = {
    "time": ("Unix time in milliseconds (U64)", svc_time),
}


@dataclass
class AgentStats:
    served: int = 0
    refused: int = 0
    casts: int = 0
    reads_refused: int = 0
    by_service: dict[str, int] = field(default_factory=dict)


class Agent:
    """Answers a bridge's CALL / CAST / READ requests for the services the user chose to offer."""

    def __init__(self, bridge, *, cluster: str = "lab", services: list[str]) -> None:
        unknown = [s for s in services if s not in SERVICES]
        if unknown:
            raise ValueError(f"unknown service(s) {unknown}; this agent offers {sorted(SERVICES)}")
        self.bridge = bridge
        self.paths = {path_hash(f"potluck://{cluster}/svc/{n}"): n for n in services}
        self.stats = AgentStats()

    def path_of(self, name: str) -> int:
        for h, n in self.paths.items():
            if n == name:
                return h
        raise KeyError(name)

    def handle(self, f: fr.Frame) -> None:
        if f.opcode == fr.Op.READ:
            # The agent's services are actors, not stored values: a READ of one is answered, not
            # dropped, so the reader learns at once that this is not how to reach it.
            try:
                req = Read.parse(f.payload)
            except ValueError:
                return
            self.stats.reads_refused += 1
            self._reply(f, Reply(path_hash=req.path_hash, timestamp_ms=0, age_ms=0, unit=0,
                                 reply_to=REPLY_TO_READ, status=int(NsError.NOT_FOUND),
                                 quality=int(Quality.UNAVAILABLE), latency_class=LATENCY_L4, value=Value()))
            return
        try:
            call = Call.parse(f.payload)
        except ValueError:
            return
        name = self.paths.get(call.path_hash)
        if f.opcode == fr.Op.CAST:
            if name is not None:
                SERVICES[name][1](call.args)
                self.stats.casts += 1
            return
        if name is None:
            self.stats.refused += 1
            rep = Reply(path_hash=call.path_hash, timestamp_ms=0, age_ms=0, unit=0, reply_to=fr.Op.CALL,
                        status=int(NsError.NOT_FOUND), quality=int(Quality.UNAVAILABLE),
                        latency_class=LATENCY_L4, value=Value())
        else:
            value = SERVICES[name][1](call.args)
            self.stats.served += 1
            self.stats.by_service[name] = self.stats.by_service.get(name, 0) + 1
            rep = Reply(path_hash=call.path_hash, timestamp_ms=self.bridge.uptime_ms & 0xFFFFFFFF, age_ms=0,
                        unit=0, reply_to=fr.Op.CALL, status=int(NsError.OK), quality=int(Quality.GOOD),
                        latency_class=LATENCY_L4, value=value)
        self._reply(f, rep)

    def _reply(self, f: fr.Frame, rep: Reply) -> None:
        try:
            self.bridge.send_frame(fr.Op.REPLY, rep.encode(), dst=f.src, msg_id=f.msg_id, lclass=LATENCY_L4)
        except Exception:  # the link went away; the bridge's reader notices
            pass


def main(argv: list[str] | None = None) -> int:
    from .bridge import Bridge

    ap = argparse.ArgumentParser(prog="potluck-agent", description=__doc__.splitlines()[0])
    ap.add_argument("--port", help="the serial frame link (COMx or /dev/tty...)")
    ap.add_argument("--tcp", help="host:port of a TCP bridge instead")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--cluster", default="lab")
    ap.add_argument("--service", action="append", default=[], choices=sorted(SERVICES),
                    help="offer this service (repeat for more); nothing is offered unless named")
    ap.add_argument("--seconds", type=float, default=0, help="stop after this long (0 = until Ctrl+C)")
    args = ap.parse_args(argv)
    if not args.service:
        ap.error("name at least one --service: the agent offers only what its machine's user chooses")

    holder: dict[str, Agent] = {}
    bridge = Bridge.open(port=args.port, tcp=args.tcp, baud=args.baud,
                         on_log=lambda m: print(f"# {m}", file=sys.stderr, flush=True),
                         on_request=lambda f: holder["agent"].handle(f))
    agent = Agent(bridge, cluster=args.cluster, services=args.service)
    holder["agent"] = agent
    bridge.start()
    try:
        if bridge.hello(timeout=3.0) is None:
            print("# no answer from the board on the frame link", file=sys.stderr)
            return 3
        for h, n in agent.paths.items():
            print(f"# serving potluck://{args.cluster}/svc/{n} (0x{h:08x}): {SERVICES[n][0]}", file=sys.stderr)
        end = time.monotonic() + args.seconds if args.seconds else None
        next_report = time.monotonic() + 10
        while end is None or time.monotonic() < end:
            time.sleep(0.2)
            if time.monotonic() >= next_report:
                next_report += 10
                s = agent.stats
                print(f"{{\"t\":\"agent\",\"served\":{s.served},\"refused\":{s.refused},\"casts\":{s.casts},"
                      f"\"reads_refused\":{s.reads_refused},\"host_ts\":{time.time():.3f}}}", flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        try:
            bridge.bye()
        except Exception:
            pass
        bridge.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
