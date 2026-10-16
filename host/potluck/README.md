# potluck -- the host package

The PC side of [Potluck](../../README.md): one Python package that builds and deploys a cell, talks to its boards,
runs the hub (the PC program that holds the link to the cell and serves the dashboard and the API), and keeps the
cell's keys. The boards run the firmware in [`firmware/`](../../firmware); everything a person runs on the PC to set
them up and use them is here.

The host tools are tested on **Windows 11 only**. They are written to be portable (the serial ports are named
`COM5` on Windows and `/dev/ttyUSB0` on Linux in the examples), but Linux has not been tested.

## Install

Python 3.10 or newer (tested with 3.14). Building firmware also needs ESP-IDF v6.0.2, and the docs use ESP-IDF's
own Python environment, which already has `pyserial` and `esptool`; see [GETTING-STARTED.md](../../GETTING-STARTED.md).

From the repository root:

```bash
pip install -e host/potluck
```

This gives three commands:

| command | what it is |
|---|---|
| `potluck` | the package's command line: `potluck apply`, `potluck hub`, `potluck ctl`, `potluck setup`, `potluck keystore`, `potluck enrol`, `potluck fw`, `potluck hubauth`, `potluck tray` |
| `potctl` | read and write the cell's namespace (the same as `potluck ctl`) |
| `potluck-capture` | record a board's serial stream and report delivery and delay (below) |

The docs write every command as `python -m potluck.<module>`, run from `host/potluck/`; `potluck <module>` is the same
thing for the modules in the first row. A module with no `potluck` sub-command (`signing`, `guest`, `release`,
`stubs`, `agent`, `hostnode`) runs only as `python -m potluck.<module>`.

Dependencies: `pyserial` (serial ports) and `cryptography` (the sealed key vault, `potluck.keystore`) install with
the package. The setup wizard flashes boards with `esptool`, run as a program of its own and never imported
(`pip install -e "host/potluck[setup]"` adds it; `POTLUCK_ESPTOOL` can name another esptool executable). The frame
decoder and the capture tool use the standard library only.

The packaged Windows build (`tools\build_package.ps1`, then `packaging\install.ps1`) carries this package as
`potluck.exe` with the same sub-commands, plus a tray icon that keeps a hub running; it needs no Python.

## The modules a person runs

Each module prints its own usage with `--help`; the module's docstring is its fullest reference.

| module | what it does |
|---|---|
| `potluck.apply` | from one cluster file to a cell running it: plan, build, flash or roll out, deploy, verify. `--plan` changes nothing; `inventory` reports what each board runs |
| `potluck.ctl` | potctl: read, write and watch namespace entries over the frame link or the hub's relay |
| `potluck.hub` | the hub: holds the PC's link to the cell, serves the dashboard and the HTTP API (127.0.0.1:8470), publishes board health to the cell's MQTT broker |
| `potluck.setup` | the setup wizard's back end, served by the hub (find boards on USB, flash, enrol, give a board Wi-Fi, blink its LED); on the command line `ports`, `probe <port>` (resets the board) and `identify <port> <seconds>` |
| `potluck.fw` | firmware over the cell: roll a signed image out one board at a time, each kept only if it comes back healthy; `status` shows what each board runs |
| `potluck.enrol` | enrol a board under the cluster's certificate authority over its USB console; certify a guest author |
| `potluck.signing` | make keys (CA, deploy, guest), certify a key, sign a deploy package |
| `potluck.guest` | sign and verify guest bundles: WebAssembly modules run in the boards' sandbox |
| `potluck.keystore` | the cell's secret keys encrypted at rest (`seal`, `check`), and their backup and restore |
| `potluck.hubauth` | the hub's logins for other computers and phones, and the programs paired with it (`tokens`, `revoke`) |
| `potluck.release` | build the release firmware image a packaged Potluck carries |
| `potluck.stubs` | simulated cells on the PC (`pot_stubs`) joined to a real cell through the cable, for testing a tree of cells |
| `potluck.tray` | the tray icon: the cell's state at a glance and a way to the dashboard |
| `potluck.agent` | the PC offering named services to the cell (for example `time`) |
| `potluck.hostnode` | the PC as a placement node: runs portable actors from the same C++ source as the boards |

