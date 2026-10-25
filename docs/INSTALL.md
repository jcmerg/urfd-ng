# Installing urfd and tcd

**English** | [Deutsch](INSTALL.de.md)

This guide sets up a URF reflector, with its web dashboard and optionally the tcd transcoder, using the setup script. You need no Linux experience beyond typing a few commands. The script asks a handful of questions, writes the configuration for you, and builds and starts everything.

- **urfd** is the reflector. It runs in Docker, together with the dashboard.
- **tcd** is the transcoder. It converts audio between D-Star, DMR/YSF, M17, P25 and NXDN, using DVSI AMBE USB sticks. The setup script runs it in Docker or directly on the machine with systemd, as you choose; both work with the sticks (see "tcd in Docker" in [tcd/README.md](../tcd/README.md#tcd-in-docker)). Without a transcoder the reflector still works, but users only hear stations that use the same mode.

## What you need

- **A computer that is always on**: a PC, a small server, a VPS, or a Raspberry Pi 4/5. 2 GB of RAM is enough.
- **Linux**: Debian 12, Ubuntu 22.04 or newer, or Raspberry Pi OS (64-bit recommended). A fresh install is best.
- **Internet access** from that computer, and the ability to open ports in your router or firewall (see [Ports](#ports)).
- **For the transcoder**: DVSI AMBE USB sticks (DV3000/ThumbDV or DV3003), plugged into the machine that runs tcd.
  - One device (DV3000 or DV3003): 1 transcoded module, DMR/YSF then needs a [codec plugin](../tcd/README.md#codec-plugins).
  - DV3000 + DV3003: 2 modules. Two DV3000: 1 module. Two DV3003: up to 3 modules.
  - More modules, or no stick at all: a [codec plugin](../tcd/README.md#codec-plugins) for D-Star and DMR does them in software (not part of this repository, `setup.sh` does not install it).
- **A reflector callsign**: `URF` plus three letters or digits, e.g. `URF363`. Check that it is not taken in the [reflector list](https://xlxapi.rlx.lu/api/exportreflectors).

## Which setup?

| Your situation | Choose in the script |
|---|---|
| Reflector and AMBE sticks on the same machine | **1** Reflector and transcoder |
| Reflector without transcoder | **2** Reflector only, answer **no** to "transcoder on another machine" |
| Reflector on a server or VPS, AMBE sticks on a Raspberry Pi at home | On the server: **2** Reflector only, answer **yes**. On the Pi: **3** Transcoder only |

## Step by step

### 1. Log in and get the software

Log in to the machine (directly, or with `ssh` from another computer), then run:

```bash
sudo apt update
sudo apt install -y git
git clone https://github.com/jcmerg/urfd-ng.git urfd
cd urfd
```

### 2. Run the setup script

```bash
sudo ./setup.sh
```

The script asks its questions one after another. A value in `[brackets]` is the suggestion: just press Enter to take it.

| Question | What to enter |
|---|---|
| What should run on this machine? | 1, 2 or 3, see [Which setup?](#which-setup) |
| Reflector callsign | Your `URFxxx` callsign |
| Sysop e-mail address | Where users and directory admins can reach you |
| Country code | Two letters, e.g. `DE` |
| Sponsor or club | Shown on the dashboard |
| Dashboard address | How the dashboard is reached from outside, e.g. `http://urf123.example.com:8363` or `http://your-ip:8363`. The suggestion is the local address, fine for a first test |
| Modules | The "rooms" of the reflector, letters A to Z. `ADMSZ` is a good start |
| Echo/parrot module | A module that plays back what you say, ideal for testing. Usually `Z` |
| Transcoded modules | The modules whose audio is converted between modes, as many as your AMBE hardware allows (more with a codec plugin) |
| Admin page | Lets you manage talkgroups and see the live log in the dashboard. The script creates a password |
| Register in the XLX directory | Lists the reflector in Pi-Star, WPSD and other hotspot software. Say **no** for now and register later, once the reflector works and is reachable from the internet |
| Install Docker | Yes, if the script offers it |
| AMBE hardware | 1 for a single device (DV3000 or DV3003), 2 for two devices, 3 for none (a codec plugin does everything in software) |
| Serial numbers | The script lists the connected USB serial devices. Enter the serial numbers of your AMBE sticks, or press Enter to let tcd pick the AMBE sticks it recognises |

Building takes a few minutes, on a Raspberry Pi up to half an hour. At the end the script shows the dashboard address, the admin password and the ports to open. Everything it prints is also saved in `/var/log/urfd-setup.log`.

### 3. Open the ports

Forward these ports from your router to the machine, or open them in the firewall of your VPS. Only the protocols you want to offer are needed.

### Ports

| Port | Protocol | For |
|---|---|---|
| 8363/tcp | HTTP | Dashboard |
| 17000/udp | M17 | M17 clients and M17 reflector interlinks |
| 20001/udp | DPlus | D-Star (DPlus) |
| 30001/udp | DExtra | D-Star (DExtra) |
| 30051/udp | DCS | D-Star (DCS) |
| 62030/udp | MMDVM | DMR hotspots, only after enabling `[MMDVM]` in `urfd.ini` (see below) |
| 8880/udp | DMR+ | DMR+ |
| 42000/udp | YSF | Fusion (YSF) |
| 41000/udp | P25 | P25 |
| 41400/udp | NXDN | NXDN |
| 10017/udp | URF | Interlinks with other URF reflectors |
| 10100/tcp | Transcoder | Only if tcd runs on another machine, and only from that machine |

### 4. Test it

1. Open the dashboard in a browser. The live dashboard is at `/live/` on the same address, e.g. `http://urf123.example.com:8363/live/`; if you put the dashboard behind your own TLS proxy, it has to pass `/ws` on as a WebSocket (`Upgrade` headers, see [docs/WEBSOCKET.md](WEBSOCKET.md#setup)).
2. Link a hotspot or radio to your reflector and module, e.g. `URF123 Z` for the echo module.
3. Say something and let go: after 3 seconds you hear yourself.
4. Test a transcoded module across modes, e.g. D-Star and DMR on module `A`.

### 5. Register in the XLX directory (optional)

Once everything works from outside, the reflector can be listed in hotspot software:

1. Edit `/opt/urfd/config/urfd.ini`, section `[CallingHome]`: set `Enable = true`, the `Country` name (e.g. `Germany`) and a `Comment`. The `DashboardUrl` from `[Names]` is sent along.
2. Moving a reflector that the directory lists already (an XLX dashboard or an older urfd registered it)? Then put its old hash in the section as `Hash = ...`, see the [README](../README.md#moving-a-reflector-that-is-listed-already). Otherwise leave `Hash` blank: the reflector makes one at its first start and saves it there.
3. Restart: `docker restart urfd`.

The reflector then reports to the directory every 5 minutes; `docker logs urfd | grep CallingHome` shows whether it got through. If you answered yes to this question during setup, it is already on.

## Daily use

| Task | Command |
|---|---|
| Update to the latest version | `sudo ~/urfd/setup.sh update` |
| What runs here, in which version | `sudo ~/urfd/setup.sh status` |
| Reflector log | `sudo ~/urfd/setup.sh logs urfd` (or `docker logs -f urfd`) |
| Restart the reflector | `sudo ~/urfd/setup.sh restart urfd` (or `docker restart urfd`) |
| Reload lists, interlink, TG mappings | `sudo ~/urfd/setup.sh reload` |
| Transcoder log | `sudo ~/urfd/setup.sh logs tcd` (or `docker logs -f tcd`, natively `journalctl -u tcd -f`) |
| Restart the transcoder | `sudo ~/urfd/setup.sh restart tcd` (or `docker restart tcd`, natively `sudo systemctl restart tcd`) |
| Transcoder monitor | `docker exec -it tcd tcdmon` (natively `tcdmon`) in a terminal, next to the reflector `http://<reflector>:8363/tcd/`, or `http://localhost:8080` in a browser on that machine (from another computer: `ssh -L 8080:localhost:8080 <machine>`, or `MonitorBindAddress = 0.0.0.0` in `tcd.ini` on a trusted LAN) |

The update keeps your configuration. It pulls the new version, rebuilds and restarts.

## Changing the configuration

| File | What's in it |
|---|---|
| `/opt/urfd/config/urfd.ini` | Reflector: modules, protocols, XLX directory registration, BrandMeister connection, D-Star and YSF links, interlinks. Every setting is explained in the file |
| `/opt/urfd/config/urfd.interlink` | Links to other reflectors |
| `/opt/urfd/config/urfd.blacklist`, `urfd.whitelist` | Callsigns that may or may not link |
| `/opt/urfd/dashboard/config.inc.php` | Dashboard: banner text, IP privacy, admin page |
| `/opt/urfd/tcd/tcd.ini` (natively `/usr/local/etc/tcd.ini`) | Transcoder: reflector address, modules, AMBE serial numbers, audio levels |

After editing, restart the reflector (`sudo ~/urfd/setup.sh restart urfd`) or the transcoder (`sudo ~/urfd/setup.sh restart tcd`). Whitelist, blacklist, interlink and TG mappings need no restart: `sudo ~/urfd/setup.sh reload`. Check `urfd.ini` before a restart with:

```bash
sudo ~/urfd/setup.sh check
```

The transcoded modules are listed in `tcd.ini` (`Modules`); `urfd.ini` says `[Transcoder] Modules = auto` (written by the setup script since 4.25.0), so urfd follows tcd. An older `urfd.ini` with a list there needs the same modules as `tcd.ini`.

**DMR hotspots** (Pi-Star, WPSD and other MMDVM hotspots) link through the MMDVM server, which is off after setup. To offer it, uncomment the `[MMDVM]` section in `urfd.ini`, map talkgroups to modules (e.g. `TG9 = A,TS2`), and either add users with passwords or set `RequireAuth = false` for open access. Then open port 62030/udp and restart the reflector.

## Troubleshooting

**The dashboard does not load.** Check that the container runs: `docker ps` should list `urfd` as `healthy`. If not, look at `docker logs urfd`, usually an error in `urfd.ini`.

**Users cannot link.** The port for their protocol is not open or forwarded, see [Ports](#ports). Test from outside your network, e.g. with a hotspot on a phone hotspot.

**Modules are not transcoded.** `docker logs urfd | grep -i transcoder` shows whether tcd connected. On the tcd machine, `docker logs tcd` (natively `journalctl -u tcd`) shows whether it found the AMBE sticks; `docker exec tcd tcd --list-devices` (natively `tcd --list-devices`) lists them. If tcd runs on another machine, `ServerAddress` in `tcd.ini` must be the reflector's address and port 10100/tcp must be reachable from there; `Password` in `tcd.ini` must be the reflector's `[Transcoder] Password` (the setup script prints it at the end of the reflector's installation). A wrong one shows as `refused: wrong password` in `docker logs urfd`.

**Run the setup again.** `sudo ./setup.sh` on an installed machine offers an update. To start over, answer no to the update and yes to set it up again: this overwrites `urfd.ini`, the dashboard config and `tcd.ini`.

## Setting it up by hand

Without the setup script, or without Docker: [INSTALL-ADVANCED.md](INSTALL-ADVANCED.md).

## Removing everything

```bash
cd ~/urfd/docker && sudo docker compose down --rmi all -v
sudo rm -rf /opt/urfd
# a native transcoder:
sudo systemctl disable --now tcd
sudo rm -f /etc/systemd/system/tcd.service /usr/local/bin/tcd /usr/local/bin/tcdmon /usr/local/bin/agc-analyze /usr/local/etc/tcd.ini /usr/local/etc/tcd-build.conf
```
