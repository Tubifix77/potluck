# Potluck in Home Assistant

This guide takes you from a Potluck cell to its boards in [Home Assistant](https://www.home-assistant.io/), step by
step. A **cell** is a group of Potluck boards that work as one (the [README](README.md) explains it); each board has a
**node id**, four hex digits such as `7368`, and the cell has a **cell id**, eight hex digits such as `ef76cc2e` taken
from the certificate authority that enrolled its boards. The person who set the cell up -- who made its keys and
enrolled its boards -- is its **owner** below; that may be you.

Four words used below:

- the **cluster file**: one JSON file that describes the whole cell -- its boards, the firmware they run, and what runs
  on them ([DEVELOPING-APPS.md](DEVELOPING-APPS.md) section 7). Its `system` is the cell's **name** (`lab` here).
- a **module**: an optional part of the boards' firmware, named in the cluster file's `firmware.modules` and built in
  only if named: `present` (the cell talks to an MQTT broker), `wifi` (a board joins your house Wi-Fi), `sandbox`,
  `pocket`.
- the **package**: what runs where, signed with the cell's keys. It is made from **manifests**, small JSON files listed
  in the cluster file's `package.manifests`; each says which board **owns** which resources (values such as
  `node-7368/hw/die_temp`) and which **actors** (the programs on the boards) produce them.
- an **extender**: a board built with the Wi-Fi extender application and given your Wi-Fi; it is on your network
  already. `python -m potluck.apply <cluster file> inventory` lists it among each board's modules; a cell without one
  needs the `wifi` module.

There are two paths, and you want both:

- **A. The boards themselves, through MQTT, with no PC.** One board on your Wi-Fi **presents** the cell: it publishes
  every board's exposed values to your MQTT broker, and Home Assistant shows each board as a device. If that board
  goes, another takes the role. This works with every PC switched off.
- **B. The hub and the Potluck integration.** The **hub** is a program on the PC that has the USB cable to the cell.
  While it runs, it adds health information to the same board devices over MQTT (alive or asleep, memory, signal,
  firmware), so it too needs path A's `present` and broker. The Potluck integration, installed with HACS, adds one
  device of its own, **Potluck hub**, with the hub's view (boards alive, apps working, problems). Without the
  integration you keep the board diagnostics and lose only that device.

## What you need

