# Getting started with Potluck

From a handful of ESP32-S3 boards on your desk to a cluster that reads one board's sensor from another, places
work on whichever board is alive, and survives you pulling a board's plug -- in about an hour, most of it
installing tools. Then [DEVELOPING-APPS.md](DEVELOPING-APPS.md) takes you on to your own apps.

This guide is for **Potluck 1.0.0**. Questions and bugs: [the issue tracker](https://github.com/Tubifix77/potluck/issues).

## Why you would use it

You have more microcontrollers than jobs for them, or more jobs than one board can do, or hardware spread
over a house, a car, a garden or a workshop. Without Potluck, each board is its own little program: you
write the radio code, the "is that board still there?" code and the "how old is this number?" code again
every time, and every board needs a cable to change what it does. Potluck is the layer that makes them one
system:

- **One namespace for all of it.** Every board's sensors and every app's results have a name like
  `potluck://lab/node-1a2c/hw/die_temp`, readable from any board or from your PC -- and every read says how
  old the value is and whether it can be trusted (`GOOD`, `STALE`, `UNAVAILABLE`, `FAULTY`), never a bare
  number.
- **Work that moves.** An actor that is not tied to one board's pins runs on whichever board the cluster
  picks, and moves to another within seconds when its board dies -- with its saved state if it kept some.
- **Deploy and detach.** Change what every board does with one signed package sent through one board on your
  PC's cable; update the firmware itself over the radio, board by board, each rolling itself back if the new
  image does not work. Then unplug the PC: the boards carry on.
- **Signed, once enrolled.** Boards join only a cluster whose certificate authority enrolled them, and an
  enrolled board accepts only packages and firmware signed by your keys. (A board not yet enrolled accepts any
  package -- which is why enrolment, step 4, comes before your first package, step 6.)
- **Room to grow.** Native apps compiled in, untrusted apps in a sandbox, a PC as a member that takes work
  while it is on, idle cores lent between boards, named services on the PC (speech, a model) that boards call.

When it is **not** the tool: anything that must be the only thing between a motor and a person (no software
system should be -- Potluck says so in its [architecture](ARCHITECTURE.md), section 12); control loops that
need a deadline across boards tighter than the radio gives (a radio hop is classed under 500 ms; a binding
under 10 ms stays on one board); and, in this edition, chips other than the ESP32-S3.

## What you need

**Hardware**

- **Two or more ESP32-S3 boards** -- as many as you like up to a cell's limit of 20 (the ESP-NOW radio
  protocol lets a board talk to 19 others). Tested on the ESP32-S3-DevKitC-1 **N16R8** (16 MB flash, 8 MB
  PSRAM); a board without PSRAM needs one build option changed (troubleshooting, below).
- A USB cable per board, for power and console while you set up.
- **One 3.3 V USB-serial adapter** (a CP2102 module, for example) and three female-female jumper wires: the
  PC talks to the cluster through it, wired to any one board. (Each board's USB cable, on the socket
  labelled UART, is its console -- for flashing and logs -- not this link.)

**Software** -- Potluck's PC side is tested on **Windows 11 only**; the tools are PowerShell-first.

