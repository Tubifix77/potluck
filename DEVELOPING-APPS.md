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

**Status of this manual (2026-10-11).** It describes today's tools, hand steps included. Milestone M12
([ARCHITECTURE.md](ARCHITECTURE.md), section 13) replaces most of those steps with one `potluck apply`
command; this manual is rewritten as it lands. Everything shown here is tested: the worked example in
section 3 is built for the boards and tested on a PC by Potluck's own test suites.

**Blind-reader test, 2026-10-11.** A fresh reader with only this manual and Potluck's source built a portable
app with a remote input and a checkpoint (9 C++ and 5 Python tests passing, a firmware image within budget) in
about 20 minutes, and listed 23 places the manual failed it. The ones a manual can fix are fixed in this
revision (the portable recipe in section 3, `owns` and the built-in modules in section 6, who does what in
section 7, the test kit in section 8). That test was aimed at the maintainer's own bench, whose cell
already runs two other apps (one private), so its deploy blockers belonged to that bench, not to you. For your own
boards running vanilla Potluck, getting them there is now [GETTING-STARTED.md](GETTING-STARTED.md); what remains for
M12 is that **type numbers can collide between authors who share apps** (section 5), that every counter is yours to
track, and a test kit for your app's own repository (section 8).

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

## 2. How an app reaches the boards (today)

```
your repository                          Potluck
  firmware/components/<your_component>/   tools/build_firmware.ps1 -AppComponents <dir>   -> firmware image
     CMakeLists.txt                       python -m potluck.fw ... rollout                 -> every board, over the radio
     potluck-modules.json  -------------> python -m potluck.signing sign --modules ...     -> signed package
     src/*.cpp, include/                  python -m potluck.ctl ... deploy --modules ...   -> every board
  manifests/<app>.json
```