- Home Assistant 2026.10 or newer, with the [MQTT integration](https://www.home-assistant.io/integrations/mqtt/) and
  a broker (such as Mosquitto) it uses, and HACS for path B. If Home Assistant runs in Docker, give its container
  `network_mode: host`, or it will not see the hub announce itself (path B then needs the manual step below).
- A Wi-Fi network on **2.4 GHz** -- the boards have no 5 GHz radio. A network that offers both bands under one name
  is fine. A board that joins puts the cell on your router's channel: the whole cell follows the router.
- A PC set up as in [GETTING-STARTED.md](GETTING-STARTED.md), "What you need" and step 1: ESP-IDF, the repository
  cloned. Every `python` below is **ESP-IDF's Python**
  (`%USERPROFILE%\.espressif\python_env\idf6.0_*\Scripts\python.exe`), run from `host\potluck\` unless it says the
  repository's root.
- From the cell's owner: **the cluster file and its `keys/` folder**. An enrolled board accepts only packages signed
  by the cell's own keys, so a change to what the cell presents -- your broker's address -- must be signed with them.
  Put the folder at `<repository>\keys\` and the cluster file beside it. Then open the cluster file and fix what
  points at the owner's PC -- paths are relative to the cluster file, or absolute:
  - `keys`: the key files, now `keys\...` next to it;
  - `firmware.potluck.git`: your clone (`..`, if the file is in `keys\`); `rev` must be a commit that has the
    `present` module (any commit from 2026-10-12 on);
  - `port`: the COM port of the USB-serial adapter cabled to the cell (Device Manager > Ports);
  - `apps`: an app is built from its own repository; an entry you cannot reach must go, and its actors leave the boards
    with it (one package per cell). Ask the owner first.
  If you are taking the cell over for good, enrol every board under your own keys instead (GETTING-STARTED.md,
  steps 2 and 4).

## A. The boards in Home Assistant, with no PC

1. **See what is there:** `python -m potluck.apply <cluster file> inventory` lists each board (its node id, the
   modules it runs) and the actors on it. A resource exists only if an actor produces it: `node-7368/hw/die_temp`
   needs a `builtin:die_temp` actor pinned to board 7368 (GETTING-STARTED.md step 6 shows one).
2. **Give a board an LED switch** (optional; the automation below uses it). Copy this into a file `ha-led.json`
   beside the cluster file, with your board's node id in two places as hex and in `pin` as decimal (`0x7368` is
   29544; Windows' calculator in Programmer mode converts):

   ```json
   {
     "schema": 1,
     "system": "lab",
     "min_core_version": 1,
     "nodes": [
       {"node_id": 29544, "owns": [
         {"path": "potluck://lab/node-7368/led/on", "unit": "none", "kind": "sampled", "access": "read_write",
          "latency_class": 3}
       ]}
     ],
     "actors": [
       {"name": "led_7368", "module": "builtin:led_switch", "latency_class": 3, "pin": 29544,
        "config": {"r": 0, "g": 0, "b": 255}}
     ]
   }
   ```

   `system` is your cell's name. Add `"ha-led.json"` to the list in the cluster file's `package.manifests`. While the
   switch is on, the board's status LED shows the colour in `config` (blue here) instead of its usual green. Pick
   blue, cyan or white: purple, yellow, red and green are the board's own status colours, and an app colour in one
   of their hues (orange counts as yellow) is shown white. The status colours purple (updating), yellow (searching)
   and red (a problem) still win, so a board that is not green shows no change ([what the colours mean](GETTING-STARTED.md#3-build-and-flash-potluck-onto-each-board)).
3. **Say what Home Assistant may see**, in the cluster file. Add `present` to `firmware.modules` -- keep the modules
   already there -- and, if no board is an extender, `wifi`; name your broker; list every resource another system may
   read or write:

   ```json
   "firmware": {"potluck": {...}, "modules": ["sandbox", "present", "wifi"]},
   "present": {"broker": "192.168.0.77:1883"},
   "outside": {"node-7368/hw/die_temp": "read", "node-7368/led/on": "read_write"}
   ```

   The broker is an IP address with an optional port (the boards do not look up names). Nothing is exposed unless
   `outside` names it; `apply` does not check that something produces each path, and a path nothing produces never
   appears. A cell that presents is built with Wi-Fi's transmit aggregation off on every board (with it on, a
   presenting board's radio stalled every 10-60 minutes), which costs an extender's hotspot some speed.
4. **Apply it**: `python -m potluck.apply <cluster file> --plan`, read what it will do, then the same without
   `--plan`. It builds the firmware with the modules (5-9 minutes the first time), rolls it out board by board (about
   2 minutes each) and sends the package; it ends with `VERIFIED`. A `REFUSED` line names what is wrong and changes
   nothing.
5. **Give two boards your Wi-Fi**, so presenting survives one of them going (skip this if a board is an extender).
   Make a text file of two lines, the network's name and its password, and for each board run, from the repository's
   root, on that board's own USB console port (not the adapter's):

   ```
   python tools\wifi_set.py COM5 C:\path\to\wifi.txt
   ```

   It prints neither the name nor the password, only `board: sta saved`, and restarts the board. Delete the file
   afterwards. Run it after step 4, so the board's firmware has the Wi-Fi module.
6. **If your broker needs a login**, send it to each board on your Wi-Fi, from the repository's root:

   ```
   python tools\console_send.py COM5 "POT! present login <user> <password>"
   ```

   It prints only the result; the board keeps the login in its flash across restarts and new packages. The command
   stays in your shell's history: clear it afterwards (PowerShell: `Clear-History`, and delete the line from
   `(Get-PSReadLineOption).HistorySavePath`).
7. **Look in Home Assistant**: Settings > Devices & services > MQTT. Each board is a device named `<cell name>
   node-<node id>` (for example `lab node-7368`), the cell itself a device `<cell name> cell`, and each exposed
   resource an entity named after its path (`hw/die_temp`). Within a few seconds of the presenting board connecting,
   values arrive every 2 seconds. Home Assistant's discovery prefix must be the default, `homeassistant`. After Home
   Assistant or the broker restarts, the presenting board sends everything again by itself.

**If nothing appears:** check that the board on your Wi-Fi is connected. `python tools\console_tap.py COM5 60
board.log` (from the repository's root) records a minute of its console; the presenting board's file has lines
`{"t":"present", ... "connected": 1, ...}`. Or the hub's dashboard (path B) lists its `present` module as `active`. Check too that your broker accepts connections from your Wi-Fi (an anonymous
broker, or the login in step 6).

## B. The hub and the Potluck integration

1. **Make the hub a certificate**, once. The hub serves Home Assistant and phones over HTTPS with a certificate of its
   own. Find the PC's address with `ipconfig` (the IPv4 address on your house network), then in **Git Bash**, in
   `host/potluck/`, with your address in place of `192.168.0.7`:

   ```
   MSYS_NO_PATHCONV=1 openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes \
     -keyout hub-key.pem -out hub-cert.pem -days 825 -subj "/CN=potluck-hub" -addext "subjectAltName=IP:192.168.0.7"
   ```

   (`MSYS_NO_PATHCONV=1` stops Git Bash from turning `/CN=...` into a file path.) Keep `hub-key.pem` to yourself. The
   certificate names the address. If your router gives the PC another one, Home Assistant follows the hub's
   announcement by itself, but a phone's browser warns until you make a certificate for the new address -- and a new
   certificate makes Home Assistant ask you to confirm it and allow it again. Better: give the PC a fixed address in
   the router.
2. **Start the hub**, from `host\potluck\`, with the USB-serial adapter's port:

   ```
   python -m potluck.hub --port COM6 --cluster <cluster file> --listen 0.0.0.0:8443 --tls-cert hub-cert.pem --tls-key hub-key.pem
   ```

   Add `--broker-login <file>` (two lines: the broker's user name and password) if your broker needs a login. A
   healthy start prints, within seconds:

   ```
   [hub] dashboard for the network at https://0.0.0.0:8443/ ...
   [hub] announced as Potluck lab ef76cc2e._potluck._tcp.local. at 192.168.0.7
   [hub] holding COM6; API http://127.0.0.1:8470/ ...
   [hub] presenting: connected to 192.168.0.77:1883
   ```

   The last line is the hub reaching your broker for its health entities. When Windows asks, allow Python **on
   private networks** (TCP 8443 for Home Assistant, UDP 5353 for the announcement), and make sure Windows calls your
   house network private, not public. The hub now answers on two addresses: `http://127.0.0.1:8470/`, its
   **dashboard**, on the PC itself (no login: that is you, the owner) and `https://<PC address>:8443/` for everyone
   else (a login, or a paired program). It runs while its window is open; nothing starts it for you. `potluck apply`
   and `potctl` work while it runs: they go through it.
3. **Install the integration with HACS**: in Home Assistant, HACS > the three dots > Custom repositories, add
   `https://github.com/Tubifix77/potluck` as type **Integration**. Close the dialog, type `Potluck` in the HACS page's
   search box, select **Potluck**, then **Download** (bottom right) and **Download** again in the dialog. Restart
   Home Assistant (Settings > System > the three dots > Restart). Just after HACS itself is set up, its list stays
   empty for a few minutes while it loads; search again after that.
4. **Accept the discovery**: Settings > Devices & services shows **Potluck** under **Discovered**, named after the
   cell (`lab (ef76cc2e)`: its name and its cell id). Select **Add**, then **Submit**. Home Assistant asks the hub to
   be paired and waits up to 5 minutes.
5. **Allow it on the PC**: open the dashboard, `http://127.0.0.1:8470/`, the **Cell** tab: the hub's card shows
   **Asking to connect: Home Assistant**, with its address. Select **Allow**. Home Assistant finishes by itself and a
   device **Potluck hub (lab)** appears. If the 5 minutes ran out, select Add again.

**If Home Assistant discovers nothing** (Docker without host networking, another network segment, multicast
blocked): Settings > Devices & services > Add integration > **Potluck**, the PC's address and `8443`. Home Assistant
shows the fingerprint of the certificate it was served, as pairs of hex digits (`88:DE:35:...`); compare it with the
hub's card on the PC (**Certificate**) -- the same digits, the same order -- then continue and allow as in step 5.

**Afterwards:** each board device also shows the hub's diagnostics (state, internal RAM free, uptime, signal, worst
link delivery) and a **Firmware** update entity that shows the board's version against the newest in the cell and
installs nothing; the cell's device shows apps working and a problem sensor per app ("passive-sensor problem"). With
the PC off, these and the Potluck hub device go unavailable, and the boards' own entities stay live through path A.
To take Home Assistant's access back: `python -m potluck.hubauth revoke "Home Assistant"`.

