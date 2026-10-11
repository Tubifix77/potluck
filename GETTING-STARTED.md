# Getting started with Potluck

From a handful of ESP32-S3 boards on your desk to a cluster that reads one board's sensor from another, places
work on whichever board is alive, and survives you pulling a board's plug -- in about an hour, most of it
installing tools. Then [DEVELOPING-APPS.md](DEVELOPING-APPS.md) takes you on to your own apps.

## Why you would use it

You have more microcontrollers than jobs for them, or more jobs than one board can do, or hardware spread
over a house, a car, a garden or a workshop. Without Potluck, each board is its own little program: you
write the radio code, the "is that board still there?" code and the "how old is this number?" code again
every time, and every board needs a cable to change what it does. Potluck is the layer that makes them one
system:

- **One namespace for all of it.** Every board's sensors and every app's results have a name like
  `potluck://lab/node-7368/hw/die_temp`, readable from any board or from your PC -- and every read says how
  old the value is and whether it can be trusted (`GOOD`, `STALE`, `UNAVAILABLE`, `FAULTY`), never a bare
  number.
- **Work that moves.** An actor that is not tied to one board's pins runs on whichever board the cluster
  picks, and moves to another within seconds when its board dies -- with its saved state if it kept some.
- **Deploy and detach.** Change what every board does with one signed package sent through one board on your
  PC's cable; update the firmware itself over the radio, board by board, each rolling itself back if the new
  image does not work. Then unplug the PC: the boards carry on.
- **Signed, from the first boot.** Boards join only a cluster whose certificate authority enrolled them, and
  accept only packages and firmware signed by your keys.
- **Room to grow.** Native apps compiled in, untrusted apps in a sandbox, a PC as a member that takes work
  while it is on, idle cores lent between boards, named services on the PC (speech, a model) that boards call.

When it is **not** the tool: anything that must be the only thing between a motor and a person (no software
system should be -- Potluck says so in its [architecture](ARCHITECTURE.md), section 12); control loops that
need a deadline across boards tighter than the radio gives (a radio hop is classed under 500 ms, and loops
under 10 ms stay on one board); and, in this edition, chips other than the ESP32-S3.

## What you need

**Hardware**

- **Two or more ESP32-S3 boards** -- as many as you like up to a cell's limit of 20 (the ESP-NOW radio
  protocol lets a board talk to 19 others). Tested on the ESP32-S3-DevKitC-1 **N16R8** (16 MB flash, 8 MB
  PSRAM); a board without PSRAM needs one build option changed (troubleshooting, below).
- A USB cable per board, for power and console while you set up.
- **One 3.3 V USB-serial adapter** (a CP2102 module, for example) and three female-female jumper wires: the
  PC talks to the cluster through it, wired to any one board. (The board's own USB port is its console,
  not this link.)

**Software** (Windows; the tools are PowerShell-first)

- [ESP-IDF v6.0.2](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32s3/get-started/index.html),
  Espressif's framework for these chips -- that version: newer ones are untested with Potluck -- installed where the build script looks for it (`D:\esp\esp-idf`) or
  wherever you like with `-IdfPath`. It brings its own Python with `pyserial`, which every command below that
  talks to a board needs.