1. Your actors are C++ classes in an [ESP-IDF component](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/build-system.html)
   (ESP-IDF's unit of code) in your own repository. Potluck finds it because a `potluck-modules.json` sits
   beside its `CMakeLists.txt`, and calls one function you define, `app_rows_<directory name>()`, for its
   registration rows. No Potluck file names your app.
2. The descriptor (`potluck-modules.json`) tells Potluck's host tools your actors' names, their type
   numbers, how they are placed and how their configuration is laid out. The tools read it as data and
   never run your code.
3. A manifest names your actors with their configuration; the host compiles it into the node image, the
   owner signs it, and one board on the PC's cable passes it to the whole cell.

---

## 3. A worked example: `pot_hello`

[`examples/apps/pot_hello`](examples/apps/pot_hello) is a complete app with one actor, `ext:hello/peers`: every
period, each board publishes how many radio peers it sees alive to `potluck://lab/node-<id>/hello/peers`, and
lights its status LED cyan while it sees none. Read its four files in this order:

- **`potluck-modules.json`** -- the descriptor:

  ```json
  {"component": "pot_hello", "types": [240, 255],
   "modules": [{"module": "ext:hello/peers", "type": 240, "placement": "every",
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

Build it into a firmware variant, from the Potluck repository, in PowerShell:

```
tools\build_firmware.ps1 -Variant hello -AppComponents D:\path\to\your\repo\firmware\components -OutDir D:\path\to\your\repo\firmware
```

`-OutDir` keeps the build and its size report in your repository, so Potluck's tree stays clean. The
example adds 1,152 bytes to the default image. Then roll the firmware out and deploy the package (section 7).

It is tested twice before any board: `tests/test_hello_app.cpp` runs the actor on a real `pot::Node` on the
PC, and `host/potluck/tests/test_examples.py` checks that the host tools compile `hello.json` to exactly
the bytes the C++ test parses. Copy that pattern (section 8).

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
- **Units:** `Unit::None` when the unit has no code (degrees per minute, say); say the unit in the output's name.

### Checkpoints (portable actors)

`ActorEnv::checkpoint` keeps up to 128 bytes for the next activation -- on another node after a failover.
Only the current owner saves, at most once a second; the newest copy reaches every eligible node, including
one that comes back. Checkpoints live in RAM: when every board reboots at once (a new package does that),
they are gone. Store what the state depends on with it: a baseline learned on one radio channel is wrong on
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
| static internal RAM, whole node | 64 KB cap; 56.6-56.9 KB used today |

---

## 5. The descriptor (`potluck-modules.json`)

```json
{"component": "<directory name>", "types": [first, last],
 "modules": [{"module": "ext:<app>/<actor>", "type": <n>, "placement": "pinned|every|portable",
              "config": [{"name": "...", "kind": "...", "default": ..., "min": ..., "max": ...}]}]}
```

- **Config kinds:** `u8 u16 u32 i8 i16 i32 f32 bool`; `path` (a namespace path, sent as its 32-bit hash);
  `node` (a node id or a label from the manifest); `system_prefix` (no value: the hash state after
  `potluck://<system>/`). Fields are encoded little-endian in the order listed; `min`/`max` are enforced on
  the host before anything is signed. A portable module takes `period_ms` from the manifest without listing it
  in the descriptor: it goes into the standard header (section 3).
- **Type numbers.** 0x00-0x7F are Potluck's built-ins; 0x80-0xFF belong to external components, given out in
  blocks of 16. A build with two rows for one type refuses the merge and names both components; an image naming
  a type the firmware has no row for is refused, never silently skipped.

| block | holder |
|---|---|
| 0x80-0x8F | passive-sensor |
| 0x90-0xEF | free: ask Potluck's maintainer for a block (an issue on the Potluck repository); it is recorded here. Until then, use one provisionally and say so in your README (JSON has no comments; the descriptor loader ignores keys it does not know) |
| 0xF0-0xFF | Potluck's own examples and tests (`pot_hello` is 0xF0) |

---

## 6. Manifests

A manifest (JSON) names the cluster (`system`), its nodes and what they own, and its actors:

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

- **One package per cluster.** Deploying a package replaces the previous one, so a manifest lists *every*
  actor the cluster should run -- yours and everyone else's. To add your app to a running cluster today, merge
  your actors into its manifest (`manifests/room-and-guests.json` merges passive-sensor's into Potluck's).

---

## 7. Building, rolling out, deploying

From the Potluck repository. Builds run in **PowerShell 7** (`pwsh`); Windows PowerShell 5.1 fails inside
ESP-IDF's `export.ps1`. Run the `git archive | tar` export in Git Bash (a binary pipe is safest there). The host
commands run from `host/potluck/` with the ESP-IDF Python (it has `pyserial`). Keys live in the git-ignored
`keys/` and are the owner's.

```
# 1. build -- your component from a git archive export, never your working tree (see section 10)
git -C <your repo> archive <commit> firmware/components | tar -x -C <export dir>
tools\build_firmware.ps1 -Clean -Variant <name> -AppComponents <export dir>\firmware\components -OutDir <out>
#    a board built as a different variant gets its own image in step 2: --image-for <its id>=<that image>

# 2. firmware over the radio, board by board (each keeps it only after 100 heartbeats with a peer)
python -m potluck.fw --port COM6 status --manifest <your cell's manifest>
python -m potluck.fw --port COM6 rollout <out>\build-<name>\potluck_m0.bin --counter <floor+1> --key ../../keys/deploy.key --bcert ../../keys/deploy.bcert --manifest <your cell's manifest>

# 3. the package
python -m potluck.signing sign <manifest> --key ../../keys/deploy.key --cert ../../keys/deploy.cert --counter <n> --out ../../keys/<name>-<n>.pkg.json --modules <descriptor>
python -m potluck.ctl --port COM6 --node 6300 deploy ../../keys/<name>-<n>.pkg.json --ca ../../keys/ca.pub --key ../../keys/deploy.key --bcert ../../keys/deploy.bcert --modules <descriptor>
```

What to know today (each is on M12's list to remove):

- **Firmware before package.** A package naming an actor type the firmware lacks is refused when the boards
  boot it, and they revert to the previous package.
- **Counters only go up.** The firmware counter and the package counter each have a floor on every board;
  `potluck.fw status` shows the firmware's. Both are numbers you track.
- **Every package deploy reboots every board** onto the new package on trial; checkpoints do not survive it.
- **Image size.** A firmware image must fit the 1 MB `factory` slot; ESP-IDF's partition check stops the build
  if it does not, and prints the space left (`... bytes (6%) free`). The extender build with passive-sensor and
  the sandbox is 990,642 B. Section 6's RAM check is separate: it prints `within budget` or fails.
- **See it run:** `python -m potluck.ctl --port COM6 --node 6300 watch potluck://lab/act/<name>/<output>` reads an
  output through the cabled board once a second, with its age and quality; `--count N` stops after N reads.
- **Verify what you flashed:** the build stamps a version (`deploy-and-detach-<n>-g<commit>`, in
  `build-<name>/project_description.json`; `-dirty` if Potluck's tree had uncommitted changes -- including the size reports an in-place build rewrites in `firmware/size`), and
  `potluck.fw status` shows what each board runs.

### On your own boards

The commands in this manual use an example cell -- boards `0x6300`, `0x7368` and `0x8160`, the PC's adapter on
`COM6` wired to `0x6300` -- but nothing in Potluck is about three boards: list as many as you have. Replace `COM6`
with your USB-serial adapter's port, `6300` with the id of the board that adapter is wired to (a board's id is the last two bytes of its MAC), and `../../keys/...` with your own keys.
Keep the cluster's `system` name `lab`: this edition's boards name their built-in resources under
`potluck://lab/...`, whatever the manifest says. On boards you just flashed, both counters start at 0, so
your first firmware and your first package are counter 1; keep a note of each counter you use. Your images contain
only what you build in: vanilla Potluck plus the components you name with `-AppComponents` (it takes several
directories) -- an image without one of them removes that app from the boards it lands on.

### Sharing your app

Put the component, its descriptor, a manifest fragment (its actors, and the `owns` its `needs` rely on) and its
tests in a public repository. Someone running it adds your component to their build (`-AppComponents`), passes
your descriptor (`--modules`) when they sign and deploy, and merges your actors into their manifest. Before you
publish, get a type block recorded in section 5: two apps that use the same type numbers cannot be built into one
image (the build refuses and names both).

---

## 8. Testing

- **On a PC first.** Keep your logic in pure functions with no Potluck dependency and test them with any C++
  test framework -- passive-sensor's detector and link statistics are tested that way. To test the actor
  itself, run it on a real `pot::Node` with a fake clock and transport, as `tests/test_hello_app.cpp` does:
  Potluck's portable sources build on Windows and Linux (`tests/CMakeLists.txt` shows which). A reusable test
  kit for apps in their own repositories is part of M12. Until then, in your own repository:
  - **Sources:** let Potluck's own test build define its portable core for you, so it cannot drift: in your
    test `CMakeLists.txt`, `add_subdirectory(<potluck>/tests potluck EXCLUDE_FROM_ALL)` and link your test
    against `pot_portable` (Monocypher, the sandbox and every include directory come with it). Write your own
    `main` -- Potluck's `tests/test_main.cpp` also emits Potluck's own fixtures -- and reuse the header-only
    `tests/test_harness.hpp` if you like (its `TEST`/`CHECK` macros; your `main` runs the registered tests).
  - **A compiler:** `tools/run_host_tests.ps1` shows how Potluck finds one (CMake and Ninja from the ESP-IDF tools
    install, MSVC through `vswhere`, or gcc/clang on PATH); copy its approach or run from a Visual Studio developer
    shell.
  - **The rig:** `test_hello_app.cpp`'s `Rig` (a `Node` with a fake clock and transport) fits pinned and every-node
    actors. For a portable actor, either declare its outputs yourself before `start()`, or load the image into a
    real `Reconciler` with `collect_portable()` and let it place and tick the actor -- as Potluck's
    `tests/test_reconcile.cpp` does for the guest actor.
- **Share the bytes.** Check that the host tools compile your manifest to the declaration your parser accepts
  (`host/potluck/tests/test_examples.py` against `test_hello_app.cpp`). From your own repository, put
  `<potluck>/host/potluck` on `sys.path` and `import potluck`; load your descriptor with
  `potluck.modules.load(path)` before compiling, and check placement with
  `python -m potluck.reconcile <manifest> --modules <descriptor>`.
- **On the boards.** Record consoles with `tools/json_capture.py <port> <file> <seconds>` (JSON lines only --
  use it on any board that holds Wi-Fi credentials) or `tools/console_tap.py` (everything, without resetting
  the board). Read the first ten minutes of any long run with your report tool before trusting the rest.
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
- **Owner:** name it as `"module": "guest:<name>"` with `"bundle"` and its `"sha256"`, then deploy with
  `--guests <dir>`. Every node checks the author's signature before it accepts the package and before the guest
  first runs.
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
README's [failure contract](README.md#the-failure-contract). Your app inherits every one of them. The safety
line holds for apps too: Potluck must never be the only thing between a motor and a person
([ARCHITECTURE.md](ARCHITECTURE.md), section 12).
