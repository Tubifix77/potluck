# Developing apps for Potluck

**Who this is for:** someone who has vanilla Potluck running on ESP32-S3 boards they own -- however many -- and
wants to write an app of their own, run it on those boards, and later share it on GitHub for others to run on
theirs. You are the owner of your cluster: your keys, your boards, your packages. Not running Potluck yet? Start
with [GETTING-STARTED.md](GETTING-STARTED.md).

For anyone who wants to make an app for a Potluck cluster: code that reads the cluster's resources
(sensors, radio, other apps' outputs), computes, and publishes results the rest of the cluster can read.
It brings together what was spread across header comments, the [decision log](M0-LOG.md) and the
experience of the first two apps built on Potluck: **passive-sensor**, which detects a person in a room
from how their body weakens the boards' own radio links, and **poor-mans-extender**, a Wi-Fi extender that
shares the radio with Potluck. Both live in their own (private) repositories, the way an app should.

**Status of this manual (2026-10-11, rewritten for M12).** One command now takes a cell from a cluster file to
running and verified: `python -m potluck.apply <cluster.json>` builds the firmware, rolls it out where it differs,
signs and deploys the package and checks every output -- the counters, the order and the variants are the tool's,
not yours (section 7). A package changes on the boards without a reboot, and checkpoints survive one. Apps test
themselves in their own repositories with Potluck's test kit (section 8). Everything shown here is tested: the
worked example in section 3 is built for the boards and tested on a PC by Potluck's own test suites, and section
7's commands are the ones that brought a room sensor, a Wi-Fi extender and a guest onto three boards in one run.

The manual was tested on readers with no other context (2026-10-11, twice); every place they got stuck is fixed
in it.

Terms used throughout:

- **Node** -- one board (or a PC running `pot_hostnode`), with a 16-bit id such as `0x7368`.
- **Cell** -- the nodes that hear each other over ESP-NOW (Espressif's board-to-board radio protocol).
- **Namespace** -- the cluster's shared map of named resources, `potluck://<system>/...`; every read
  returns a value *with* its age and quality ([section 4's tuple](ARCHITECTURE.md)).
- **Actor** -- a unit of app code. Potluck decides which node runs it and drives it.
- **Package** -- a signed manifest naming every actor the cluster runs; deploying one replaces the last.

---

## 1. Two ways to write an app

| | **Native actor** (an external component) | **Guest** (a WebAssembly module, since M7) |
|---|---|---|
| Language | C++17, against `pot/actor.hpp` | anything that compiles to WebAssembly 1.0; the example guests are Rust |
| Speed | native | ~50x slower than native, measured on the S3 |
| Can touch | everything an actor can: the namespace, calls to other nodes, every radio frame's signal strength, the status LED, background workers | only its wired inputs and outputs, a 128-byte checkpoint, a log and a clock -- nine imports, nothing else |
| Trust | compiled into the owner's signed firmware: the owner trusts its author with the board | runs sandboxed: the owner trusts its author only with its own memory and its fuel |
| Shipping | rebuild the firmware with your component, roll it out over the radio, then deploy a package | sign the module once as its author; the owner names it in a package -- no firmware change |
| Cost per instance | its slot (384 B) and whatever it allocates | ~80 KB of heap (its 64 KiB memory page, in PSRAM) |
| Needs | nothing extra | boards built with `CONFIG_POT_WASM`, an author certificate from the owner |

Choose **native** when the app needs radio samples, the LED, timing below a tick, or speed -- passive-sensor
is native for all four reasons. Choose a **guest** when the code comes from someone the owner does not want
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

1. Your actors are C++ classes in an [ESP-IDF component](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/build-system.html)
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
lights its status LED cyan while it sees none. Read its four files in this order:

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

And its manifest, [`examples/apps/hello.json`](examples/apps/hello.json):

```json
{"name": "hello", "module": "ext:hello/peers", "latency_class": 4, "config": {"period_ms": 5000}}
```

Put it on your boards with the preset [`examples/cells/hello.json`](examples/cells/hello.json) -- your boards' ids
and your adapter's port in it, then `python -m potluck.apply ../../examples/cells/hello.json` from `host/potluck/`
(section 7). The example adds 1,152 bytes to the image.

It is tested twice before any board: [`examples/apps/pot_hello/tests`](examples/apps/pot_hello/tests) runs the
actor on a real `pot::Node` on the PC with Potluck's test kit, and `host/potluck/tests/test_examples.py` checks
that the host tools compile `hello.json` to exactly the bytes the C++ test parses. Copy that pattern (section 8).

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
   give each output's full path as a `path` field (passive-sensor's fusion actor takes
   `"confidence": "potluck://lab/act/rf_fusion/confidence"`), and the host sends its hash.

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
  match fields by timestamp then see two windows (found by passive-sensor).
- **A failed sensor read is `publish_faulty(hash)`, never the old number.** Readers then get quality FAULTY
  and no value until the next publish.
- Output paths a board builds itself (one per peer, say) are hashed with `path_hash_from(system_prefix,
  "node-7368/...")`: the board does not know the cluster's name, so the descriptor's `system_prefix` field
  sends the hash state for `potluck://<system>/`.
- **`outputs()` is how the rest of the cell can read you.** Every node holds the image, so every node can
  declare a replica of your output from it; a read of your path anywhere then has an entry to answer from.
  Pinned outputs are declared at boot, every-node and per-link ones when a read first asks (adoption).

### Reading other resources

`node.read(hash, reading, &is_local)` answers from the local namespace: a remote value comes from the replica
**with the age it had when it was last fetched**. There is no push (SUBSCRIBE is reserved, not built), so a
reader refreshes with `node.request_read(owner, hash)` and gets the fresh value on a later read --
refresh-on-read: a remote value lags about one read period. Key consistency on the owner's sample
timestamp, not on two reads of a sequence number (passive-sensor discarded 109 of 160 sets before it did).
Poll the minimum: ~70 reads a second pushed the heartbeat's 99th-percentile round trip to 42-85 ms.

- **Whom to ask:** the owner is in the local entry: `const NsEntry* e = node.ns().find(hash)`, then
  `e->owner_node` (0 while nobody owns it, e.g. a portable actor not placed yet). `node.read()` first creates the
  entry if the image says the resource exists, so call it before `find()`.
- **STALE carries a value.** Under the default "informative" policy, `Reading::get_f32()` returns true for a
  STALE reading too -- the value with its true age. If an old value must not count, check
  `reading.quality == Quality::Good` yourself.
- **Before the producer exists:** `node.read()` returns `NsError::NotFound` until some actor (or the image, for
  another node's output) has declared the resource -- the order actors start in on a board is the image's, and
  is not a contract. Treat NotFound like NO_DATA: skip this period, read again next.
- **Units:** `Unit::None` when the unit has no code (degrees per minute, say); say the unit in the output's name.

### Checkpoints (portable actors)

`ActorEnv::checkpoint` keeps up to 128 bytes for the next activation -- on another node after a failover, or on
any node after every board has rebooted. Only the current owner saves, at most once a second; the newest copy
reaches every eligible node, including one that comes back, and each of them writes it to flash at most once a
minute while it changes (so after a power cut the actor resumes from a state up to a minute old; measured on
three boards: a guest's counter resumed 44 counts behind, `ck_restored` 1 on every board). Store what the state
depends on with it: a baseline learned on one radio channel is wrong on
another, so passive-sensor keeps the channel in its checkpoint. Put a version byte first, and refuse a
checkpoint you do not recognise.

- **`save()` may return false** -- within a second of the last save, or before this node's claim to the actor
  has settled. Keep a "dirty" flag and save again on a later tick; the guest actor does exactly that.
- **When it matters:** gravity places a portable actor on the node that owns its inputs, so the death that moves
  the actor often takes its input too. A checkpoint pays when the actor's node dies and its inputs live on.

### Board services

`ActorEnv::board` (null on a PC): `led_app(on, r, g, b, blink_ms)` shows an app state on the status LED --
only while every link is healthy (blue, yellow and red always win, so no app can hide a dead link), and it
lapses 5 s after the last call; `bg_submit(work, arg)` runs a computation on a background worker below
everything the node does, refused when all workers are busy; `bg_yield` must be called every ~50 ms inside
it (the task watchdog watches the idle task).

### Limits

| | |
|---|---|
| actors in one image | 16 |
| pinned actors on one node | 8 |
| portable actors in a cell | 8 |
| eligible nodes per portable actor | 8 |
| config bytes per actor | 48 |
| outputs per actor | 8 per call of the `outputs()` hook -- per node, and per node and peer for per-link outputs, so a per-link actor can own more in all |
| namespace entries per node | 128 |
| checkpoint | 128 B |
| actor slot | 384 B |
| static internal RAM, whole node | 64 KB cap; 59.9 KB used by Potluck alone, 60.1 KB with the sandbox, the extender and the room sensor |
| firmware image | 2 MB (an update slot); the image with everything above is 1.0 MB |

---

## 5. The descriptor (`potluck-modules.json`)

```json
{"component": "<directory name>", "types": [first, last], "requires": ["sandbox"],
 "modules": [{"module": "ext:<app>/<actor>", "type": <n>, "placement": "pinned|every|portable",
              "outputs": ["{node}/<app>/<output>"],
              "config": [{"name": "...", "kind": "...", "default": ..., "min": ..., "max": ...}]}]}
```

- **`outputs`** (optional, but give it): the paths an actor of the module publishes, under `potluck://<system>/`,
  with `{node}` for `node-<id>` of the node it runs on and `{actor}` for its name -- `"{node}/hello/peers"`,
  `"act/{actor}/out"`. `potluck apply` reads every one back before it says VERIFIED; an output it is not told
  about it cannot check.
- **`requires`** (optional): Potluck's board modules your component needs in the same image -- `sandbox`,
  `extender`, and from M13 `wifi`, `present`, `pocket`. `potluck apply` refuses a cluster file that builds your
  component without them, naming what to add to its `firmware.modules`.
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
| 0x80-0x8F | passive-sensor |
| 0x90-0xEF | free: ask Potluck's maintainer for a block (an issue on the Potluck repository); it is recorded here. Until then, use one provisionally and say so in your README (JSON has no comments; the descriptor loader ignores keys it does not know) |
| 0xF0-0xFF | Potluck's own examples and tests (`pot_hello` is 0xF0) |

---

## 6. Manifests

A manifest (JSON) names the cluster (`system`), its nodes and what they own, and its actors. Your app ships a
*fragment* -- its actors, and the `owns` their `needs` rely on -- and `potluck apply` composes it with the
others into the owner's one package (below):

```json
{"name": "rf_fusion", "module": "ext:passive_sensor/rf_fusion", "latency_class": 4,
 "needs": ["potluck://lab/node-7368/hw/die_temp"], "pin": 29544, "config": {...}}
```

- **`latency_class`** -- the deadline class of the actor's bindings, from ARCHITECTURE section 4: L0 under
  100 us (same silicon), L1 under 10 ms (same node), L2 under 20 ms (one wired hop), L3 under 500 ms (one
  radio hop), L4 best effort. An actor may only bind resources of its class or tighter, and the build refuses
  a binding the transport cannot meet. Radio apps are typically L4 (passive-sensor's three actors all are).
- **`needs`** -- the resources it reads; a portable actor's gravity is higher on a node that owns them. **Every
  path in `needs` must be declared by some node's `owns` list** (or the manifest is refused: "...is neither a
  declared resource nor a binding"):

  ```json
  {"node_id": 29544, "label": "B", "owns": [
    {"path": "potluck://lab/node-7368/hw/die_temp", "unit": "celsius", "kind": "sampled",
     "access": "read", "latency_class": 4, "staleness_bound_ms": 5000}]}
  ```
  This holds for a pinned actor reading a built-in on its own board too: `builtin:die_temp` on node 0x8160 is
  `potluck://<system>/node-8160/hw/die_temp`, and an actor that reads it lists it in `needs`, with the board's
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
  | `builtin:svc_client` | pinned | `period_ms` 100-60000; one `needs` of `potluck://<system>/svc/<name>`; `on_host_loss` continue or hold | the PC service's answer |
  | `builtin:led` | pinned | `r` `g` `b` 0-255, `blink`, `period_ms` 200-10000 | the status LED's healthy colour |
  | `builtin:mc_lender` / `builtin:mc_job` | pinned | M9's borrowed-core job (`slots`; `units`, `samples`, `seed`, `lend`, `start_after_ms`) | `node-<id>/job/mc/pi` |
  | `builtin:fault` | pinned | `panic_after_ms` | nothing: it crashes the node on purpose, to test reverts |
  | `guest:<name>` | portable | section 9 | `act/<actor>/<output>` |

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
(it has `pyserial`), in any shell (PowerShell or Git Bash):

```
python -m potluck.apply <cluster.json> --plan       # check everything, change nothing
python -m potluck.apply <cluster.json>              # make the cell what the file says, then verify it
python -m potluck.apply <cluster.json> inventory    # what every board runs: firmware, package, actors, counters
```

A cluster file for the hello app on three boards (the preset [`examples/cells/hello.json`](examples/cells/hello.json);
paths are relative to the file):

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
| `port` | the serial port of the PC's adapter, wired to one of the boards (`--port` overrides it) |
| `boards` | every board: its node id in hex (the last two bytes of its MAC) and a label |
| `keys` | the owner's CA certificate and deploy key ([GETTING-STARTED.md](GETTING-STARTED.md) makes them) |
| `firmware` | Potluck at a commit (`potluck`), its optional board modules by name (`modules`: `sandbox` so far; each one costs flash and RAM, and some open the cell to the outside, so none is in an image unless named -- `"extra": ["CONFIG_POT_WASM=y"]` still means `sandbox`), extra Kconfig lines (`extra`), and `extender` if a board should be a Wi-Fi extender: `{"git": ..., "rev": ..., "components": "firmware/components"}`, like an app. Every board gets the same image; a board becomes an extender only if it holds Wi-Fi credentials, the others are ordinary members. Or `{"image": ..., "elf": ...}` for a build made elsewhere, or `null` to leave the firmware alone |
| `apps` | each app: `git` (a local repository or a URL), `rev` (a commit or a tag), `components` (built into the firmware; every `potluck-modules.json` under it is loaded), `manifests` (its fragments) |
| `package` | more fragments (`"manifests": [path, or {"manifest": path, "root": dir for its relative paths}]`), more descriptors (`"modules"`), and `guests_root` |
| `verify` | `timeout_s`, and `read`: more paths to read back |
| `outside` | what systems outside the cell (Home Assistant, through the presenting module, M13) may do with each resource: `{"node-7368/hw/die_temp": "read", "node-7368/led/level": "read_write"}`, a path with or without `potluck://<system>/`. Nothing is exposed unless named here. `read_write` needs a resource some node declares in `owns` with write access, and never one marked `safety_relevant`. The owner's choice, so only the cluster file says it: a fragment with `outside` is refused |

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

Measured on three boards (2026-10-11): from minimal Potluck to a room sensor, the extender and a guest, 390 s
from command to verified, 366 s of it the firmware on three boards; taking the room sensor off again, 23.5 s, no
board rebooting, the guest's count carrying on; a fresh firmware build adds about 9 minutes the first time.

- **Builds** run in PowerShell 7 (`pwsh`) where it is installed; the build is `tools/build_firmware.ps1` from the
  exported tree, its output in the cache (`%LOCALAPPDATA%\potluck\cache` on Windows, `~/.cache/potluck` elsewhere;
  `--cache` moves it). An app built from a working tree it is still editing is how the first apps lost two days
  to "transient" failures (section 10); `apply` cannot do that, and warns when `HEAD` has uncommitted changes it
  will not build.
- **See it run:** `python -m potluck.ctl --port COM6 --node <cabled board> watch potluck://<system>/act/<name>/<output>`
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
have. A board's id is the last two bytes of its MAC; the board on the cable is the one your USB-serial adapter
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
  declaration carries its pin, so build the rig as that node -- `pot::testkit::Rig rig(0x8160);` -- and read its
  outputs under `node-8160`; the default node, 0x0100, suits an every-node actor.

  Run it with `POTLUCK_DIR` set to your Potluck checkout (an environment variable, or `-DPOTLUCK_DIR=` to CMake). On Windows Potluck's script finds CMake, Ninja and a
  compiler for you: `tools\run_host_tests.ps1 -Source <your repo>\tests -BuildDir <your repo>\build\tests`; elsewhere
  `cmake -S tests -B build && cmake --build build && ctest --test-dir build`. `rig.feed(path, value, type)`
  publishes an input as another actor would; `rig.start(row, &why, true)` gives a portable actor the checkpoint
  store, and `rig.restart()` builds a fresh instance from it, as a failover or a reboot does. The complete example
  is [`examples/apps/pot_hello/tests`](examples/apps/pot_hello/tests), which Potluck's own suite also builds and
  runs, so the kit cannot drift from the core. Keep your pure logic (detectors, statistics) in functions with no
  Potluck dependency and test it the same way.
- **Share the bytes.** Check that the host tools compile your manifest to the declaration your parser accepts
  (`host/potluck/tests/test_examples.py` against `test_hello_app.cpp`). From your own repository, put
  `<potluck>/host/potluck` on `sys.path` and `import potluck`; load your descriptor with
  `potluck.modules.load(path)` before compiling, and check placement with
  `python -m potluck.reconcile <manifest> --modules <descriptor>`.
- **On the boards.** Record consoles with `tools/json_capture.py <port> <file> <seconds>` (JSON lines only --
  use it on any board that holds Wi-Fi credentials) or `tools/console_tap.py` (everything, without resetting
  the board). CTest counts one test per executable; the kit's own output lists every case. Read the first ten minutes of any long run with your report tool before trusting the rest.
  **Count deaths from the `peer_dead` events, never from sampled state.** Test with at least three boards: two
  hide multi-node bugs. Reboot each board in turn -- passive-sensor's reboot tests found four Potluck faults.
- **Run the cheapest experiment that can kill the idea first**, with its kill criterion written down
  (passive-sensor's PS-0, the extender's step 0).

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

- A portable actor that moves without a checkpoint starts cold: passive-sensor's outputs read NO_DATA for ~66 s
  while it relearned. Save a checkpoint, and put in it everything the state was learned under.
- A board that comes back took the actor over before it had heard any checkpoint; Potluck now hands the newest
  one to a node the moment it reappears (resume in ~0.5 s).

**Credentials**

- Wi-Fi names and passwords are the owner's to type; tools record only JSON status lines from a board that
  holds them, and its Wi-Fi log is kept at WARN so network names never reach a capture.

**What the apps made Potluck add** -- the platform grew from its apps, which is the point. The extender brought
channel following, the single-hop relay, the runtime death window, the BUSY bit, the PSRAM build and the
"application owns the radio" seam (M5.1, M6.1). The room sensor brought the external-component seam itself,
radio samples for actors, several outputs per actor with replicas on demand, checkpoints and the app LED (M8.2).

---

## 11. The contract your app inherits

Potluck's failure modes -- the 600 ms death window, stale values delivered marked, a portable actor's move,
at-least-once activation during a partition, firmware updates taking a board out for ~15 s -- are listed in the
README's [failure contract](README.md#the-failure-contract). A package change adds one: an actor whose
declaration changed (its config or its placement) is stopped and a new instance started, its outputs removed and
declared again -- readers see NO_DATA until it publishes; a portable one resumes from its checkpoint, as after a
failover. An actor whose declaration did not change is not touched. An actor whose output path changes (another
name, another cluster name) is a new actor, and starts without a checkpoint. A checkpoint restored after every
board rebooted is up to a minute old. Your app inherits every one of them. The safety
line holds for apps too: Potluck must never be the only thing between a motor and a person
([ARCHITECTURE.md](ARCHITECTURE.md), section 12).
