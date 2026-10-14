# node-red-contrib-potluck

Node-RED nodes for a [Potluck](https://github.com/Tubifix77/potluck/blob/main/README.md) cell: read what its boards
measure, with how good each reading is, and write what the cell's owner allows.

**Potluck** makes a few small boards (ESP32-S3 microcontrollers) behave as one system, called a **cell**. Each board
brings something -- a sensor, an LED, a radio -- and publishes it as a **resource** with a path such as
`potluck://lab/node-7368/hw/die_temp` (cell `lab`, board `7368`, the chip's temperature). A program on a PC, the
**hub**, holds the cable to the cell and serves one picture of it over HTTP. These nodes talk to the hub; they never
talk to the boards, and they cannot change what runs on them.

## Install

From your Node-RED user directory (usually `~/.node-red`), with this repository checked out:

```sh
npm install <path to potluck>/integrations/node-red
```

then restart Node-RED. The package needs nothing beyond Node.js itself (18 or newer) and Node-RED 3 or newer. Three
nodes appear: **potluck in** and **potluck out** in the *network* group, and the **potluck-hub** connection they share.

## Connect to the hub

**Node-RED on the hub's own PC** (the simple case): set the hub URL to `http://127.0.0.1:8470/`. There is no login:
the hub trusts its own PC. Use `127.0.0.1`, not `localhost`, which may resolve to an address the hub does not listen on.

**Node-RED on another computer**: the hub must have been started with `--listen` (its owner knows); it then speaks
HTTPS and announces itself on the house network.

1. In the potluck-hub connection, click **Find hubs on the network** and pick yours. This fills in the URL and the
   hub's certificate fingerprint. (The hub signs its own certificate, so the fingerprint is how Node-RED knows it is
   talking to that hub. If your network blocks multicast, the owner can read the fingerprint from the hub's
   certificate file: `openssl x509 -in <cert.pem> -noout -fingerprint -sha256`.)
2. Choose **Ask for**: *look* to read, *write* if a potluck-out node will use it.
3. Click **Pair**. On the hub's PC, the owner opens the hub's dashboard and clicks **Allow** for your request. The token
   appears in the connection; press **Update**, then **Deploy**. Nothing to type, and the token is stored with your
   Node-RED credentials, not in the flow.

**This program** is the name the hub knows you by: on its dashboard, in its rate limits, and in the list of programs a
resource may be written by. Pair with the name you will keep.

## potluck in

Sends one message per change:

```js
{
  topic: "potluck://lab/node-7368/hw/die_temp",
  payload: 51.5,                    // null when there is no value to show
  potluck: {
    kind: "resource",
    quality: "GOOD",                // GOOD, STALE, UNAVAILABLE, NO_DATA or FAULTY
    unit: "C", age_ms: 2685,        // the reading's age when the hub last read the cell
    owner: "7368",                  // the board that owns it
    board: { node: "7368", label: "B", state: "alive", since_at: 1791775000000 },
    name: "node-7368/hw/die_temp", outside_writable: false, safety_relevant: false,
    cell: "ef76cc2e", system: "lab", initial: false
  }
}
```

**Always look at `msg.potluck.quality`.** `payload` is `null` whenever the reading is not there: the board is dead
(`UNAVAILABLE`), has never published (`NO_DATA`), says its sensor is broken (`FAULTY`), or the hub cannot be reached.
Never treat `null` as zero or as off. `STALE` still carries the value: it is older than it should be.

Options: **Resources** filters by path or name (`node-7368/*, act/rf_fusion/out`; empty is everything the cell exposes
to other systems). **Board state changes** adds a message when a board comes and goes (`topic`
`potluck://lab/node-8160`, `payload` `alive`, `asleep` or `dead`). **Events** adds the cell's events (`topic`
`potluck://lab/event/<kind>`, `payload` the detail). Use a **switch** on `msg.potluck.kind` (`resource`, `board`,
`event`) to separate them.

## potluck out

Writes `msg.payload` to the resource in `msg.topic` (or the one set on the node) and passes the message on with the
answer:

```js
msg.potluck = { kind: "write", result: "applied", ok: true, status: 200, path: "potluck://lab/node-7368/led/on", value: true }
```

`payload` is `true`/`false` or a number (`"on"`, `"off"` and numbers as text are accepted). Show `result` to whoever
asked: `applied`, or why not -- `not writable from outside` (the owner does not allow it), `this login may look, not
write` (pair again asking for *write*), `not writable by this program` (the owner named other programs),
`the owning board did not answer`. Safety-relevant resources can never be written from outside, and the board checks
again whatever the hub says.

A board treats a whole number and a number with a fraction as different types. For a resource that takes a fraction
(a setpoint of 20.0), set **Numbers** to *always with a fraction*, or 20 arrives as a whole number and is refused.

## What the nodes do for you

- **Live, and recovering.** They follow the hub's live stream. If it goes quiet for three of the hub's read periods
  (at least 10 s), they read the hub every 2 s instead and try the stream again every 15 s. If the hub cannot be
  reached at all for that long, every resource is sent once more as `null` / `UNAVAILABLE`, so no flow acts on an old
  number; when the hub returns, the real values follow.
- **Polite.** The hub gives each program 20 requests and 2 writes a second. When it says "slow down", the nodes wait
  exactly as long as it asks and try a write again (up to three times).
- **Visible.** The connection tells the hub every 30 s whether it is connected, so the hub's dashboard lists
  Node-RED next to the cell's other integrations. Each node's status dot shows the same: green connected, yellow
  connected by polling, red unreachable or refused.

## Without Node-RED

`lib/client.js` is the whole logic and runs on plain Node.js:

```js
const { PotluckClient } = require('node-red-contrib-potluck');
const c = new PotluckClient({ url: 'http://127.0.0.1:8470/', name: 'my-script' });
c.on('resource', (r) => console.log(r.path, r.value, r.quality, r.unit));
c.start();
c.write('node-7368/led/on', true).then((r) => console.log(r.result));
```

## Tests

`npm test` (or `node --test test/client.test.js`) runs the client against `tools/hub_mock.py`, the repository's
stand-in for the hub (set `POTLUCK_PYTHON` to a Python 3 if `python` is not on the path), and against a small test
server for pairing over HTTPS, rate limits and a stalled stream. The repository's gate runs it as
`host/potluck/tests/test_node_red_lib.py`.

## The interface it is built on

[INTEGRATIONS.md](https://github.com/Tubifix77/potluck/blob/main/INTEGRATIONS.md) and the hub's API,
[HUB-API.md](https://github.com/Tubifix77/potluck/blob/main/host/potluck/HUB-API.md). This package was written from
those two documents alone, as the test that they are enough.