- [ESP-IDF v6.0.2](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32s3/get-started/index.html),
  Espressif's framework for these chips -- that version: newer ones are untested with Potluck. Install it
  anywhere and set `IDF_PATH` to it (ESP-IDF's installer does), or pass `-IdfPath <folder>` to
  `tools\build_firmware.ps1`. It brings its own Python (3.14 in the install Potluck is tested with) with
  `pyserial`, which every command below that talks to a board needs.
- [PowerShell 7](https://learn.microsoft.com/powershell/scripting/install/install-powershell-on-windows)
  (`pwsh`), **required** for every firmware build. The older Windows PowerShell 5.1 fails inside ESP-IDF's
  `export.ps1`.
- [Git](https://git-scm.com/) (with Git Bash), and -- only to run Potluck's own tests -- Python 3.10 or newer
  on the `PATH` and a C++ compiler (Visual Studio Build Tools).

**Linux is untested.** The host tools are Python and serial ports there are named like `/dev/ttyUSB0`, but
neither the firmware steps below nor the tests (`tools/run_all_tests.sh` has never been run) have been
exercised on Linux.

**No ESP-IDF on the PC that runs the cell?** Potluck also builds as a Windows program of its own:
`tools\build_package.ps1` makes `dist\Potluck` (`potluck.exe`, a tray icon, Espressif's flasher and the release
firmware), and `packaging\install.ps1` installs it for your user. Building the package needs ESP-IDF and a Python
packaging environment once (the top of `tools\build_package.ps1` says how); the PC it is installed on needs
neither. The tray icon starts at login and keeps a hub (below) running; with no cell yet the hub runs in setup
mode, and the tray's menu opens the setup wizard (http://127.0.0.1:8470/setup.html), which finds your boards on
USB, flashes and enrols them and makes the cell. The rest of this guide is the route by hand.

## 0. Try it with no boards (optional)

The README's [Try it with no hardware at all](README.md#try-it-with-no-hardware-at-all) boots the real
firmware in an emulator and reads its namespace from your PC. Ten minutes, and you will recognise every
reading below.

## 1. Get Potluck

```
git clone --branch v1.0.0 https://github.com/Tubifix77/potluck.git
cd potluck
pwsh tools\run_all_tests.ps1
```

`--branch v1.0.0` checks out the release; your cluster file pins the same tag (step 6), so what you build is
what was released. The tests take a few minutes and need the C++ compiler; `ALL PASS` means your toolchain
builds Potluck's core and its host tools work. Skip them if you only want to use Potluck.

Commands below that start with `python -m potluck` run from `host\potluck\`, with ESP-IDF's Python (on
Windows: `%USERPROFILE%\.espressif\python_env\idf6.0_py3.14_env\Scripts\python.exe`, or whichever
`idf6.0_*` folder your install made). Installed into that Python (`python -m pip install -e host\potluck`), the
package also gives a `potluck` command with the main ones as sub-commands: `potluck apply ...` is
`python -m potluck.apply ...`. Keys go in `keys\` at the repository's root, which git ignores.

## 2. Make your cluster's keys

Two keys, made once: a **certificate authority** (CA) that says which boards belong to your cluster, and a
**deploy key**, certified by the CA, that signs what you send to them. Keep the CA's `.key` file safe and
offline if you can -- whoever has it can enrol boards into your cluster.

```
python -m potluck.signing keygen --role ca --label my-ca --out ..\..\keys\ca
python -m potluck.signing keygen --role deploy --label my-deploy --out ..\..\keys\deploy
python -m potluck.signing certify --ca ..\..\keys\ca.key --key ..\..\keys\deploy.pub --out ..\..\keys\deploy.cert
python -m potluck.enrol --deploy-cert ..\..\keys\deploy.pub --ca-key ..\..\keys\ca.key --out ..\..\keys\deploy.bcert
```

`deploy.cert` lets the PC tools check a package; `deploy.bcert` is the same certificate in the compact form
the boards check. `keygen` never replaces an existing key: run it twice and the second is refused (`--force`
exists for when you mean it -- replacing the CA means enrolling every board again).

## 3. Build and flash Potluck onto each board

Plug the board in by the USB-C socket labelled **UART** -- the one labelled USB does not work while the radio
runs, by design (Potluck turns off a USB workaround that costs radio performance). Then, from the repository's
root, in a PowerShell 7 prompt (not Git Bash):

```
pwsh tools\build_firmware.ps1 -Flash -Port COM7
```

with the board's port (Device Manager shows it). The first build takes ~5 minutes; repeat with each board's
port -- the image is built once and flashed again. A freshly booted board blinks red, green, blue (its LED's
self-test) and then shows **yellow: searching**, until it hears another board.

**What a board's LED says** -- its own view of the cell:

| colour | meaning |
|---|---|
| **purple** | updating: receiving new firmware or a new package, or running one on trial before it is confirmed |
| **yellow** | searching: no other board heard (yet, or for over 10 minutes), or hunting for the cell's radio channel |
| **red** | disconnected, dead or stalled: a board it knew died in the last 10 minutes, a link losing over half its heartbeats, or its own radio failing to send |
| **green** | on the Potluck network, all well. An app may show its own colour in green's place -- blue or cyan (a Home Assistant LED switch, an app's state); purple, yellow and red always win |
| **blinking blue** | identify: the setup wizard or the dashboard asked this board to show itself, for as many seconds as it asked |

Each board's **node id** is normally the last two bytes of its MAC address: a board whose MAC ends in `1a:2c` is
node `0x1a2c`. You will need them; step 4 prints them. (The ids in this guide -- `1a2b`, `1a2c`, `1a2d` -- and the
ports -- `COM7` for a board, `COM6` for the adapter -- are examples: use your own. On Linux, untested, a port is
a name like `/dev/ttyUSB0`.)

## 4. Enrol each board

Boards admit only peers from the same cluster, so until they are enrolled they ignore each other. For each
board, on its own USB console port:

```
python -m potluck.enrol --port COM7 --ca-key ..\..\keys\ca.key
```

It asks the board for the key it made at its first boot, signs a certificate for it with your CA and hands it
back; the board checks it and saves it. Note the node id it prints: normally the one the board's MAC gives, but if
your cell already has a board with that id, the board gets the lowest free id from `0x0100` up (`--node-id <hex>`
names one yourself). The cell's ids are kept in `keys\nodes.json`, beside the CA key. The certificate takes effect
at the next boot: press each board's **RST** button. Within a few seconds the LEDs turn **green** -- the boards found each
other (still yellow: they do not hear each other yet -- see Troubleshooting; red: a link losing over half its
heartbeats, or a board that died).

## 5. Connect the PC

Wire the USB-serial adapter to any one board -- this is "the cabled board" from now on: adapter **TXD to the
board's GPIO18, RXD to GPIO17, GND to GND** (the picture: [bench/m1-wiring.html](bench/m1-wiring.html), open
it in a browser). Plug the adapter in first, and never leave it wired to a board while it is unplugged. Then:

```
python -m potluck.ctl --port COM6 info
python -m potluck.ctl --port COM6 --node 1a2c ls
```

with the adapter's port and one of your node ids: `ls` lists that board's built-in resources -- read through
the cabled board, over the radio, each with its age and quality. `watch sys/uptime` reads one every second;
unplug that board and watch it turn `UNAVAILABLE`.

## 6. Your first package

A package tells the whole cluster what to run; a **cluster file** tells Potluck's tool what the whole cell should
be -- its boards, its firmware, its package -- and `potluck apply` makes it so. Two files, anywhere (paths in a
cluster file are relative to it). The package, `my-package.json`, with your node ids -- this one has three boards,
list as many as you have:

```json
{
  "schema": 1, "system": "lab", "min_core_version": 1,
  "nodes": [
    {"node_id": 6699, "label": "kitchen"},
    {"node_id": 6700, "label": "hall"},
    {"node_id": 6701, "label": "garden"}
  ],
  "actors": [
    {"name": "temp_hall", "module": "builtin:die_temp", "latency_class": 4, "pin": 6700,
     "config": {"period_ms": 1000}},
    {"name": "ticker", "module": "builtin:ticker", "latency_class": 3, "config": {"period_ms": 500}}
  ]
}
```

(Node ids are decimal in a package: `0x1a2c` is 6700; `python -c "print(0x1a2c)"` converts yours.) `temp_hall`
publishes the hall board's chip temperature; `ticker` is a counter that runs on one board, chosen by the cluster
-- with more than 8 boards, among the 8 it ranks best. And the cluster file, `my-cell.json`, beside it:

```json
{
  "schema": "potluck-cluster/1", "system": "lab", "port": "COM6",
  "boards": [{"node": "1a2b", "label": "kitchen"}, {"node": "1a2c", "label": "hall"},
             {"node": "1a2d", "label": "garden"}],
  "keys": {"ca": "<repo>/keys/ca.pub", "deploy_key": "<repo>/keys/deploy.key",
           "deploy_cert": "<repo>/keys/deploy.cert", "deploy_bcert": "<repo>/keys/deploy.bcert"},
  "firmware": {"potluck": {"git": "<repo>", "rev": "v1.0.0"}},
  "package": {"manifests": ["my-package.json"]}
}
```

(`<repo>` is your Potluck checkout; node ids are hex here; `port` is your adapter's. `"rev"` is the release tag
you checked out: `apply` builds exactly that commit, never your working tree.) From `host/potluck/`:

```
python -m potluck.apply <path>\my-cell.json --plan
python -m potluck.apply <path>\my-cell.json
```

`--plan` checks everything and changes nothing. The second builds the firmware (a few minutes the first time;
after that the build is reused until Potluck's firmware changes), updates each board whose firmware differs --
over the radio, one board at a time, the cabled board last, about two minutes each -- then signs the package,
sends it to every board and checks that every board runs it and every output reads back. It ends with
`VERIFIED` and how long each step took, or says what is wrong. The counters, the order and which board needs
what are the tool's to work out; `python -m potluck.apply my-cell.json inventory` shows what every board runs.

Now watch both:

```
python -m potluck.ctl --port COM6 watch potluck://lab/node-1a2c/hw/die_temp
python -m potluck.ctl --port COM6 watch potluck://lab/act/ticker/out
```

The ticker's value says which board runs it: its upper 16 bits are the node id (`python -c "print(hex(VALUE >> 16))"`
with the number it shows). **Unplug that board** -- unless it is the cabled one, through which the PC sees
everything; then unplug another and watch it stay put, or pin the ticker elsewhere. Within about a second the
ticker runs on another board and counts again. Plug it back in and it rejoins.

Change the package -- add an actor, take one out, change a period -- and run `apply` again: the boards take it
without rebooting, and actors you did not change keep running. A board that was off when you applied gets the
package from a neighbour when it comes back. Each board tries a new package before it keeps it, and goes back to
the old one by itself if the new one fails.

Rename the cluster by changing `system` in both files (say, `home`): the boards then name their resources
`potluck://home/...`.

## 7. Updating firmware later

When a new release is out, fetch it (`git fetch --tags`, then `git checkout <new tag>`), set the cluster file's
`"rev"` to the new tag, and run `apply` again: boards whose firmware differs from the new build are updated, the
others are left alone. Each keeps the new image only after it has proved itself -- a board that crashes on it, or
never rejoins the cluster with it, rolls itself back, and the update stops there.

## 8. The hub and its dashboard (optional)

The **hub** is a program on your PC that holds the adapter's port, keeps a live model of the whole cell -- every
board, every resource, every app -- and serves it as a **dashboard** in your browser and as an HTTP API for other
programs ([host/potluck/HUB-API.md](host/potluck/HUB-API.md)). From `host/potluck/`:

```
python -m potluck.hub --cluster <path>\my-cell.json
```

then open http://127.0.0.1:8470/. It takes the port from the cluster file (`--port` overrides it) and runs until you
stop it (Ctrl+C). While it runs, `potluck.ctl` and `apply` given the same port go through it without being told;
one hub runs per PC and per cell, and a second is refused. The packaged install's tray icon starts one for you.

## Reading a board's console

Each board logs to its USB console (the UART socket, 115200 baud). Two tools read it without resetting the board,
with ESP-IDF's Python, from the repository's root:

```
python tools\console_tap.py COM7 60 board.log            # every line for 60 s, stamped with the PC's clock
python tools\console_send.py COM7 "POT! id"              # send one console command, print the JSON it answers
```

ESP-IDF's own `idf.py -p COM7 monitor` works too, but resets the board when it opens the port. A console log can
name your Wi-Fi network if a board holds Wi-Fi credentials: read it before you share it, or use
`tools\json_capture.py`, which keeps only Potluck's JSON lines.

## Where next

- **Your boards in Home Assistant:** [HOME-ASSISTANT.md](HOME-ASSISTANT.md) -- with no PC through MQTT, and the
  hub's view through the Potluck integration. Any other system: [INTEGRATIONS.md](INTEGRATIONS.md).
- **Make an app:** [DEVELOPING-APPS.md](DEVELOPING-APPS.md) -- your own actors, compiled in or sandboxed, tested
  on the PC first.
- **What your apps can rely on, and what they cannot:** the README's
  [failure contract](README.md#the-failure-contract).
- **How it is built and why:** [ARCHITECTURE.md](ARCHITECTURE.md).

## Troubleshooting

| what you see | why, and what to do |
|---|---|
| A board seems dead over USB | It is on the socket labelled USB. Use **UART**. |
| A board's LED is red | It hears a board losing over half its heartbeats, one that died in the last 10 minutes (one gone longer no longer counts: a PC switched off does not leave the boards red), or its own radio cannot send. The hub's dashboard names the board. |
| LEDs stay yellow after enrolling | The certificate takes effect at the next boot: press RST. Still yellow: the board's console log shows `peer_refused` for a board from another CA -- enrol it with *your* CA. |
| `enrol` times out | Another program holds the board's port (a serial monitor): close it. |
| The build fails at once in `export.ps1` | You ran it from Windows PowerShell 5.1; use `pwsh`. |
| A board without PSRAM boots into errors | Every board in a cell runs one image, so build the whole cell without PSRAM: add `"extra": ["CONFIG_SPIRAM=n"]` to the cluster file's `"firmware"` (for step 3's first flash: `tools\build_firmware.ps1 -Variant nopsram -Extra "CONFIG_SPIRAM=n" -Flash -Port COM7`). Portable actors are then compiled out, and a board keeps at most 16 actors with 48 config bytes each and takes packages up to 512 bytes. |
| `apply` says `REFUSED, nothing changed: ...` | It found a problem before touching any board, and the rest of the line names it: a board that does not answer (power it, or take it out of the cluster file), a key file missing, a package that does not compile. |
| `apply` says `NOT VERIFIED` | It changed the cell and something did not check out; it lists what: a board not confirming the package (its console says why), or an output with no GOOD reading by the deadline (an actor that takes long to start: raise `"verify": {"timeout_s": ...}`). Run it again once fixed. |
| `apply` waits for a trial to end | A board is still trying out the last package (it keeps one only after it has run cleanly); `apply` waits for it, up to five minutes. |
| A board does not keep a new image | Working as intended: it rolled back because the image did not prove itself; its console log says why. |

## Limits of this edition

Stated so they do not surprise you: up to **20 boards in a radio cell**, all within radio range of each other (a
board told to relay, `POT! relay 1` on its console, carries one more hop and keeps doing so across reboots -- one relay
per cell: with two, the lower node id keeps the role) -- more boards make a tree of cells under one package, each
cell's *foreman* board summarising its cell upward (the cluster file's `cells`, [DEVELOPING-APPS.md](DEVELOPING-APPS.md));
**ESP32-S3** boards, plus PCs as members; **one package per cluster**, which lists everything the cluster runs
(`apply` composes it from every app's part); up to 64 actors in a package, at most 8 of them running on any one
board, 16 portable actors in a cell, each placed among at most 8 boards (the best 8 of a bigger cell), and 128
named resources per board. These are for boards with PSRAM; the full table is in
[DEVELOPING-APPS.md](DEVELOPING-APPS.md#limits). Every one of them is a number in the source, not a law of nature.
