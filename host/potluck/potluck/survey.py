"""M19/M20: a tree of cells proposed from a measured survey -- which boards hear which.

The setup wizard does not ask the owner who hears whom; it measures (ARCHITECTURE section 13, M20's large setup): a
minute or two of the link reports every board already makes -- delivery and signal per peer -- and proposes the cells,
grouped by who hears whom with the rooms as hints. The one question it asks is which boards stay on a charger: a
foreman's eligibility is mains power (Review of M19 #4); the reconciler assigns the foreman and its standby.

    propose(boards, links, chargers)  ->  a Proposal: cells, foreman candidates, notes
    write(cluster_path, proposal)     ->  the cluster file's boards[].cells/foreman and its cells block

The rules, each from the architecture:

- A cell is one radio space: every pair of its boards hears each other (a cell has no forwarding, section 4), and it
  holds at most CELL_MAX boards, so that a cell of ten under an upper cell of ten fits a board's 19 peers.
- Boards of one room start together; rooms are hints, not walls -- two small rooms that hear each other become one cell.
- Twenty boards or fewer that all hear each other are one cell, as before M19: a small setup never sees a tree.
- Each cell's foreman candidates are its boards on a charger, up to two, those that hear most of the other cells'
  candidates first -- the upper cell is a radio space too, so whoever holds a role must hear the other cells' holders.
  A cell with no board on a charger, or candidates that do not hear another cell's, is a note for the owner, not a
  guess.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

#: A cell's most boards in a proposed tree (ten cells of ten fit a board's 19 peers, ARCHITECTURE section 13).
CELL_MAX = 10
#: One cell, no tree, up to this many boards that all hear each other (a board's 19 peers and itself).
FLAT_MAX = 20
#: What "hears" means on one link: this share of frames delivered, and a signal no weaker than this (dBm).
HEAR_DELIVERY = 0.9
HEAR_RSSI = -85
UPPER = "tree"
UPPER_WINDOW_MS = 5000


@dataclass
class Proposal:
    flat: bool                                          # one cell, no tree
    cells: dict[str, list[int]] = field(default_factory=dict)       # cell name -> boards
    foremen: dict[str, list[int]] = field(default_factory=dict)     # cell name -> its foreman candidates
    notes: list[str] = field(default_factory=list)

    def to_json(self) -> dict[str, Any]:
        return {"flat": self.flat, "cells": {k: [f"{n:04x}" for n in v] for k, v in self.cells.items()},
                "foremen": {k: [f"{n:04x}" for n in v] for k, v in self.foremen.items()}, "notes": self.notes}


def hears(links: dict[tuple[int, int], dict], a: int, b: int) -> bool:
    """a and b hear each other: the link measured either way delivers and is strong enough."""
    for x, y in ((a, b), (b, a)):
        k = links.get((x, y))
        if k is None:
            continue
        d, r = k.get("delivery"), k.get("rssi")
        if d is not None and d >= HEAR_DELIVERY and (r is None or r >= HEAR_RSSI):
            return True
    return False


def _slug(s: str) -> str:
    import re
    return re.sub(r"[^a-z0-9]+", "-", s.lower()).strip("-") or "cell"


def propose(boards: list[int], links: dict[tuple[int, int], dict], chargers: set[int] | frozenset[int] = frozenset(),
            rooms: dict[int, str] | None = None, cell_max: int = CELL_MAX) -> Proposal:
    boards = sorted(set(boards))
    rooms = rooms or {}
    if len(boards) <= FLAT_MAX and all(hears(links, a, b) for i, a in enumerate(boards) for b in boards[i + 1:]):
        return Proposal(flat=True, cells={"": boards})
    # 1. per room, boards that all hear each other, at most cell_max a group
    by_room: dict[str, list[int]] = {}
    for n in boards:
        by_room.setdefault(rooms.get(n, ""), []).append(n)
    groups: list[tuple[str, list[int]]] = []
    for room in sorted(by_room):
        mine: list[list[int]] = []
        for n in by_room[room]:
            for g in mine:
                if len(g) < cell_max and all(hears(links, n, m) for m in g):
                    g.append(n)
                    break
            else:
                mine.append([n])
        groups += [(room, g) for g in mine]
    # 2. rooms are hints: merge the smallest groups that fit and hear each other whole
    merged = True
    while merged:
        merged = False
        groups.sort(key=lambda rg: (len(rg[1]), rg[1][0]))
        for i in range(len(groups)):
            for j in range(i + 1, len(groups)):
                a, b = groups[i][1], groups[j][1]
                if len(a) + len(b) <= cell_max and all(hears(links, x, y) for x in a for y in b):
                    room = groups[i][0] if len(a) >= len(b) else groups[j][0]
                    groups[i] = (room, sorted(a + b))
                    del groups[j]
                    merged = True
                    break
            if merged:
                break
    # 3. names: the room's, numbered when a room has more than one cell
    groups.sort(key=lambda rg: rg[1][0])
    p = Proposal(flat=False)
    used: dict[str, int] = {}
    for room, g in groups:
        base = _slug(room) if room else "cell"
        used[base] = used.get(base, 0) + 1
        name = base if used[base] == 1 and room else f"{base}-{used[base]}"
        p.cells[name] = sorted(g)
    if len(p.cells) > cell_max:
        p.notes.append(f"{len(p.cells)} cells: an upper cell holds ten -- a second upper level is needed, or fewer, "
                       f"larger cells (move boards so more of them hear each other)")
    # 4. foreman candidates: boards on a charger, the ones hearing most other cells' chargers first
    cand = {name: [n for n in g if n in chargers] for name, g in p.cells.items()}
    for name, g in p.cells.items():
        if not cand[name]:
            p.notes.append(f"cell '{name}' has no board on a charger: put one of {', '.join(f'{n:04x}' for n in g)} "
                           f"on a charger -- a foreman stays on mains power")
            p.foremen[name] = []
            continue
        others = [m for k, v in cand.items() if k != name for m in v]
        score = sorted(cand[name], key=lambda n: (-sum(hears(links, n, m) for m in others), n))
        p.foremen[name] = sorted(score[:2])
    # 5. the upper cell is a radio space: every candidate hears every other cell's candidates
    deaf = [(a, b) for k1, v1 in p.foremen.items() for k2, v2 in p.foremen.items() if k1 < k2
            for a in v1 for b in v2 if not hears(links, a, b)]
    if deaf:
        p.notes.append("the upper cell needs its foremen within earshot of each other; these do not hear: " +
                       ", ".join(f"{a:04x}-{b:04x}" for a, b in deaf[:8]) + (" ..." if len(deaf) > 8 else "") +
                       " -- move them, or put other boards of their cells on a charger")
    return p


def links_from_model(model: dict) -> tuple[list[int], dict[tuple[int, int], dict], dict[int, str]]:
    """The hub's model (GET /api/model) as propose()'s input: boards, links (delivery, rssi) and rooms."""
    boards, links, rooms = [], {}, {}
    for b in model.get("boards", []):
        try:
            n = int(str(b.get("node")), 16)
        except ValueError:
            continue
        boards.append(n)
        if b.get("room"):
            rooms[n] = str(b["room"])
        for k in b.get("links", []):
            try:
                q = int(str(k.get("peer")), 16)
            except ValueError:
                continue
            links[(n, q)] = {"delivery": k.get("delivery"), "rssi": k.get("rssi")}
    return boards, links, rooms


def write(cluster: str | Path, p: Proposal) -> dict:
    """Put a proposal into the cluster file: each board's cells and foreman mark, and the cells block -- every cell a
    child of one upper cell. Boards the proposal does not name are left as they are. Returns the new document."""
    lacking = [name for name, v in p.foremen.items() if not v]
    if not p.flat and lacking:
        raise ValueError(f"cell(s) {', '.join(lacking)} have no board on a charger, so no foreman -- the tree cannot be "
                         f"written until one of each is")
    path = Path(cluster)
    doc = json.loads(path.read_text(encoding="utf-8"))
    cell_of = {n: name for name, g in p.cells.items() for n in g}
    elig = {n for v in p.foremen.values() for n in v}
    for b in doc.get("boards", []):
        n = int(b["node"], 16) if isinstance(b["node"], str) else int(b["node"])
        b.pop("cells", None)
        b.pop("foreman", None)
        if p.flat or n not in cell_of:
            continue
        b["cells"] = [cell_of[n]]
        if n in elig:
            b["foreman"] = True
    cells = dict(doc.get("cells") or {})
    named = {c for b in doc.get("boards", []) for c in b.get("cells", [])}
    for name in list(cells):  # the last tree's: the upper cell, and cells no board names any more
        if name == UPPER or name in p.cells or name not in named:
            cells.pop(name)
    if not p.flat:
        for name in p.cells:
            cells[name] = {"parent": UPPER}
        cells[UPPER] = {"window_ms": UPPER_WINDOW_MS}
    if cells:
        doc["cells"] = cells
    else:
        doc.pop("cells", None)
    from .setup import _write_cluster
    _write_cluster(path, doc)
    return doc