Two more modules are libraries the hub uses rather than programs: `potluck.store` (sandboxed apps from GitHub,
installed only by the owner through the dashboard) and `potluck.survey` (proposes a tree of cells from measured links).

## Where the manuals are

- [GETTING-STARTED.md](../../GETTING-STARTED.md): from boards in a box to a running cell.
- [DEVELOPING-APPS.md](../../DEVELOPING-APPS.md): writing, testing and deploying an app; cluster files.
- [HUB-API.md](HUB-API.md): the hub's HTTP API.
- [INTEGRATIONS.md](../../INTEGRATIONS.md) and [HOME-ASSISTANT.md](../../HOME-ASSISTANT.md): other systems.
- [M0-RUNBOOK.md](../../M0-RUNBOOK.md): the operator's manual -- building, flashing, emulating, measuring.

## Two serial links, two speeds

- **A board's console** -- the DevKit's UART USB port -- runs at **115200** baud. Logs, `POT!` console commands,
  enrolment and the setup wizard use it; the hub can also carry the frame link over it.
- **The frame link** through a separate USB-serial adapter wired to a board's GPIO17/GPIO18 runs at **921600** baud.
  That is the default `--baud` of `ctl`, `fw`, `hub`, `agent`, `hostnode` and `stubs` (and of `apply`, which reads
  `"baud"` from the cluster file); pass `--baud 115200` to them when the port is a board's console.
  `potluck-capture` reads consoles, so its default is 115200.

## potluck-capture

`python -m potluck` (installed as `potluck-capture`) reads a board's serial stream, decodes Potluck Frames, records
them in the capture format of [ARCHITECTURE.md](../../ARCHITECTURE.md) section 7.6, and prints delivery ratio and a
round-trip delay histogram. Nothing is required to parse a saved log or capture; reading a port needs `pyserial`.

```bash
python -m potluck --list-ports
python -m potluck --port COM5                     # live table from a board's console (/dev/ttyUSB0 on Linux)
python -m potluck --port COM5 --capture captures/node1.jsonl --interval 60
python -m potluck --replay captures/node1.jsonl   # re-analyse, no board
python -m potluck --file node1.log --once         # a saved console log
```

Ctrl-C prints the summary and exits 0: interrupting a long run is how it ends, and the summary is the result.
`--show-frames` decodes every frame as it arrives (needs `CONFIG_POT_FRAME_TEE=y` in the firmware).

What it will not print:

- **No one-way delay.** The two boards' clocks are not synchronised and ESP-NOW's retries are not symmetric, so the
  tool reports round-trip time, the local transmit-queue delay, and the turnaround the peer measured on its own
  clock, and never divides anything by two.
- **No interpolated percentiles.** A board measures a 16-bucket histogram, so p50 and p99 are reported as the bucket
  interval containing them -- `2.0-3.0 ms`, not `2.47 ms`.
- **No zero standing in for unknown.** An unmeasured delivery ratio shows as `--` in the table and `unmeasured` in
  the summary.

## Why there are two frame decoders

`potluck/frame.py` is a second, independent implementation of the codec in
[`firmware/components/pot_frame`](../../firmware/components/pot_frame). That duplication is deliberate. A single
implementation can only be tested for self-consistency -- encode, decode, compare -- and a header field silently
swapped with its neighbour passes every time. Two implementations written from the specification and checked
against the same golden bytes test the specification itself.

The same reasoning covers `link_stats.py`: a board ships histogram counts without edges, so the host has to know the
bucket edges, and they can drift. `test_records.py` recomputes the board's own p50/p99 from its counts and fails if
the two disagree.

## Tests

`tools\run_all_tests.ps1` (from the repository root) runs the C++ suite, regenerates the fixtures, and runs every
Python test file. One file on its own:

```bash
python host/potluck/tests/test_frame.py
```

or `python -m pytest` from `host/potluck/`.

`tests/fixtures/stats_sample.jsonl` is generated by the C++ test binary, so the Python side is tested against
exactly the bytes a board emits:

```bash
build/tests/pot_tests.exe --emit-fixtures host/potluck/tests/fixtures
```

Regenerate it whenever `stats_json.cpp` changes. If a test then fails, the serial format changed and the Python side
needs updating to match -- which is the point of having the fixture.
