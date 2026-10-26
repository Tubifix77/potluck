# Developing apps for Potluck

**Who this is for:** someone who has vanilla Potluck running on ESP32-S3 boards they own -- however many -- and
wants to write an app of their own, run it on those boards, and later share it on GitHub for others to run on
theirs. You are the owner of your cluster: your keys, your boards, your packages. Not running Potluck yet? Start
with [GETTING-STARTED.md](GETTING-STARTED.md).

For anyone who wants to make an app for a Potluck cluster: code that reads the cluster's resources
(sensors, radio, other apps' outputs), computes, and publishes results the rest of the cluster can read.
It draws on the experience of the first two apps built on Potluck: a **room-presence app**, which detects a
person in a room from how their body weakens the boards' own radio links, and a **Wi-Fi extender app** that
shares the radio with Potluck. Both live in repositories of their own, the way an app should (they are not
public; they ship compiled into the release image).

This manual is for **Potluck 1.0.1**, built with **ESP-IDF v6.0.2** and run with ESP-IDF's **Python 3.14**; the PC
side is tested on Windows 11 only (Linux is untested). One command takes a cell from a cluster file to running
and verified: `python -m potluck.apply <cluster.json>` builds the firmware, rolls it out where it differs, signs
and deploys the package and checks every output -- the counters, the order and the variants are the tool's, not
yours (section 7). A package changes on the boards without a reboot, and checkpoints survive one. Apps test
themselves in their own repositories with Potluck's test kit (section 8). The worked example in section 3 is
built for the boards and tested on a PC by Potluck's own test suites. Questions, bugs and type-number requests:
[Potluck's issue tracker](https://github.com/Tubifix77/potluck/issues).

Terms used throughout:

- **Node** -- one board (or a PC running `pot_hostnode`), with a 16-bit id such as `0x1a2c`.
- **Cell** -- the nodes that hear each other over ESP-NOW (Espressif's board-to-board radio protocol).
- **Namespace** -- the cluster's shared map of named resources, `potluck://<system>/...`; every read
  returns a value *with* its age and quality ([section 4's tuple](ARCHITECTURE.md)).
- **Actor** -- a unit of app code. Potluck decides which node runs it and drives it.
- **Package** -- a signed manifest naming every actor the cluster runs; deploying one replaces the last.
- **Replica** -- a node's local copy of a resource another node owns: a read anywhere answers from it, with the
  age the value had when it was last fetched.
- **Owner** -- you: the person whose keys sign the cluster's packages. (A resource's *owner node* is the node
  that publishes it.)

---

## 1. Two ways to write an app

| | **Native actor** (an external component) | **Guest** (a WebAssembly module) |
|---|---|---|
| Language | C++17, against `pot/actor.hpp` | anything that compiles to WebAssembly 1.0; the example guests are Rust |
| Speed | native | ~50x slower than native, measured on the S3 |
| Can touch | everything an actor can: the namespace, calls to other nodes, every radio frame's signal strength, the status LED, background workers | only its wired inputs and outputs, a 128-byte checkpoint, a log and a clock -- nine imports, nothing else |
| Trust | compiled into the owner's signed firmware: the owner trusts its author with the board | runs sandboxed: the owner trusts its author only with its own memory and its fuel |
| Shipping | rebuild the firmware with your component, roll it out over the radio, then deploy a package | sign the module once as its author; the owner names it in a package -- no firmware change |
| Cost per instance | its slot (384 B) and whatever it allocates | ~80 KB of heap (its 64 KiB memory page, in PSRAM) |
| Needs | nothing extra | boards built with the `sandbox` module (`CONFIG_POT_WASM`), an author certificate from the owner |

Choose **native** when the app needs radio samples, the LED, timing below a tick, or speed -- the room-presence
app is native for all four reasons. Choose a **guest** when the code comes from someone the owner does not want
to trust with the board, or should change without a firmware update. The rest of this manual is about
native actors until section 9, which covers guests.

---

## 2. How an app reaches the boards

```
your repository                          your cluster file (cluster.json)
  firmware/components/<your_component>/    "apps": [{"git": <your repo>, "rev": <a commit or tag>,
     CMakeLists.txt                                   "components": "firmware/components",
     potluck-modules.json                             "manifests": ["manifests/<app>.json"]}]
     src/*.cpp, include/
  manifests/<app>.json                   python -m potluck.apply cluster.json
  tests/  (Potluck's test kit)             -> firmware with your component, on every board that needs it
                                           -> one signed package: your actors and everyone else's
                                           -> verified: every output read back
```

1. Your actors are C++ classes in an [ESP-IDF component](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32s3/api-guides/build-system.html)
   (ESP-IDF's unit of code) in your own repository. Potluck finds it because a `potluck-modules.json` sits
   beside its `CMakeLists.txt`, and calls one function you define, `app_rows_<directory name>()`, for its
   registration rows. No Potluck file names your app.
2. The descriptor (`potluck-modules.json`) tells Potluck's host tools your actors' names, their type
   numbers, how they are placed and how their configuration is laid out. The tools read it as data and
   never run your code.
3. A manifest fragment in your repository names your actors with their configuration. The owner's cluster file
   names your repository at a commit; `potluck apply` builds your component in (from a `git archive` export,
   never a working tree), composes your fragment with every other app's into one package, signs it with the
   owner's key and deploys it through the board on the PC's cable to the whole cell.

---

## 3. A worked example: `pot_hello`

[`examples/apps/pot_hello`](examples/apps/pot_hello) is a complete app with one actor, `ext:hello/peers`: every
period, each board publishes how many radio peers it sees alive to `potluck://lab/node-<id>/hello/peers`, and
lights its status LED cyan while it hears at least one (an app's colour shows in green's place, so only on a
healthy board). Read its four files in this order:

- **`potluck-modules.json`** -- the descriptor:

  ```json
  {"component": "pot_hello", "types": [240, 255],
   "modules": [{"module": "ext:hello/peers", "type": 240, "placement": "every",
                "outputs": ["{node}/hello/peers"],
                "config": [{"name": "system_prefix", "kind": "system_prefix"},
                           {"name": "period_ms", "kind": "u16", "default": 5000, "min": 1000, "max": 60000}]}]}
  ```

- **`CMakeLists.txt`** -- an ordinary ESP-IDF component that `REQUIRES pot_actors pot_link pot_ns pot_deploy`.
- **`include/hello/hello.hpp`, `src/hello.cpp`** -- the config parser, the actor class and its registration row.
- **`src/app_rows.cpp`** -- `app_rows_pot_hello()`, the one function Potluck calls.

And its manifest fragment, [`examples/apps/hello.json`](examples/apps/hello.json), whole -- a manifest needs
`system`, a non-empty `nodes` list and `actors` (`schema` and `min_core_version` default to 1):

```json
{
  "schema": 1,
  "system": "lab",
  "min_core_version": 1,
  "nodes": [
    {"node_id": 25344, "label": "A"},
    {"node_id": 29544, "label": "B"},
    {"node_id": 33120, "label": "C"}
  ],
  "actors": [
    {
      "name": "hello",
      "module": "ext:hello/peers",
      "latency_class": 4,
      "config": {
        "period_ms": 5000
      }
    }
  ]
}
```

Its three boards are the author's own cell: nothing in it needs them (no `owns`, no actor pinned to one), so
`apply` ignores them and runs the actor on your boards, however many (section 6). Put it on your boards with the
preset [`examples/cells/hello.json`](examples/cells/hello.json) -- your boards' ids and your adapter's port in it,
then `python -m potluck.apply ../../examples/cells/hello.json` from `host/potluck/` (section 7).

It is tested twice before any board: [`examples/apps/pot_hello/tests`](examples/apps/pot_hello/tests) runs the
actor on a real `pot::Node` on the PC with Potluck's test kit, and `host/potluck/tests/test_examples.py` checks
that the host tools compile `hello.json` to exactly the bytes the C++ test
[`tests/test_hello_app.cpp`](tests/test_hello_app.cpp) parses. Copy that pattern (section 8).

### A portable actor: what changes

`pot_hello` runs on every node. An actor that should run *once* in the cell and move when its board dies is
portable, and differs in five places:

1. **The descriptor says `"placement": "portable"`** and does *not* list `period_ms`: the host writes the period
   into the actor's standard header from the manifest's `config.period_ms` (default 1000, 50-60000).
2. **Its config bytes start with Potluck's standard header**, then your own fields in descriptor order:
   `out_hash u32` (the hash of `potluck://<system>/act/<name>/out` -- the actor's identity in the reconciler,
   whether or not you publish there), `period_ms u16`, `count u8`, then `count` x (`node u16`, `gravity u8`): the
   nodes it may run on and how much each is preferred. Parse it with
   `portable_header(decl, TickerConfig& place, size_t& header_len)` (`pot/deploy.hpp`); your bytes start at
   `decl.cfg + header_len`.
3. **The row has a placement hook**: `bool placement(const ActorDecl& d, TickerConfig& out)` returns
   `portable_header(d, out, len)` and checks the total length -- see `ticker_placement` in
   `firmware/components/pot_actors/src/ticker.cpp`, the smallest portable actor.
4. **The reconciler declares your outputs, not your `start()`.** It calls your `outputs()` hook with `node` 0 when
   it loads the image, declares every output on every node with no owner, and moves their owner with the actor.
   Your `start()` declares nothing; your `tick()` is called only while this node is the outputs' fenced owner.
5. **Output paths come in through the config.** The board knows neither the actor's name nor the cluster's, so
   give each output's full path as a `path` field (the room-presence app's fusion actor takes one like
   `"confidence": "potluck://lab/act/fusion/confidence"`), and the host sends its hash.

The guest actor (`firmware/components/pot_wasm/src/guest.cpp`, `GuestActor` and `kGuestKind`) is a complete
portable actor with several outputs, a remote input and a checkpoint, if you want one to read.

---

## 4. The actor API

Everything below is in [`firmware/components/pot_actors/include/pot/actor.hpp`](firmware/components/pot_actors/include/pot/actor.hpp),
which is the reference; this is the map.

### The registration row

Each actor type is one `ActorKind`:

```cpp
ActorKind{type, "name", &check, &create, &placement /* or nullptr */, &outputs /* or nullptr */};
```

- `check(decl, env, &why)` -- validate a declaration with no side effects; set `why` on refusal. Potluck
  checks every actor in an image before it creates any: a half-applied image is worse than either whole one.
- `create(mem, decl, env)` -- placement-new your actor into `mem` (`kActorSlotBytes`, 384 bytes).
  `static_assert(sizeof(YourActor) <= kActorSlotBytes)`; anything bigger goes in PSRAM or a static buffer.
- `placement(decl, out)` -- portable types only (below): fill where it may run.
- `outputs(decl, node, peer, out, cap)` -- the resources it publishes, so *other* nodes can hold replicas
  (below).

### Placement: pinned, every node, or portable

| placement | the manifest says | where it runs | its outputs |
|---|---|---|---|
| `pinned` | `"pin": <node id>` | that node | per node, under `node-<id>/...` |
| `every` | nothing | one instance on every node | per node, or per link (`peer` != 0) |
| `portable` | nothing (optionally pinned as a single choice) | one instance in the cell, placed by the [reconciler](ARCHITECTURE.md) (section 7.7), moved when its node dies | node-free, `act/<name>/...`; moved and fenced together |

A portable actor's config starts with Potluck's standard header (output key, period, eligible nodes and
their gravity); the host writes it for you in front of your own fields. It must be latency class L3 or
looser and may only *read* the resources it binds: an actor that owns an actuator is never portable.

### The lifecycle

- `start(now_ms)` -- once per activation, after the node exists. A pinned or every-node actor declares the
  resources it owns here; a portable actor's outputs are already declared by the reconciler (section 3). Return
  false to be dropped (logged, the node runs on).
- `tick(now_ms)` -- every pass of the link task, **with the node's lock held, beside the heartbeat. Return
  promptly.** Anything slow goes to a background worker (`BoardServices::bg_submit`).
- `on_rx_sample(s)` -- pinned and every-node actors only: every radio frame from a known peer, with its RSSI
  (received signal strength), channel, and whether it was heard away from the home channel. Record and return.
- `on_call(...)` / `on_call_result(...)` -- units of work between nodes (CALL/REPLY): accept or refuse at once,
  answer later.
- `on_console(line, len)` -- a `POT! ...` console line, for test instruments.
- `stats_json(buf, cap, now)` -- one JSON object for the periodic stats output.
- The destructor -- a portable actor is destroyed when it is fenced or handed over; a fresh instance is
  built for each activation.

### Publishing

- Declare an output once (`node.ns().declare(NsDecl{...})`), then `node.ns().publish(hash, value, now_ms)`.
- **Publish a set of values that belong together with one `now_ms`.** `Node::publish` stamps each call with
  the clock at that moment, so a set published field by field can straddle a millisecond, and readers that
  match fields by timestamp then see two windows (found by the room-presence app).
- **A failed sensor read is `publish_faulty(hash)`, never the old number.** Readers then get quality FAULTY
  and no value until the next publish.
- Output paths a board builds itself (one per peer, say) are hashed with `path_hash_from(system_prefix,
  "node-1a2c/...")`: the board does not know the cluster's name, so the descriptor's `system_prefix` field
  sends the hash state for `potluck://<system>/`.
- **`outputs()` is how the rest of the cell can read you.** Every node holds the image, so every node can
  declare a replica of your output from it; a read of your path anywhere then has an entry to answer from.
  Pinned outputs are declared at boot, every-node and per-link ones when a read first asks (adoption).

### Reading other resources

`node.read(hash, reading, &is_local)` answers from the local namespace: a remote value comes from the replica
**with the age it had when it was last fetched**. There is no push (SUBSCRIBE is reserved, not built), so a
reader refreshes with `node.request_read(owner, hash)` and gets the fresh value on a later read --
refresh-on-read: a remote value lags about one read period. Key consistency on the owner node's sample
timestamp, not on two reads of a sequence number (the room-presence app discarded 109 of 160 sets before it did).
Poll the minimum: ~70 reads a second pushed the heartbeat's 99th-percentile round trip to 42-85 ms.

- **Whom to ask:** the owner node is in the local entry: `const NsEntry* e = node.ns().find(hash)`, then
  `e->owner_node` (0 while nobody owns it, e.g. a portable actor not placed yet). `node.read()` first creates the
  entry if the image says the resource exists, so call it before `find()`.
- **STALE carries a value.** Under the default "informative" policy, `Reading::get_f32()` returns true for a
  STALE reading too -- the value with its true age. If an old value must not count, check
  `reading.quality == Quality::Good` yourself.
- **Before the producer exists:** `node.read()` returns `NsError::NotFound` until some actor (or the image, for
  another node's output) has declared the resource -- the order actors start in on a board is the image's, and
  is not a contract. Treat NotFound like NO_DATA: skip this period, read again next.
- **Units:** `Unit::None` when the unit has no code (degrees per minute, say); say the unit in the output's name.

### Where the boards are, and showing a place on the floor plan

A board the owner has placed on the floor plan holds its own position: `node-<id>/sys/pos/x` and `/y`, f32 in
metres from the plan's top-left corner, x to the right, y down (the cluster file's `"position": [x, y]` on that
board; `apply` compiles it into the signed package as a pinned `builtin:position` actor, so a cell with no PC keeps
it). A board with no position declares neither -- treat NotFound as "not placed". Read them like any resource
(above); each costs the reader one replica while it reads (others adopt them on demand, never at boot).
The same actor holds `node-<id>/sys/room` (u32): the rooms the cluster file names, sorted by name and numbered from
1, so two boards are room-mates when theirs are equal and non-zero. An app that keeps something per link only for a
board's nearest boards says so in its descriptor -- `"peers": {"nearest": K}`, K 1..4 -- and `apply` then works out
each board's set (the other boards in its room, or every board when it has none; by distance between positions,
ties to the lower id; unplaced boards after placed ones, by id; a board with no position takes the lowest ids) and
the board holds it as `node-<id>/sys/near` (bytes: up to four u16 ids, little-endian, nearest first, 0-padded). Read
your own board's set locally; `apply --plan` counts the module's `{peer}` outputs for that set, not every board.

A portable module that works one room at a time says `"per_room": true` in its descriptor; `apply` then runs
each of its actors once per room of the cluster file, as `<name>_<room>` (the room's name lowercased, every run of
other characters one `_`: "Living room" -> `living_room`), placed only among that room's boards, with `<room>` and
its own `act/<name>/` rewritten in its string config values. A cluster file with no rooms keeps the one actor. A portable
actor's eligible list holds at most 8 boards: in a bigger room `apply` keeps the 8 the reconciler would rank first
with every board alive (stable, from the actor's output path) -- the actor runs only on those, though it may read
every board in the room.

A module may ask the dashboard for a **panel** -- a card on its first screen -- from a closed vocabulary, its words as
text only (the dashboard never runs an app's code or markup):
`"panel": {"kind": "room", "output": "act/{actor}/out", "label": "Presence", "on": "Someone is here", "off": "Empty"}`.
`"room"` is for a `per_room` module: one card per room, shaded and saying `on` while the output is true or non-zero,
`off` otherwise, and "Not known now" while the reading is not GOOD. `"value"` shows the output's value under `label`.
`output` is one of the module's own `act/{actor}/` outputs; `label`, `on` and `off` are at most 40 printable characters.
A card appears only for a resource the cluster file's `outside` makes visible (Library tab: Visible or Controllable).

A module that needs something wired to its board says so in **parts**, and brings its own wiring guide:
`"parts": [{"part": "SHT41", "bus": "i2c", "pins": {"sda": "sda_gpio", "scl": "scl_gpio"}, "guide": "VCC to 3V3, ..."}]`.
Each pin names one of the module's own integer `config` fields, the one your actor reads its GPIO from -- so the guide
and the firmware use the same number: the actor's config in the manifest, else the field's default. `bus` is one of
`i2c`, `spi`, `uart`, `adc`, `onewire`, `gpio`; `part` and the roles are at most 40 printable characters, `guide` at
most 400. `potluck apply --plan` refuses a GPIO that two parts claim on one board, one the firmware itself drives (the
hub's `cell.pins`: the frame link, CAN when built in, the status LED, the button) and one outside the chip's 0..48;
which pins a board leaves free beyond that (flash, PSRAM, USB) is its datasheet's to say. The hub lists each app's
parts with their GPIOs (`apps[].parts`), for a wiring view.

A module whose actor takes **key or pointer events from the PC** says so in **takes**: `"takes": ["key"]`,
`["pointer"]` or both (M24). A program gives the hub events for the app (`POST /api/input`, HUB-API.md); the hub
numbers them and delivers them in order as CALLs of `act/<actor>/in`, one batch at a time, sending a batch again when
its answer is late. Your actor implements `on_call`: for that path, read the batch with `parse_input` (which also gives
its stream: a restarted hub numbers from 1 again in a new one) and keep order with `InputOrder` (both in
`pot/input.hpp`, which documents the 8-byte event), act on each event `take(stream, seq)` returns true for, and answer at once with `node().reply_call(from, msg_id, path, Value::of_u32(order.last))` -- the hub sends the
next batch only after that answer. Pin the actor: events go to one board. `builtin:key_echo` is the worked example.

An actor that knows where something is declares seven outputs under one prefix, and the dashboard's floor plan
draws it: `act/<name>/state` (u32: 0 nobody, 1 present but not placed, 2 placed), `x`, `y` (f32, Unit::Metre,
plan coordinates as above), `a`, `b` (f32, Unit::Metre, the semi-axes of the zone), `theta` (f32, Unit::Radian,
from +x toward +y) and `conf` (f32, Unit::Ratio, 0..1). Publish x..theta only while the state is 2 and let them go
STALE otherwise, and list those six with a `?` in front in your descriptor's `outputs`
(`"?act/{actor}/x"`): an output published only sometimes is counted in the namespace like any other, but neither
`apply`'s verify nor the hub's "working" waits for it. Say in your own manual what the zone is; the floor plan
shades it and never draws a point. An actor declares at most 12 outputs (`kMaxOutputs`).

### Checkpoints (portable actors)

`ActorEnv::checkpoint` keeps up to 128 bytes for the next activation -- on another node after a failover, or on
any node after every board has rebooted. Only the node running the actor saves, at most once a second; the newest copy
reaches every eligible node, including one that comes back, and each of them writes it to flash at most once a
minute while it changes (so after a power cut the actor resumes from a state up to a minute old; measured on
three boards: a guest's counter resumed 44 counts behind). Store what the state
depends on with it: a baseline learned on one radio channel is wrong on
another, so the room-presence app keeps the channel in its checkpoint. Put a version byte first, and refuse a
checkpoint you do not recognise.

- **`save()` may return false** -- within a second of the last save, or before this node's claim to the actor
  has settled. Keep a "dirty" flag and save again on a later tick; the guest actor does exactly that.
- **When it matters:** gravity places a portable actor on the node that owns its inputs, so the death that moves
  the actor often takes its input too. A checkpoint pays when the actor's node dies and its inputs live on.

### Board services

`ActorEnv::board` (null on a PC): `led_app(on, r, g, b, blink_ms)` shows an app state on the status LED --
only while the board is green (purple, yellow and red always win, so no app can hide a dead link or an update);
pick blue or cyan -- any other colour (anything with red in it, white included, or plain green) is shown blue, so no
app can look like the board's own status, and it
lapses 5 s after the last call. `led_app` is slot 0 of the board's app states: two apps on one board that both use it
take turns, the last caller winning (on the owner's cell an LED switch and a presence light made the LED alternate).
`led_claim(slot, on, r, g, b, blink_ms)` (slots 1-3; null on firmware before 2026-10-19 -- fall back to `led_app`)
gives an app a slot of its own: its `off` clears only that slot, and the LED shows the most recent `on` that has not
lapsed. The built-in LED switch uses slot 1. `bg_submit(work, arg)` runs a computation on a background worker below
everything the node does, refused when all workers are busy; `bg_yield` must be called every ~50 ms inside
it (the task watchdog watches the idle task: a core kept from it for 5 s resets the chip).

### Limits

For the images `apply` builds, which have PSRAM (the ESP32-S3 N16R8); a board built with `CONFIG_SPIRAM=n` has
the smaller figures in brackets.

| | |
|---|---|
| actors in one package (the declarations a board keeps) | 64 (16); in a tree of cells a package lists up to 255 and each board keeps its own cells' |
| actors running on one node (pinned and every-node) | 8 |
| portable actors in a cell | 16 (none: the reconciler is compiled out) |
| eligible nodes per portable actor | 8 |
| config bytes per actor | 160 (48) |
| outputs per actor | 12 per call of the `outputs()` hook (`kMaxOutputs`) -- per node, and per node and peer for per-link outputs, so a per-link actor can own more in all |
| guests in one package | 4 (boards built with the `sandbox` module) |
| signed package | 32 KB (512 B; 2 KB with the presenting module) |
| namespace entries per node | 128 |
| checkpoint | 128 B |
| actor slot | 384 B |
| static internal RAM, whole node | 64 KB cap (65,536 B); the release image -- Potluck with the sandbox, `present`, `wifi`, `pocket` and the two built-in apps -- uses 63,485 B. `apply --plan` prints your image's figure |
| firmware image | 2 MB (an update slot); the release image is 1,215,488 B |

---

## 5. The descriptor (`potluck-modules.json`)

```json
{"component": "<directory name>", "types": [first, last], "requires": ["sandbox"],
 "modules": [{"module": "ext:<app>/<actor>", "type": <n>, "placement": "pinned|every|portable",
              "outputs": ["{node}/<app>/<output>"],
              "config": [{"name": "...", "kind": "...", "default": ..., "min": ..., "max": ...}],
              "peers": {"nearest": K}, "per_room": true,
              "panel": {"kind": "room|value", "output": "act/{actor}/<output>", "label": "...", "on": "...", "off": "..."},
              "parts": [{"part": "...", "bus": "i2c|spi|uart|adc|onewire|gpio", "pins": {"<role>": "<config field>"},
                         "guide": "..."}],
              "takes": ["key", "pointer"]}]}
```

`component`, `types` and a non-empty `modules` are required; in each module, `module` (starting `ext:`), `type`
(inside the component's block) and `placement`. Everything else is optional.

- **`outputs`** (optional, but give it): the paths an actor of the module publishes, under `potluck://<system>/`,
  with `{node}` for `node-<id>` of the node it runs on and `{actor}` for its name -- `"{node}/hello/peers"`,
  `"act/{actor}/out"`, and `{peer}` for one output per other board (`"{node}/link/{peer}/rssi"`). A `?` in front
  (`"?act/{actor}/x"`) marks an output published only sometimes (section 4). `potluck apply` reads every one back
  before it says VERIFIED (the `{peer}` and `?` ones excepted), and counts them all in each board's namespace
  table: a board holds 128 entries -- its built-ins, every output in the package, its own, and the replicas its
  role reads -- and `apply --plan` prints the count per board and refuses a package that would not fit. An output
  it is not told about it can neither check nor count.
- **`requires`** (optional): Potluck's board modules your component needs in the same image -- `sandbox`,
  `extender`, `wifi`, `present`, `pocket`. `potluck apply` refuses a cluster file that builds your component
  without them, naming what to add to its `firmware.modules`.
- **`peers`** (optional): `{"nearest": K}`, K 1..4 -- the module keeps per-link outputs only for each board's K
  nearest boards (section 4, "Where the boards are").
- **`per_room`** (optional, portable modules only): `true` runs one actor per room of the cluster file (section 4).
- **`panel`** (optional): a card on the dashboard's first screen -- `kind` `"room"` (for a `per_room` module) or
  `"value"`, `output` one of the module's own `act/{actor}/` outputs, and `label`, `on`, `off` as text of at most
  40 printable characters (section 4).
- **Config kinds:** `u8 u16 u32 i8 i16 i32 f32 bool`; `path` (a namespace path, sent as its 32-bit hash);
  `node` (a node id or a label from the manifest); `system_prefix` (no value: the hash state after
  `potluck://<system>/`). Fields are encoded little-endian in the order listed; `min`/`max` are enforced on
  the host before anything is signed. A portable module takes `period_ms` from the manifest without listing it
  in the descriptor: it goes into the standard header (section 3).
- **Type numbers.** 0x00-0x7F are Potluck's built-ins; 0x80-0xFF belong to external components, given out in
  blocks of 16 from the table below, so apps by different authors can be built into one image. Two apps that
  use one type number cannot be: `potluck apply` refuses the cluster file before anything changes and names both
  components (and the firmware build would refuse the merge the same way); an image naming a type the firmware
  has no row for is refused, never silently skipped.

| block | holder |
|---|---|
| 0x80-0x8F | the room-presence app built into the release image |
| 0x90-0x9F | an Amiga 500 emulator app (M27's experiment, 2026-10-26) |
| 0xA0-0xEF | free: ask Potluck's maintainer for a block ([an issue](https://github.com/Tubifix77/potluck/issues)); it is recorded here. Until then, use one provisionally and say so in your README (JSON has no comments; the descriptor loader ignores keys it does not know) |
| 0xF0-0xFF | Potluck's own examples and tests (`pot_hello` is 0xF0) |

---

## 6. Manifests

A manifest (JSON) names the cluster (`system`), its nodes and what they own, and its actors. Your app ships a
*fragment* -- its actors, and the `owns` their `needs` rely on -- and `potluck apply` composes it with the
others into the owner's one package (below):

```json
{"name": "overheat", "module": "ext:my_app/overheat", "latency_class": 4,
 "needs": ["potluck://lab/node-1a2c/hw/die_temp"], "pin": 6700, "config": {...}}
```

(an actor object in the fragment's `actors` list; the whole shape is section 3's `hello.json`).

- **`latency_class`** -- the deadline class of the actor's bindings, from ARCHITECTURE section 4: L0 under
  100 us (same silicon), L1 under 10 ms (same node), L2 under 20 ms (one wired hop), L3 under 500 ms (one
  radio hop), L4 best effort. An actor may only bind resources of its class or tighter, and the build refuses
  a binding the transport cannot meet. Radio apps are typically L4 (the room-presence app's three actors all are);
  control loops use L0-L2 (`manifests/q1-joint.json`, `q1-vent.json` are worked examples). A setpoint written from
  afar but read by a tight loop is declared at its reader's class (L1, with a long `staleness_bound_ms`) and
  written by a looser actor: a resource may be tighter than the actor binding it.
- **`needs`** -- the resources it reads; a portable actor's gravity is higher on a node that owns them. **Every
  path in `needs` must be declared by some node's `owns` list** (or the manifest is refused: "...is neither a
  declared resource nor a binding"):

  ```json
  {"node_id": 6700, "label": "hall", "owns": [
    {"path": "potluck://lab/node-1a2c/hw/die_temp", "unit": "celsius", "kind": "sampled",
     "access": "read", "latency_class": 4, "staleness_bound_ms": 5000}]}
  ```
  This holds for a pinned actor reading a built-in on its own board too: `builtin:die_temp` on node 0x1a2d is
  `potluck://<system>/node-1a2d/hw/die_temp`, and an actor that reads it lists it in `needs`, with the board's
  `owns` declaring it (as above). A package with no actor reading it needs no `owns` for it.
- **`safety_relevant`** (in an `owns` entry, default false) -- mark a resource whose wrong value could hurt
  someone or something. It is then never writable from outside the cell, whatever the owner's cluster file says:
  `apply` refuses such a package, and the board that owns the resource refuses such a write itself.
- **`pin`** -- a hand-written override for where it runs.
- **`config`** -- plain values (numbers, booleans, strings), checked against the descriptor.
- **Built-in modules** you can put in the same manifest (rules in `host/potluck/potluck/deploy.py`):

  | module | placement | config | publishes |
  |---|---|---|---|
  | `builtin:die_temp` | pinned | `period_ms` 200-60000 (1000) | `node-<id>/hw/die_temp`, the chip's temperature, f32 degC |
  | `builtin:ticker` | portable | `period_ms` 50-60000 (100) | `act/<name>/out`, a counter (the reconciler's test actor) |
  | `builtin:svc_client` | pinned | `period_ms` 100-60000; one `needs` of `potluck://<system>/svc/<name>`; and on the actor itself, beside `pin` (never inside `config`), `"on_host_loss"`: `continue` or `hold` (the default) | the PC service's answer |
  | `builtin:led` | pinned | `r` `g` `b` 0-255, `blink`, `period_ms` 200-10000 | nothing shown: a healthy board is always green (GETTING-STARTED.md's LED table); kept so older packages still deploy |
  | `builtin:led_switch` | pinned | `r` `g` `b` 0-255, `blink_ms` 0 or 200-25500 | `node-<id>/led/on`, a writable bool: the status LED shows the colour in green's place while it is true (blue or cyan; any other colour is shown blue; purple, yellow and red still win) |
  | `builtin:inbox` | pinned | `outputs`: `"<name>:<type>[:<unit>] ..."`, 1-12, type `bool` `i32` `u32` `f32` `i64` `u64` `f64`, unit a `potluck.value.Unit` name; `stale_ms` (10000; 0 never stale) | `act/<actor>/<name>`, writable: values a program outside the cell writes through the hub (INTEGRATIONS.md, step 7); NO_DATA until written, STALE past `stale_ms` |
  | `builtin:file_hash` | pinned | M25's test app: when `act/<name>/go` is written, asks the PC's `file` service (its one `svc/<name>` in `needs`) for `file` (config: a plain name) and hashes what arrives with SHA-512 | `act/<name>/go` (a u32, written to start), `act/<name>/bytes`, `act/<name>/state` (0 idle, 1 reading, 2 done, 3 failed), `act/<name>/digest` (u64: the SHA-512's first eight bytes, big-endian), `act/<name>/ms` |
  | `builtin:key_echo` | pinned | M24's test app: takes key and pointer events from the PC on `act/<name>/in` (`POST /api/input`), in order, and echoes them; no config | `act/<name>/key` (the last key: code \| down << 16), `act/<name>/count` (events taken), `act/<name>/gaps` (events missed -- 0 unless something is wrong) |
  | `builtin:speak` | pinned | asks the PC to say its phrase (`text`, 1..120 bytes) when `act/<name>/say` is written; binds one `svc/<name>` path in `needs` (the PC's `speech`) | `act/<name>/say` (a u32, written to ask -- every write is one request), `act/<name>/result` (0 not asked, 1 done, 2 refused, 3 gone) |
  | `builtin:mc_lender` / `builtin:mc_job` | pinned | a demonstration job on borrowed idle cores (`slots`; `units`, `samples`, `seed`, `lend`, `start_after_ms`, `control`) | `node-<id>/job/mc/pi`; with `"control": true` also `node-<id>/job/mc/run`, a u32 written to run with lending (1), alone (2), or stop (3) |
  | `builtin:fault` | pinned | `panic_after_ms` | nothing: it crashes the node on purpose, to test reverts |
  | `guest:<name>` | portable | section 9 | `act/<actor>/<output>` |

  `apply` adds three more built-ins itself, from the cluster file, so you do not write them: `builtin:position`
  (a board's `position`, `room` and nearest boards), `builtin:present` (the cluster file's `present`) and
  `builtin:foreman` (a tree's `cells`).

- **One package per cluster, composed from fragments.** Deploying a package replaces the previous one, so the
  package lists *every* actor the cluster should run. Nobody writes it by hand: `potluck apply` composes it from
  the fragments the cluster file names. The cluster's boards are its nodes; a fragment's board is matched to one
  of them by node id or by label, and its paths (`node-<id>`, `potluck://<system>/`) are rewritten to that board
  and that cluster. A board in a fragment that nothing in it needs (no `owns`, no actor pinned to it) is taken as
  the author's own cell and ignored, so a fragment written on your three boards runs on someone's five. Two
  fragments with an actor of the same name, a binding that differs, or a board the cluster lacks but the
  fragment needs are refused before anything changes.
- **The cluster's name** is the cluster file's `system` (1-31 characters, no spaces or `/`). The boards name
  their built-in resources under it (`potluck://<system>/node-<id>/sys/uptime`) once the package says so; a
  cluster called `lab` carries nothing extra.

---

## 7. Building, rolling out, deploying: `potluck apply`

A cluster file says what the cell should be; `apply` makes it so. From `host/potluck/`, with the ESP-IDF Python
(it has `pyserial`), in PowerShell 7 or Git Bash (`potluck apply ...` is the same command, once
`python -m pip install -e host/potluck` has installed the package):

```
python -m potluck.apply <cluster.json> --plan       # check everything, change nothing
python -m potluck.apply <cluster.json>              # make the cell what the file says, then verify it
python -m potluck.apply <cluster.json> inventory    # what every board runs: firmware, package, actors, counters
```

A cluster file for the hello app on three boards (the preset [`examples/cells/hello.json`](examples/cells/hello.json),
which lives in the Potluck checkout, so `"git": "../.."` and `"rev": "HEAD"` are the release you checked out --
your own cluster file names Potluck's release tag instead, as in GETTING-STARTED.md; paths are relative to the
file; the ids and the port are examples):

```json
{"schema": "potluck-cluster/1", "system": "lab", "port": "COM6",
 "boards": [{"node": "1a2b", "label": "kitchen"}, {"node": "1a2c", "label": "hall"}, {"node": "1a2d", "label": "garden"}],
 "keys": {"ca": "../../keys/ca.pub", "deploy_key": "../../keys/deploy.key",
          "deploy_cert": "../../keys/deploy.cert", "deploy_bcert": "../../keys/deploy.bcert"},
 "firmware": {"potluck": {"git": "../..", "rev": "HEAD"}, "extra": ["CONFIG_POT_WASM=y"]},
 "apps": [{"name": "hello", "git": "../..", "rev": "HEAD", "components": "examples/apps",
           "manifests": ["examples/apps/hello.json"]}],
 "verify": {"timeout_s": 120}}
```

| key | what it says |
|---|---|
| `system` | the cluster's name (section 6) |
| `port` | the serial port of the PC's adapter, wired to one of the boards (`COM6`; on Linux, untested, `/dev/ttyUSB0`; `--port` overrides it), and optionally `baud` (921600) |
| `boards` | every board: its node id in hex and a label (and optionally a `room`, a `position` on the floor plan, and -- past one cell -- its `cells`: one cell name, or two; a board admits only boards of its own cells; `"foreman": true` for a board on mains power that may hold its cell's foreman role, which needs exactly one cell, one with a `parent`; `"stub": true` for a board the PC hosts for a tree's bench tests, ids f000..fdff, never reached through the cable); the id is the one its certificate names -- the last two bytes of its MAC unless the cell assigned another at enrolment (`potluck.enrol --node-id`, or the lowest free id from 0x0100 up when the MAC's id is taken) |
| `cells` | a tree of cells (one signed package for the whole tree; each board keeps its cells' part; `apply --plan` checks every board's peers and namespace, and in a tree `apply` verifies through the cable only what the cable reaches -- the other cells take the package and the firmware by catch-up; `--partial` lets boards that do not answer catch up): per cell name, `parent` (the upper cell its foreman joins -- every federating cell needs a board marked `foreman`), `window_ms` (its own death window, 0..25500 in steps of 100: an upper cell's is longer), `period_ms` (how often its foreman publishes the summary, default 2000) and `export` -- what goes up, never a mirror: `{"warmest": {"max": "hw/die_temp", "unit": "celsius"}}` (max, min, sum, avg or count over every board's `node-<id>/<suffix>`), or `{"a_temp": {"copy": "node-0100/hw/die_temp", "type": "f32", "unit": "celsius"}}` for one resource; `alive`, the boards the foreman sees, is always one. Each export is `potluck://<system>/cell-<name>/<export>`, its age the oldest input's and its quality the worst input's. `apply --plan` counts every board's peers (19 at most: its cells, one seat per child cell in the upper cell, the PC on the cabled board's cells) and refuses a tree that does not fit, naming the board; a binding across cells is L4 |
| `keys` | `ca`: the CA's public key (`ca.pub`); `deploy_key`, `deploy_cert`, `deploy_bcert`: the deploy key and its two certificates ([GETTING-STARTED.md](GETTING-STARTED.md), step 2, makes them). The CA's secret key is never named here |
| `firmware` | Potluck at a commit or tag (`potluck`: `{"git": ..., "rev": ...}`), its optional board modules by name (`modules`: `sandbox` -- guests; `wifi` -- Potluck's own Wi-Fi station, which puts the board on your network, you giving it the network with `tools/wifi_set.py <port> <file>`; `present` -- the cell presenting itself to an MQTT broker; `pocket` -- the board's own access point and a read-only page for a phone in reach ([M0-RUNBOOK.md](M0-RUNBOOK.md), section 16); each one costs flash and RAM, and some open the cell to the outside, so none is in an image unless named -- `"extra": ["CONFIG_POT_WASM=y"]` still means `sandbox`), extra Kconfig lines (`extra`, such as `"CONFIG_SPIRAM=n"` for boards without PSRAM), and `extender` if a board should be a Wi-Fi extender: `{"git": ..., "rev": ..., "components": "firmware/components"}`, the extender app's repository, like an app (that repository is not public). Every board gets the same image; a board becomes an extender only if it holds Wi-Fi credentials, the others are ordinary members. Or `{"image": ..., "elf": ...}` for a build made elsewhere, or `null` to leave the firmware alone |
| `apps` | each app: `name`, `git` (a local repository or a URL), `rev` (a commit or a tag), `components` (a folder built into the firmware; each of its direct subfolders with a `CMakeLists.txt` and a `potluck-modules.json` is an app component, and that descriptor is loaded -- nothing deeper is looked at), `manifests` (its fragments), `enabled` (false: built in, but adds nothing to the package), and in a tree `cell` -- the cell its portable actors live in (a portable actor lives in one cell; one whose boards span cells is refused) |
| `package` | more fragments (`"manifests": [path, or {"manifest": path, "root": dir for its relative paths, "cell": in a tree, as an app's}]`), more descriptors (`"modules"`), and `guests_root` |
| `verify` | `timeout_s`, and `read`: more paths to read back |
| `present` | the cell presents itself to an MQTT broker, by Home Assistant's discovery: `{"broker": "192.168.1.50[:port]", "keepalive_s": 10, "period_ms": 2000}` (an IPv4 address; port 1883 by default, 8883 with TLS), and `"tls_ca": "<file>"` -- the broker's CA (PEM or DER, relative to the cluster file) -- to connect with TLS, the CA pinned by the package's signature. A broker login is given to each board on its console ([HOME-ASSISTANT.md](HOME-ASSISTANT.md)). A cell that presents is built with Wi-Fi's AMPDU transmit aggregation off on every board (a board presenting over its own station with it on wedges its radio; `"extra": ["CONFIG_ESP_WIFI_AMPDU_TX_ENABLED=y"]` overrides, at that risk), which costs an extender's hotspot some throughput. Needs `present` in `firmware.modules`, and a board on the network (an extender, or the `wifi` module). It presents only what `outside` names ([M0-RUNBOOK.md](M0-RUNBOOK.md), section 16). `"hub": true`: the PC's placement node presents whenever it is in the cell and the boards take the role back when it goes -- the same devices in Home Assistant, never two presenters; needs a placement host in the cell (`kind: "host"`, `placement: true`). In a tree, `"cell"` names the cell that presents |
| `pc_services` | M23: the named services the hub's PC offers the cell, answered by the hub itself -- `["time", "speech"]`: `time` is Unix time in milliseconds (U64); `speech` says a board's text (1..200 bytes of UTF-8, the CALL's arguments) with the PC's own voice and answers 1 (said) or 0 (not). Nothing is served unless named here: it is the PC's user's choice. The package must still have the PC own each path (`potluck://<system>/svc/<name>`, on a node of `kind: "host"`) for a board's caller to compile |
| `bulk` | M25: `{"folder": "<path>", "rate"?: <chunks a second, 1..200, default 15>}` -- the folder the PC's `file` service offers the cell, read-only (relative to the cluster file, or absolute). A board's app asks for a file by name (`pot/bulk.hpp`: plain names only, up to 1 MiB) and the hub pushes it to the app's input in chunks; `pc_services` names `"file"` only with a folder |
| `outside` | what the cell presents over MQTT (Home Assistant's discovery, through the `present` module), and what may be written from outside, per resource: `{"node-1a2c/hw/die_temp": "read", "node-1a2c/led/on": "read_write"}`, a path with or without `potluck://<system>/` (the dashboard's Library tab writes the same: Hidden, Visible, Controllable). Nothing is presented unless named here. `read_write` needs a resource some node declares in `owns` with write access, and never one marked `safety_relevant`. **Writes over MQTT** are checked by the owning board against exactly that -- `read_write` and not `safety_relevant` -- so anyone who can publish on your broker can write every resource you made `read_write`: give the broker a login and use TLS (`present.tls_ca`). For writes through the hub's API only, `{"mode": "read_write", "writers": ["kitchen-panel"]}` names the programs that may make them, by the name each was paired or logged in with; it does not apply to Home Assistant, which writes over MQTT. A program paired with the hub reads the hub's whole model, every resource, whatever `outside` says: pairing is your approval. Your choice, so only the cluster file says it: a fragment with `outside` is refused |
| `plan` | the floor plan the dashboard draws: `{"width": m, "height": m, "rooms": [{"name", "x", "y", "w", "h"}]}`, metres from its top-left corner (the dashboard's Plan tab writes it) |

What `apply` does, in order:

1. **Plan, changing nothing.** It exports every source with `git archive` into a cache (once per commit), loads
   the descriptors, composes and compiles the package (every actor's config checked, every guest's author
   signature checked under the cluster CA), finds the keys, builds the firmware -- or reuses the cached build of
   the same firmware sources, so a commit that changes only documents or host tools sends nothing to the boards --
   and reads every board's inventory (each board's optional modules with what each is doing: `sandbox:active`,
   `extender:idle (no credentials)`), prints what each module costs the image -- flash, internal RAM and PSRAM, from
   the build's own size report -- and how much of Potluck's 64 KB internal budget is left, then says what it would change: which boards get firmware, what runs now
   and what the package replaces it with, and whether the boards must reboot for it. Any problem stops it here,
   saying why: `REFUSED, nothing changed: ...`
   (exit code 2). A firmware image larger than its 2 MB update slot, a package over the node image's limits, a
   guest whose signature does not verify, a type number two apps share, a board that does not answer -- all
   stop here.
2. **Firmware, only where it differs** (by the image's ELF SHA-256): over the cell, one board at a time, the board
   on the cable last, at a counter above every board's floor. A board keeps an image only after 100 heartbeats
   with a radio peer, else rolls back; a rollback stops the rollout there. About two minutes a board.
3. **The package**, signed at a counter above every board's and sent through the cabled board to the whole cell.
   Each board takes it in place when it can -- actors whose declaration is unchanged keep running with their
   state, the rest stop and their outputs leave the namespace, new ones start (a changed portable actor from its
   checkpoint) -- on trial exactly as before: a
   board that crashes on it reverts by itself. Only a change to a guest's code needs a reboot, and the board does
   that itself. A board that was off gets the package from a peer when it comes back.
4. **Verify**: every board confirmed on the package, with the actors the package gives it; every portable actor
   running once in the cell; every output read back GOOD -- not just with a value; a fresh actor that learns
   before it publishes gets until `timeout_s`. The outputs it reads are the ones it can know: each node's `owns`,
   the built-ins' (`die_temp`, a ticker's, a guest's), the paths your descriptor lists in `outputs` (section 5)
   and any in `verify.read`. An app whose descriptor lists none is not checked beyond "it runs". Then a report: what each step
   took, which boards took the package live, and which rebooted. `VERIFIED` (exit 0) or `NOT VERIFIED` and why
   (exit 1).

Measured on three boards: from minimal Potluck to an image with the room-presence app, the extender app and a
guest, 390 s from command to verified, 366 s of it the firmware on three boards; taking the room-presence app off
again, 23.5 s, no board rebooting, the guest's count carrying on; a fresh firmware build adds about 9 minutes the
first time.

- **Builds need PowerShell 7 (`pwsh`)**: `apply` runs `tools/build_firmware.ps1` from the exported tree, and
  Windows PowerShell 5.1 fails inside ESP-IDF's `export.ps1`. It finds ESP-IDF v6.0.2 through `IDF_PATH`. The
  output goes in the cache (`%LOCALAPPDATA%\potluck\cache` on Windows, `~/.cache/potluck` elsewhere; `--cache`
  moves it). An app built from a working tree it is still editing is how the first apps lost two days to
  "transient" failures (section 10); `apply` cannot do that, and warns when `HEAD` has uncommitted changes it will
  not build.
- **See it run:** `python -m potluck.ctl --port COM6 --node <cabled board> watch potluck://<system>/act/<name>/<output>`
  (`COM6` stands for your adapter's port)
  reads an output once a second, with its age and quality; `--count N` stops after N reads; `--system` names the
  cluster for shorthand paths. The first read of another board's output is often NO_DATA or STALE: the cabled
  board answers from its copy and refreshes the copy as it does (section 4), so the next read is fresh.
- **Removing every app's actors but yours** is what a cluster file with only your app does: one package per
  cluster. To add your app to a cell that runs others, add it to that cell's cluster file beside them.
- **The tools underneath** are still there for one step at a time: `potluck.fw` (firmware over the cell),
  `potluck.signing` and `potluck.ctl deploy` (a package), `potluck.reconcile` (where portable actors would run).
  M0-RUNBOOK.md shows them.

### On your own boards

The commands in this manual use an example cell; nothing in Potluck is about three boards: list as many as you
have. A board's id is the one its certificate names -- by default the last two bytes of its MAC, but the cell may
assign another at enrolment, so an app takes ids from the runtime, never from a MAC; the board on the cable is the one your USB-serial adapter
is wired to. On boards you just flashed, both counters start at 0 -- `apply` takes them from there. Your images
contain only what the cluster file builds in: an image without an app's component removes that app from the
boards it lands on (the package that names its actors is then refused there, and the board runs on without
them).

### Sharing your app

Put the component, its descriptor, a manifest fragment and its tests (section 8) in a public repository, and tag
a release. Someone running it adds one entry to their cluster file's `apps` -- your repository's URL, the tag,
`"components": "firmware/components"`, `"manifests": [...]` -- and runs `apply`. Before you publish, get a type
block recorded in section 5: two apps that use the same type numbers cannot be built into one image, and `apply`
refuses the pair by name.

---

## 8. Testing

- **On a PC first, with Potluck's test kit.** [`tests/testkit`](tests/testkit) builds Potluck's portable core --
  the boards' own `Node`, namespace, image parser and actor API, compiled for the PC -- and gives your tests a
  `main()`, the header-only `TEST`/`CHECK` harness and a rig: one node, a clock you move, the LED captured, a
  checkpoint store for a portable actor, background work run at once. Your repository's `tests/CMakeLists.txt`:

  ```cmake
  cmake_minimum_required(VERSION 3.20)
  project(my_app_tests C CXX)
  set(POTLUCK_DIR "$ENV{POTLUCK_DIR}" CACHE PATH "a Potluck checkout")
  add_subdirectory("${POTLUCK_DIR}/tests/testkit" potluck_testkit)
  enable_testing()
  potluck_app_test(my_app_tests
    SOURCES  test_my_actor.cpp ../firmware/components/pot_my/src/my.cpp
    INCLUDES ../firmware/components/pot_my/include)
  ```

  and a test:

  ```cpp
  #include "pot/testkit.hpp"
  #include "test_harness.hpp"
  #include "my/my.hpp"

  TEST(my_app, publishes_after_one_period) {
      pot::testkit::Rig rig;                      // node 0x0100 in cluster "lab", the clock at 0
      CHECK(rig.declare_bytes(kDecl, sizeof(kDecl)));  // your actor's declaration (below)
      CHECK(rig.start(my::row()));                // your row's check(), create(), start()
      rig.advance(1000);                          // 1000 ms, ticking the actor every millisecond
      CHECK(rig.read("potluck://lab/node-0100/my/out").quality == pot::Quality::Good);
  }
  ```

  The declaration is what the host compiles for one actor -- node `u16` (little-endian), type `u8`, config length
  `u8`, then the config -- and Potluck prints it for your manifest, ready to paste:
  `python -m potluck.deploy decl <manifest> --modules <your descriptor> [--actor <name>]`. A **pinned** actor's
  declaration carries its pin, so build the rig as that node -- `pot::testkit::Rig rig(0x1a2d);` -- and read its
  outputs under `node-1a2d`; the default node, 0x0100, suits an every-node actor.

  Run it with `POTLUCK_DIR` set to your Potluck checkout (an environment variable, or `-DPOTLUCK_DIR=` to CMake). On Windows Potluck's script finds CMake, Ninja and a
  compiler for you: `tools\run_host_tests.ps1 -Source <your repo>\tests -BuildDir <your repo>\build\tests`; elsewhere (untested)
  `cmake -S tests -B build && cmake --build build && ctest --test-dir build`. `rig.feed(path, value, type)`
  publishes an input as another actor would; `rig.start(row, &why, true)` gives a portable actor the checkpoint
  store, and `rig.restart()` builds a fresh instance from it, as a failover or a reboot does. The complete example
  is [`examples/apps/pot_hello/tests`](examples/apps/pot_hello/tests), which Potluck's own suite also builds and
  runs, so the kit cannot drift from the core. Keep your pure logic (detectors, statistics) in functions with no
  Potluck dependency and test it the same way.
- **Share the bytes.** Check that the host tools compile your manifest to the declaration your parser accepts
  (`host/potluck/tests/test_examples.py` against [`tests/test_hello_app.cpp`](tests/test_hello_app.cpp)). From your own repository, put
  `<potluck>/host/potluck` on `sys.path` and `import potluck`; load your descriptor with
  `potluck.modules.load(path)` before compiling, and check placement with
  `python -m potluck.reconcile <manifest> --modules <descriptor>`.
- **On the boards.** Record consoles with `tools/json_capture.py <port> <file> <seconds>` (JSON lines only --
  use it on any board that holds Wi-Fi credentials) or `tools/console_tap.py` (everything, without resetting
  the board). CTest counts one test per executable; the kit's own output lists every case. Read the first ten minutes of any long run with your report tool before trusting the rest.
  **Count deaths from the `peer_dead` events, never from sampled state.** Test with at least three boards: two
  hide multi-node bugs. Reboot each board in turn -- the room-presence app's reboot tests found four Potluck faults.
- **Run the cheapest experiment that can kill the idea first**, with its kill criterion written down. The
  room-presence app began with a recording of every link's signal strength while a person walked a scripted
  route through the room, before any detector existed (kill it if no link reacted beyond the empty room's own
  noise); the extender app began by measuring its hotspot's throughput and latency beside a running cell.

---

## 9. Guests (WebAssembly)

A guest exports `tick(now_ms)` (and optionally `init()`, run once per activation after its checkpoint is
available) and may import nine functions from `potluck` -- the API is
[`guests/potluck_guest.rs`](guests/potluck_guest.rs), with two example guests beside it. The owner's
manifest wires its inputs (by index, in `needs` order) and its outputs (`"outputs": "out:f32 alarm:i32"`,
published to `act/<actor>/<name>`), and sets its fuel per tick and its memory (0 or 64 KiB).

- **Author:** `python -m potluck.guest sign <module.wasm> --key <author.key> --cert <author cert> --out <bundle>`
  (the owner certifies the author once: `python -m potluck.enrol --guest-cert ...`).
- **Owner:** name it in a fragment as `"module": "guest:<name>"` with `"bundle"` (a path relative to the
  fragment's root -- the cluster file's directory, or the `root` given with the fragment) and its `"sha256"`, and
  run `apply`. It checks the author's signature before anything changes, and every node checks it again before
  it accepts the package and before the guest first runs. A guest's code is the one thing a package cannot change
  in place: a package that changes it reboots the boards into it (on trial, as always).
- **What it inherits:** a tick that faults (out of fuel, out of bounds, a publish to an output it was not given)
  publishes nothing and its outputs read FAULTY until a tick succeeds; three in a row quarantine it until it is
  next activated; a guest can never write anything. Guests on one board run one at a time, so a guest can hold
  up another on the same board for its whole fuel budget.

Full steps: [M0-RUNBOOK.md](M0-RUNBOOK.md), section 12.

### Publishing a guest for any cell (the store)

A guest can be installed by owners you have never met, from your GitHub repository, through their dashboard's
Library ("Apps from GitHub", off until the owner turns it on). Put `potluck-app.json` at the repository's root:

```json
{"type": "potluck-app-v1", "name": "overheat_alarm", "description": "warns when a temperature runs high",
 "author_key": "<your Ed25519 public key, hex>", "module": "build/overheat_alarm.wasm",
 "sha256": "<the module's SHA-256>", "sig": "<your Ed25519 signature over the module, hex>",
 "inputs": [{"name": "temperature", "type": "f32", "unit": "celsius"}],
 "outputs": "out:f32 alarm:i32 count:i32", "fuel": 100000, "period_ms": 1000}
```

`sig` is what `potluck.guest sign` puts in a bundle: Ed25519 over `potluck-guest-v1\0` followed by the module's
SHA-512. There is no certificate and there are no paths: on the first install the owner's cell pins your key to your
repository and certifies it for that cell, and the owner binds each input to one of their resources (of the same
type; your `unit` is shown to them). If your key ever changes, every cell refuses the update until its owner removes
the app and installs it again -- keep the key. Publish releases: the owner may name a tag, and an install is pinned to
the module's hash. `name` is lowercase letters, digits and `_`, starts with a letter, at most 31 characters; at
most 8 inputs, each `i32` or `f32`; `fuel` 1,000..50,000,000; `period_ms` 100..3,600,000. From the repository's
root, with any Python 3.10 or newer:

```
python tools/store_feed.py keygen <author key file>
python tools/store_feed.py app <module.wasm> --key <author key file> --name <name> --outputs "out:f32 alarm:i32" --out potluck-app.json
```

`keygen` writes the secret key (hex, one line -- keep it out of version control) and prints the public half; `app`
writes a signed `potluck-app.json` for the module, with optional `--input name:type[:unit]` (repeated),
`--description`, `--fuel` and `--period-ms`. It writes `module` as the file's bare name: put the module beside
`potluck-app.json`, or edit the path to where it lies in the repository. An app may sit in a folder of a repository (the owner gives the
folder too). The
store's own list is [Tubifix77/potluck-store](https://github.com/Tubifix77/potluck-store): `feed.json`, signed by the
store key Potluck carries; a pull request adds an app to it.

---

## 10. Lessons from the first two apps

**Building**

- **Build another repository only from `git archive <commit>`**, never from its working tree: two "transient"
  build failures were half-finished edits in it.
- **Treat Potluck as read-only from your app, and your app as read-only from Potluck.** What one needs from the
  other goes in as a change request; bugs found in the other are reported, not patched.
- **After changing `sdkconfig.defaults`, regenerate `sdkconfig` and check the value**: an existing `sdkconfig`
  wins over the defaults file without a word.

**Timing and the radio**

- **Never print from `tick()` or `on_rx_sample()`.** ESP-IDF's console blocks ~10 ms a line at 115200 baud; a
  per-frame log on the link task moved heartbeat round trips from 4-6 ms to 16-22 ms. Print from a task of your
  own through a buffer, and count what you drop.
- **Sharing the radio costs jitter, not liveness.** Beside a streaming Wi-Fi repeater the cell kept every
  heartbeat window, with round trips up to 85-110 ms at the 99th percentile.
- **An app that owns Wi-Fi attaches ESP-NOW instead of starting it** (`EspNowConfig::attach`), and Potluck then
  follows the app's channel instead of choosing one. Every Wi-Fi station scan takes the one radio off the cell's
  channel for seconds: back off scans, and let Potluck widen the node's death window while the station hunts.
- **Never move Wi-Fi or lwIP buffers to PSRAM on a board that runs ESP-NOW.** It forces static transmit
  buffers, which with AMPDU wedged ESP-NOW about 2.5-5 minutes into every boot.
- **Signal strength depends on the channel.** Tag learned radio state with `RxSample::channel`, and drop samples
  marked `RxSample::off_home` (heard during a channel sweep, not a real move).

**State and failover**

- A portable actor that moves without a checkpoint starts cold: the room-presence app's outputs read NO_DATA for
  ~66 s while it relearned. Save a checkpoint, and put in it everything the state was learned under.
- A board that comes back used to take the actor over before it had heard any checkpoint; Potluck hands the
  newest one to a node the moment it reappears (resume in ~0.5 s).

**Credentials**

- Wi-Fi names and passwords are the owner's to type; tools record only JSON status lines from a board that
  holds them, and its Wi-Fi log is kept at WARN so network names never reach a capture.

**What the apps made Potluck add** -- the platform grew from its apps, which is the point. The extender app
brought channel following, the single-hop relay, the death window a node can widen at run time, a busy flag in
the heartbeat, the PSRAM build and the "application owns the radio" seam. The room-presence app brought the
external-component seam itself, radio samples for actors, several outputs per actor with replicas on demand,
checkpoints and the app LED.

---

## 11. The contract your app inherits

Potluck's failure modes -- the 600 ms death window, stale values delivered marked, a portable actor's move,
at-least-once activation during a partition, firmware updates taking a board out for ~15 s -- are listed in the
README's [failure contract](README.md#the-failure-contract). A package change adds one: an actor whose
declaration changed (its config or its placement) is stopped and a new instance started, its outputs removed and
declared again -- readers see NO_DATA until it publishes; a portable one resumes from its checkpoint, as after a
failover. An actor whose declaration did not change is not touched. An actor whose output path changes (another
name, another cluster name) is a new actor, and starts without a checkpoint. A checkpoint restored after every
board rebooted is up to a minute old. And two restarts protect every board whatever its apps do: a board whose
every radio send fails for 60 s restarts itself (on a busy 2.4 GHz band a board's radio can stop accepting sends,
and this is what recovers it), and a task that keeps a core from its idle task for 5 s resets the chip (the task
watchdog -- why `bg_yield` exists). The reason for a board's last reset is its `node-<id>/sys/reset` resource.
Your app inherits every one of them. The safety
line holds for apps too: Potluck must never be the only thing between a motor and a person
([ARCHITECTURE.md](ARCHITECTURE.md), section 12).
