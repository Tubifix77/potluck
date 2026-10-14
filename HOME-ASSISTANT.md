# Potluck in Home Assistant

This guide takes you from a Potluck cell to its boards in [Home Assistant](https://www.home-assistant.io/), step by
step. A **cell** is a group of Potluck boards that work as one (the [README](README.md) explains it); each board has a
**node id**, four hex digits such as `7368`, and the cell has a **cell id**, eight hex digits such as `ef76cc2e` taken
from the certificate authority that enrolled its boards. The person who set the cell up -- who made its keys and
enrolled its boards -- is its **owner** below; that may be you.

There are two paths, and you want both:

- **A. The boards themselves, through MQTT, with no PC.** One board on your Wi-Fi **presents** the cell: it publishes
  every board's exposed values to your MQTT broker, and Home Assistant shows each board as a device. If that board
  goes, another takes the role. This works with every PC switched off.
- **B. The hub, through the Potluck integration.** The **hub** is a program on the PC that has the USB cable to the
  cell. With it running, the same board devices gain health information (alive or asleep, memory, signal, firmware),
  and a Potluck device shows the hub's own view (boards alive, apps working, problems). You install it with HACS.

## What you need

- Home Assistant 2026.10 or newer, with the [MQTT integration](https://www.home-assistant.io/integrations/mqtt/) and
  a broker (such as Mosquitto) it uses, and HACS for path B. If Home Assistant runs in Docker, give its container
  `network_mode: host`, or it will not see the hub announce itself (path B then needs the manual step below).
- From the cell's owner: **the cluster file** (a JSON file describing the cell, [DEVELOPING-APPS.md](DEVELOPING-APPS.md)
  section 7) **and its `keys/` folder**. An enrolled board accepts only packages signed by the cell's own keys, so a
  change to what the cell presents -- your broker's address -- must be signed with them. If you are taking the cell
  over for good, enrol every board under your own keys instead ([GETTING-STARTED.md](GETTING-STARTED.md), steps 2
  and 4).
- For `potluck apply` (path A): a PC set up as in [GETTING-STARTED.md](GETTING-STARTED.md), "What you need" --
  ESP-IDF, because the presenting module must be built into the boards' firmware the first time.
- For the hub (path B): Python 3 with `pyserial` (`pip install pyserial`), or ESP-IDF's own Python, which has it.

## A. The boards in Home Assistant, with no PC

1. **Say what Home Assistant may see**, in the cluster file. Add the presenting module and, if no board has an
   extender's Wi-Fi, Potluck's own Wi-Fi module; name your broker; list every resource another system may read or
   write:

   ```json
   "firmware": {"potluck": {...}, "modules": ["present", "wifi"]},
   "present": {"broker": "192.168.0.77:1883"},
   "outside": {"node-7368/hw/die_temp": "read", "node-7368/led/on": "read_write"}
   ```

   The broker is an IP address with an optional port (the boards do not look up names). Nothing is exposed unless
   `outside` names it. A writable entity needs the resource declared writable by its board -- for a board's status
   LED, the package's manifest gives that board `{"path": "potluck://lab/node-7368/led/on", "unit": "none",
   "kind": "sampled", "access": "read_write", "latency_class": 3}` in its `owns` and the actor
   `builtin:led_switch` (see `manifests/m13-present.json`).
2. **Apply it**, from `host/potluck/`: `python -m potluck.apply <cluster file> --plan`, read what it will do, then
   the same without `--plan`. It builds the firmware with the modules, rolls it out board by board, and sends the
   package; it ends with `VERIFIED`.
3. **Give two boards your Wi-Fi**, so presenting survives one of them going: make a text file of two lines, the
   network's name and its password, and for each board run (with ESP-IDF's Python, on the board's own USB port):

   ```
   python tools\wifi_set.py COM5 C:\path\to\wifi.txt
   ```

   It prints neither the name nor the password, only `board: sta saved`, and restarts the board. Delete the file
   afterwards. Run it after step 2, so the board's firmware has the Wi-Fi module.
4. **If your broker needs a login**, type `POT! present login <user> <password>` on the console of each of those two
   boards (it prints only the result; the boards keep it in their flash across restarts and new packages).
5. **Look in Home Assistant**: Settings > Devices & services > MQTT. Each board is a device named `<cell name>
   node-<node id>` (for example `lab node-7368`), the cell itself a device `<cell name> cell`, and each exposed
   resource an entity named after its path (`hw/die_temp`). Within a few seconds of the presenting board connecting,
   values arrive every 2 seconds. Home Assistant's discovery prefix must be the default, `homeassistant`. After Home
   Assistant or the broker restarts, the presenting board sends everything again by itself.

**If nothing appears:** check that the board on your Wi-Fi is connected -- the hub's dashboard lists its `present`
module as `active` (path B), or its console prints `{"t":"present", ... "connected": 1, ...}` -- and that your broker
accepts connections from the boards' network (an anonymous broker, or the login in step 4).

## B. The hub and the Potluck integration

1. **Make the hub a certificate**, once. The hub serves Home Assistant and phones over HTTPS with a certificate of its
   own. Find the PC's address with `ipconfig` (the IPv4 address on your house network), then in **Git Bash**:

   ```
   MSYS_NO_PATHCONV=1 openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes \
     -keyout hub-key.pem -out hub-cert.pem -days 825 -subj "/CN=potluck-hub" -addext "subjectAltName=IP:192.168.0.7"
   ```

   (`MSYS_NO_PATHCONV=1` stops Git Bash from turning `/CN=...` into a file path.) Keep `hub-key.pem` to yourself.
2. **Start the hub**, from `host/potluck/`, with the USB-serial adapter's port (Device Manager, Ports):

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

   The last line is the hub reaching your broker for its health entities (it needs `present` in the cluster file).
   Keep the window open while you use it. When Windows asks, allow Python **on private networks** (TCP 8443 for Home
   Assistant, UDP 5353 for the announcement), and make sure Windows calls your house network private, not public.
   The hub now answers on two addresses: `http://127.0.0.1:8470/` on the PC itself (no login: that is you, the owner)
   and `https://<PC address>:8443/` for everyone else (a login, or a paired program).