- [PowerShell 7](https://learn.microsoft.com/powershell/scripting/install/install-powershell-on-windows)
  (`pwsh`). The older Windows PowerShell 5.1 fails inside ESP-IDF's `export.ps1`.
- [Git](https://git-scm.com/) (with Git Bash), Python 3.10 or newer, and -- only to run Potluck's own tests --
  a C++ compiler (Visual Studio Build Tools).

Linux: Potluck's host tools and tests run there (`tools/run_all_tests.sh`), and PowerShell 7 runs on Linux,
but the firmware steps below have only been exercised on Windows.

## 0. Try it with no boards (optional)

The README's [Try it with no hardware at all](README.md#try-it-with-no-hardware-at-all) boots the real
firmware in an emulator and reads its namespace from your PC. Ten minutes, and you will recognise every
reading below.

## 1. Get Potluck

```
git clone https://github.com/Tubifix77/potluck.git
cd potluck
pwsh tools\run_all_tests.ps1
```

The tests take a few minutes and need the C++ compiler; `ALL PASS` means your toolchain builds Potluck's core
and its host tools work. Skip them if you only want to use Potluck.

Commands below that start with `python -m potluck` run from `host\potluck\`, with ESP-IDF's Python (on
Windows: `%USERPROFILE%\.espressif\python_env\idf6.0_py3.14_env\Scripts\python.exe`, or whichever
`idf6.0_*` folder your install made). Keys go in `keys\` at the repository's root, which git ignores.

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
root:

```
pwsh tools\build_firmware.ps1 -Flash -Port COM7
```

with the board's port (Device Manager shows it). The first build takes ~5 minutes; repeat with each board's
port -- the image is built once and flashed again. A freshly booted board blinks red, green, blue (its LED's
self-test) and then shows **blue: no peer**.

Each board's **node id** is the last two bytes of its MAC address: a board with MAC `b8:1f:3f:da:73:68` is node
`0x7368`. You will need them; step 4 prints them.

## 4. Enrol each board

Boards admit only peers from the same cluster, so until they are enrolled they ignore each other. For each
board, on its own USB console port:

```
python -m potluck.enrol --port COM7 --ca-key ..\..\keys\ca.key
```

It asks the board for the key it made at its first boot, signs a certificate for it with your CA and hands it
back; the board checks it and saves it. Note the node id it prints. The certificate takes effect at the next
boot: press each board's **RST** button. Within a few seconds the LEDs turn **green** -- the boards found each
other (yellow or red means a link is losing heartbeats, or a peer is gone).

## 5. Connect the PC

Wire the USB-serial adapter to any one board -- this is "the cabled board" from now on: adapter **TXD to the
board's GPIO18, RXD to GPIO17, GND to GND** (the picture: [bench/m1-wiring.html](bench/m1-wiring.html), open
it in a browser). Plug the adapter in first, and never leave it wired to a board while it is unplugged. Then:

```
python -m potluck.ctl --port COM6 info
python -m potluck.ctl --port COM6 --node 7368 ls
```

with the adapter's port and one of your node ids: `ls` lists that board's built-in resources -- read through
the cabled board, over the radio, each with its age and quality. `watch sys/uptime` reads one every second;
unplug that board and watch it turn `UNAVAILABLE`.

## 6. Your first package

A package tells the whole cluster what to run. Write `my-cell.json` (anywhere) with your node ids -- this one
has three boards, list as many as you have:

```json
{
  "schema": 1, "system": "lab", "min_core_version": 1,
  "nodes": [
    {"node_id": 25344, "label": "A"},
    {"node_id": 29544, "label": "B"},
    {"node_id": 33120, "label": "C"}
  ],
  "actors": [
    {"name": "temp_b", "module": "builtin:die_temp", "latency_class": 4, "pin": 29544,
     "config": {"period_ms": 1000}},
    {"name": "ticker", "module": "builtin:ticker", "latency_class": 3, "config": {"period_ms": 500}}
  ]
}
```

(Node ids are decimal in JSON: `0x7368` is 29544. Call the cluster `lab` for now -- this edition's boards name
their built-in resources under `potluck://lab/...`.) `temp_b` publishes board B's chip temperature; `ticker` is
a counter that runs on one board, chosen by the cluster from the boards listed (up to 8 of them; with more
boards, give it `"pin"` or a shorter list -- see DEVELOPING-APPS.md, section 6). Sign it and deploy it through the cabled board
(its node id goes after `--node`):

```
python -m potluck.signing sign my-cell.json --key ..\..\keys\deploy.key --cert ..\..\keys\deploy.cert --counter 1 --out ..\..\keys\my-cell-1.pkg.json
python -m potluck.ctl --port COM6 --node 6300 deploy ..\..\keys\my-cell-1.pkg.json --ca ..\..\keys\ca.pub --key ..\..\keys\deploy.key --bcert ..\..\keys\deploy.bcert
```

The cabled board checks the signature, passes the package to every board, and each reboots into it on trial,
keeping it once it has run cleanly. Now watch both:

```
python -m potluck.ctl --port COM6 watch potluck://lab/node-7368/hw/die_temp
python -m potluck.ctl --port COM6 watch potluck://lab/act/ticker/out
```

The ticker's value says which board runs it (the node id is in its upper 16 bits). **Unplug that board**: within
about a second the ticker runs on another one and counts again. Plug it back in and it rejoins.

Each package needs a higher `--counter` than the last (2, 3, ...): boards refuse anything older, so a replayed
old package cannot roll them back. Keep a note of the last one.

## 7. Updating firmware later

Over the radio, one board at a time, each keeping the new image only after it has proved itself:

```
python -m potluck.fw --port COM6 status
python -m potluck.fw --port COM6 rollout ..\..\firmware\build\potluck_m0.bin --counter 1 --key ..\..\keys\deploy.key --bcert ..\..\keys\deploy.bcert
```

`status` shows each board's version and firmware counter; the rollout's counter must be above it. ~2 minutes a
board. A board that crashes on the new image, or never rejoins the cluster with it, rolls itself back, and the
rollout stops there.

## Where next

- **Make an app:** [DEVELOPING-APPS.md](DEVELOPING-APPS.md) -- your own actors, compiled in or sandboxed, tested
  on the PC first.
- **What your apps can rely on, and what they cannot:** the README's
  [failure contract](README.md#the-failure-contract).
- **How it is built and why:** [ARCHITECTURE.md](ARCHITECTURE.md).

## Troubleshooting

| what you see | why, and what to do |
|---|---|
| A board seems dead over USB | It is on the socket labelled USB. Use **UART**. |
| LEDs stay blue after enrolling | The certificate takes effect at the next boot: press RST. Still blue: the board's console log shows `peer_refused` for a board from another CA -- enrol it with *your* CA. |
| `enrol` times out | Another program holds the board's port (a serial monitor): close it. |
| The build fails at once in `export.ps1` | You ran it from Windows PowerShell 5.1; use `pwsh`. |
| A board without PSRAM boots into errors | Build with `-Variant nopsram -Extra "CONFIG_SPIRAM=n"` (portable actors are then compiled out). |
| `deploy` answers `DOWNGRADE` | The counter is not above the boards' last one: use a higher one. |
| `deploy` answers `BAD_SIGNATURE` or `UNSIGNED` | The package was not signed by a deploy key your CA certified, or `--key`/`--bcert` were left out. |
| A board does not keep a new image | Working as intended: it rolled back because the image did not prove itself; its console log says why. |

## Limits of this edition

Stated so they do not surprise you: up to **20 boards in a radio cell**, all within radio range of each other
(a board can be set to relay for one more hop: `POT! relay 1` on its console); **ESP32-S3** boards, plus PCs as members; **one package per cluster**,
which lists everything the cluster runs; up to 16 actors in a package, a portable actor placed among at most 8 boards, and 128 named resources per
board; the
cluster is called `lab`. Every one of these is a number in the source, not a law of nature -- and the next
milestone, M12, works on the ones that get in the way of building on Potluck
([ARCHITECTURE.md](ARCHITECTURE.md), section 13).