## An automation on a board's LED

The LED from step A.2 is a switch, `switch.<cell name>_node_<node id>_led_on` -- `switch.lab_node_7368_led_on` here.
In Home Assistant: Settings > Automations & scenes > Create automation > Create new automation, then the three dots >
Edit in YAML, and paste (this one lights the LED at sunset and turns it off at 23:00):

```yaml
alias: Potluck LED in the evening
triggers:
  - trigger: sun
    event: sunset
    id: "on"
  - trigger: time
    at: "23:00:00"
    id: "off"
actions:
  - choose:
      - conditions:
          - condition: trigger
            id: "on"
        sequence:
          - action: switch.turn_on
            target:
              entity_id: switch.lab_node_7368_led_on
    default:
      - action: switch.turn_off
        target:
          entity_id: switch.lab_node_7368_led_on
```

Save it, and run it once from its three dots > Run actions to see the LED turn blue. The write goes from the
presenting board to the LED's board, marked as from outside the cell, and that board checks the package again. A
resource not listed as `read_write` in `outside` cannot be written from Home Assistant at all.

## More

How any other system uses the cell, and what the MQTT messages contain: [INTEGRATIONS.md](INTEGRATIONS.md). The hub's
API in full: [host/potluck/HUB-API.md](host/potluck/HUB-API.md).