3. **Install the integration with HACS**: HACS > the three dots > Custom repositories, add
   `https://github.com/Tubifix77/potluck` as type **Integration**, open **Potluck**, **Download**, then restart Home
   Assistant (Settings > System > the three dots > Restart).
4. **Accept the discovery**: Settings > Devices & services shows **Potluck** under **Discovered**, named after the
   cell (`lab (ef76cc2e)`: its name and its cell id). Select **Add**, then
   **Submit**. Home Assistant asks the hub to be paired and waits up to 5 minutes.
5. **Allow it on the PC**: open `http://127.0.0.1:8470/`, the **Cell** tab: the hub's card shows **Asking to connect:
   Home Assistant**, with its address. Select **Allow**. Home Assistant finishes by itself and a device **Potluck hub
   (lab)** appears. If the 5 minutes ran out, select Add again.

**If Home Assistant discovers nothing** (Docker without host networking, another network segment, multicast
blocked): Settings > Devices & services > Add integration > **Potluck**, the PC's address and `8443`. Home Assistant
shows the fingerprint of the certificate it was served, as pairs of hex digits (`88:DE:35:...`); compare it with the
hub's card on the PC (**Certificate**) -- the same digits, the same order -- then continue and allow as in step 5.

**Afterwards:** each board device also shows the hub's diagnostics (state, internal RAM free, uptime, signal, worst
link delivery) and a **Firmware** update entity that shows the board's version against the newest in the cell and
installs nothing; the cell's device shows apps working and a problem sensor per app ("passive-sensor problem"). With
the PC off, these and the Potluck hub device go unavailable, and the boards' own entities stay live through path A.
To take Home Assistant's access back: `python -m potluck.hubauth revoke "Home Assistant"`, from `host/potluck/`.

## An automation on a board's LED

The LED from step A.1 is a switch, `switch.lab_node_7368_led_on`. Any automation can turn it on or off; the write goes
from the presenting board to the LED's board, marked as from outside the cell, and that board checks the package again.
A resource not listed as `read_write` cannot be written from Home Assistant at all.

## More

How any other system uses the cell, and what the MQTT messages contain: [INTEGRATIONS.md](INTEGRATIONS.md). The hub's
API in full: [host/potluck/HUB-API.md](host/potluck/HUB-API.md).
