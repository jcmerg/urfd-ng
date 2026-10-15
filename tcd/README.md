# tcd — Hybrid Digital Voice Transcoder

**English** | [Deutsch](README.de.md)

**Version 0.7.1** — the transcoder of [urfd-ng](../README.md), in the `tcd/` folder of the same repository. Based on [n7tae/tcd](https://github.com/n7tae/tcd).

tcd converts voice between the digital modes on a URF reflector, so a D-Star user hears a DMR user and the other way round. It uses DVSI AMBE hardware for D-Star (AMBE) and DMR/YSF (AMBE2+), and software vocoders for M17 (Codec2), P25/NXDN (IMBE) and, optionally, DMR/YSF (MD380). The reflector hands the audio of its transcoded modules to tcd over TCP and gets every other mode back.

## Contents

- [Install](#install)
- [Hardware](#hardware)
- [Selecting the AMBE devices](#selecting-the-ambe-devices)
- [Configuration (tcd.ini)](#configuration-tcdini)
- [Audio levels](#audio-levels)
- [Monitoring](#monitoring)
- [Operation and troubleshooting](#operation-and-troubleshooting)
- [Changes from upstream](#changes-from-upstream)

## Install

### With the setup script

The easiest way: the setup script in the repository root installs reflector and transcoder together, or the transcoder alone on a second machine, asks a few questions and writes `tcd.ini`. See [docs/INSTALL.md](../docs/INSTALL.md).

```bash
git clone https://github.com/jcmerg/urfd-ng.git urfd
cd urfd
sudo ./setup.sh
```

### With build.sh

To build and install only the transcoder:

```bash
git clone https://github.com/jcmerg/urfd-ng.git urfd
cd urfd
sudo bash tcd/build.sh               # two or more AMBE devices
sudo bash tcd/build.sh --with-md380  # a single DV3000, DMR/YSF in software
```

`build.sh` installs the build tools and the vocoder libraries ([imbe_vocoder](https://github.com/jcmerg/imbe_vocoder), and with `--with-md380` [md380_vocoder](https://github.com/jcmerg/md380_vocoder): native on armhf, through [dynarmic](https://github.com/jcmerg/md380_vocoder_dynarmic) on x86_64 and arm64), builds tcd as the owner of the checkout and installs `tcd`, `tcdmon`, `agc-analyze` and the systemd unit. A running tcd is restarted. The first install puts the template at `/usr/local/etc/tcd.ini`; edit it (see [Configuration](#configuration-tcdini)) and start with `sudo systemctl start tcd`.

To update: `git pull` in the checkout and run `build.sh` again, or `sudo ./setup.sh update`.

### By hand

With the libraries installed:

```bash
cd urfd/tcd
make -j$(nproc)             # without MD380
make -j$(nproc) md380=true  # with the MD380 software vocoder
sudo make install
```

tcd builds against the urfd sources it shares with the reflector (`TCPacketDef.h`, `TCSocket.*`, ...) from `../reflector`, so both sides always speak the same protocol. `config/tcd.ini` is the commented template, `config/tcd.service` the systemd unit.

### Requirements

- Linux with systemd: Debian, Ubuntu or Raspberry Pi OS; x86_64, arm64 or armhf
- DVSI AMBE USB devices (see [Hardware](#hardware)); they use the kernel's `ftdi_sio` driver, no extra driver needed
- `libspeexdsp-dev` for the noise suppression of analog input; `build.sh` installs it
- Low latency to the AMBE devices: tcd runs natively, not in Docker. It can run on the reflector's machine or on another one, e.g. a Raspberry Pi next to the radio gear.

## Hardware

Each transcoded module needs a D-Star channel and a DMR/YSF channel. A DV3000 (ThumbDV, DVstick-30, USB-3000) has one channel, a DV3003 (DVstick-33, USB-3003) three. Without AMBE hardware for DMR/YSF, the MD380 software vocoder takes that part (build with `md380=true` / `--with-md380`).

| Hardware | Build | Modules | Notes |
|---|---|---|---|
| 1× DV3000 or 1× DV3003 | MD380 required | 1 | D-Star on the device, DMR/YSF in software |
| DV3000 + DV3003 | without MD380 | 2 | mixed mode, recommended |
| 2× DV3000 | without MD380 | 1 | one for D-Star, one for DMR/YSF |
| 2× DV3003 | without MD380 | up to 3 | one for D-Star, one for DMR/YSF |

A single device always needs the MD380 build; tcd refuses to start without it. With two devices, the MD380 is not needed: since 0.2.5 `DmrGainMode = ambe` gives DMR → DMR/YSF the same AGC without re-encoding (see [`DmrGainMode`](#dmrysf--dmrysf-level-dmrgainmode)). Without the MD380, tcd builds and runs on any platform (x86_64, arm64, armhf) with no ARM code to emulate; the MD380 build only adds `reencode` as an alternative. tcd detects the devices and picks the mode itself.

### Mixed mode: DV3000 + DV3003

Two modules at once, with the channels assigned automatically:

| Channel | Use |
|---|---|
| DV3000 ch0 | D-Star, first module |
| DV3003 ch0 | D-Star, second module |
| DV3003 ch1 | DMR/YSF, first module |
| DV3003 ch2 | DMR/YSF, second module |

```ini
Modules = FS            # first = D-Star on the DV3000, second = D-Star on the DV3003
DeviceSerial = DQ015SBR # DV3000
DeviceSerial = DKB7FXGE # DV3003
```

### Single device with MD380

One module, D-Star on the DV3000 or DV3003 and DMR/YSF through the MD380. In this mode, DMR/YSF → DMR/YSF audio is **not re-encoded** after the AGC, because the MD380 cannot decode and re-encode in the same pipeline (shared encoder/decoder state). Use the bitstream gain (`DmrGainMode = ambe`, which follows the AGC; in this mode since 0.2.5) for that path; all paths between different modes still get the full AGC. Re-encode on DMR/YSF → DMR/YSF needs a second AMBE device.

### DV3003 notes

- **Companding off**: every DV3003 channel gets companding explicitly disabled. The DV3003 defaults to companding on (CP_ENABLE pull-up), the DV3000 to off; without this, the DMR/YSF channels got µ-law PCM read as linear, which distorts the audio.
- **Serial flush**: before the soft reset, 350 zero bytes clear any stale serial state.
- **Per-channel codec**: one DV3003 can run D-Star and DMR/YSF channels at the same time (mixed mode).

## Selecting the AMBE devices

```bash
tcd --list-devices
```

lists the USB serial devices from `/dev/serial/by-id` with their serial numbers and marks the ones that look like AMBE devices.

Without `DeviceSerial` in `tcd.ini`, tcd uses every device it recognises as AMBE by its name. With other USB serial devices on the machine (GPS, CAT cables, ...), list your AMBE devices explicitly. `DeviceSerial` must match exactly one of:

- the USB serial number (`DKB7FXGE`)
- the older libftd2xx form with the port letter of multi-port devices (`DA1234A`)
- the `/dev/serial/by-id` name
- the device node (`/dev/ttyUSB0`)

tcd opens the devices through the kernel's `ftdi_sio` driver and sets their USB latency timer to 1 ms (the driver default is 16 ms, too slow for the AMBE timing; the log shows the value). Devices that an older, libftd2xx based tcd left unbound from `ftdi_sio` are bound back at startup, so an upgrade needs no replugging.

## Configuration (tcd.ini)

`/usr/local/etc/tcd.ini`; the template in [`config/tcd.ini`](config/tcd.ini) explains every key. Restart tcd after changes (`sudo systemctl restart tcd`), or change the audio settings live in the [web dashboard](#web-dashboard).

### Connection and devices

| Key | Template | Meaning |
|---|---|---|
| `ServerAddress` | `127.0.0.1` | address of the urfd reflector |
| `Port` | `10100` | the reflector's `[Transcoder] Port` |
| `Modules` | `FS` | transcoded modules, the same letters as the reflector's `[Transcoder] Modules` |
| `DeviceSerial` | — | AMBE device to use, one line each (see [above](#selecting-the-ambe-devices)) |

### Gains

All gains are in dB, -40 to +40.

| Key | Template | Applied to |
|---|---|---|
| `OutputGainDStar` | `0` | D-Star output, after the AGC |
| `OutputGainDMR` | `-18` | DMR/YSF output, after the AGC; every path to DMR/YSF, including DMR → DMR/YSF |
| `OutputGainDMRBitstream` | `0` | experimental, -24 to 0 dB: for D-Star, M17, P25, USRP and SVX sources this much of `OutputGainDMR` is applied through b2 in the AMBE2+ frame after the encoder, the rest as PCM before it, so the encoder gets more level; live in the monitor |
| `OutputGainM17` | `0` | M17/Codec2 output, after the AGC |
| `OutputGainIMBE` | `0` | P25/NXDN output, after the AGC |
| `OutputGainUSRP` | `0` | USRP and SVX output, after the AGC (both get the same PCM from tcd) |
| `DStarGainIn` / `DStarGainOut` | `0` | inside the DVSI chip, D-Star decode / encode |
| `DmrGainIn` / `DmrGainOut` | `0` | inside the DVSI chip, DMR decode / encode |

SVX and USRP gains per connection are set in the reflector's `urfd.ini` (`RxGain`/`TxGain` in `[SvxReflector]`, `[SVXServer]` and `[USRP]`); `OutputGainUSRP` here sets the level of the PCM tcd returns for both.

### DMR/YSF → DMR/YSF level (`DmrGainMode`)

DMR to DMR/YSF audio normally passes through as AMBE2+ without being decoded, so the AGC cannot touch it. `DmrGainMode` decides how its level is adjusted:

| Mode | Needs | What it does |
|---|---|---|
| `off` | — | no adjustment |
| `ambe` | — | applies the AGC's gain plus `AmbeGainDb` (-30 to +10 dB) and `OutputGainDMR` through the b2 (delta-gamma) gain parameter in the AMBE2+ frame. No re-encode, no quality loss, also repairs Golay FEC errors. Since 0.2.5 it works in the gamma domain and is level independent (see below). The default. |
| `reencode` | MD380 | full decode and re-encode through the MD380, with AGC and `DmrReencodeGain`; an alternative to `ambe`, same level with the same value. Falls back to `ambe` without MD380. |

| Key | Template | Code default |
|---|---|---|
| `DmrGainMode` | `ambe` | `ambe` |
| `AmbeGainDb` | `0` | `0` |
| `DmrReencodeGain` | `0` | — |

DMR radios expect a lower level than the AGC target (`AGCTarget`, -16 dBFS). `OutputGainDMR = -18` brings every path to DMR/YSF down to it: the DVSI encoder for D-Star, M17, P25 and USRP sources, and b2 or the re-encode for DMR sources. `AmbeGainDb` and `DmrReencodeGain` only move DMR → DMR/YSF against the other paths and normally stay 0. (tcd 0.2.6 put the -18 into `AmbeGainDb`/`DmrReencodeGain`, which left D-Star → DMR 18 dB too loud.)

With the PCM gain the encoder gets the other codecs' audio at about -34 dBFS, far below the level it is made for. `OutputGainDMRBitstream` (experimental) moves part of it behind the encoder: that many dB go through b2 in the frame afterwards, level independent like `DmrGainMode = ambe`, the rest stays a PCM gain before the encoder. All of it in b2 (0.6.0 had only that, as a switch) sounded a little better over an SvxLink node to DMR (2026-10-15) but with artifacts here and there: b2 moves quiet frames in coarse steps of 3-16 dB, so a large shift cannot be met on quiet syllables and the error is made up in the next frames. Since 0.7.0 it is a split, e.g. -9 gives the encoder -25 instead of -34 dBFS and b2 only half the shift. 0 (the default) is the plain PCM gain; it can be moved in the monitor during a QSO. Even -6 crackled: between two coarse b2 steps the tracker made up the rounding error in the next frame, so the gain of quiet frames alternated by 3-6 dB every 20 ms. Since 0.7.1 the gain of this path moves by at most 1.2 dB per frame; in a simulation with speech-like b2 sequences at -4 dB no jump above 3 dB is left (68 before) and loud frames still meet the target. Larger shifts stay limited by the format itself, the coarse bottom of the b2 table: keep the split small: with 0.7.1, -4 and -6 sounded the same and clean over SvxLink to DMR (2026-10-15), URF363 runs -6. DMR → DMR keeps the unlimited tracker, it was clean in tests.

How `ambe` works: the decoder computes γ = Δγ[b2] + 0.5·γ<sub>prev</sub>, and a frame's mean log2 amplitude is γ, so one unit of γ is 6 dB. The Δγ table is coarse for low b2 and fine for high b2, so moving b2 by a fixed number of steps (tcd up to 0.2.4) changed quiet audio by 3–16 dB per step and loud audio by about 1.2 dB. tcd now tracks γ for the original and the modified stream and chooses, frame by frame, the b2 that comes closest to the wanted gain; the rounding error is made up in the next frame. Measured with the MD380 decoder on input at -40, -30 and -16 dBFS, gains of -12 to +6 dB arrive within 0.8 dB. The resolution per frame is coarse, so very quiet sources cannot be raised beyond what b2 = 31 allows.

### AGC

| Key | Template | Code default | Meaning |
|---|---|---|---|
| `AGC` | `true` | `false` | automatic gain control on/off |
| `AGCTarget` | `-16` | `-16` | target level in dBFS |
| `AGCAttack` | `50` | `50` | ms to bring loud audio down |
| `AGCRelease` | `500` | `500` | ms to bring quiet audio up |
| `AGCMaxGainDown` | `24` | `24` | most attenuation in dB |

Gate and gain limit come in two profiles: **AGC digital** for D-Star, DMR/YSF, M17 and P25/NXDN input, **AGC analog** for SVX and USRP input.

| Key | Template | Code default | Meaning |
|---|---|---|---|
| `AGCDigitalNoiseGate` / `AGCAnalogNoiseGate` | `-55` / `-40` | `-55` / `-40` | gate threshold in dBFS: the gate closes 60 ms after the level fell below it and fades the gain to the gate floor; within 6 dB above it the gain is not raised. Set it above the noise floor of the sources |
| `AGCDigitalMaxGainUp` / `AGCAnalogMaxGainUp` | `30` / `12` | `30` / `12` | most amplification in dB |
| `AGCDigitalGateFloor` / `AGCAnalogGateFloor` | `0` / `0` | `0` / `0` | gain while the gate is closed, in dB (0 = unity, at most) |
| `AGCAnalogNoiseSuppress` | `-10` | `-10` | SVX and USRP only: most attenuation of the noise by the speexdsp preprocessor before the AGC, in dB (-40 to 0, 0 = off) |

**Analog sources (SVX, USRP)** carry FM audio, whose pauses are not silent but the receiver's noise floor, often -45 to -35 dBFS. With the settings for digital sources the gate never closes on it, and the AGC raised the noise between words by up to 30 dB: on a good FM signal (noise at -45 dBFS, speech at -28 dBFS) the speech-to-noise distance fell from 17 to 11 dB. SVX and USRP therefore go through AGC analog with its own gate, gain limit and gate floor; target, attack, release and `AGCMaxGainDown` are shared. With the defaults the distance rises to 25 dB in the same case, and noisier signals are no longer made worse (simulation through `AGC.h`). 20 minutes of FM-Funknetz traffic recorded at an SvxLink node confirm it: on RF the distance fell from 34 to 19 dB with AGC digital and stays at 32 dB with AGC analog, for app and RoIP clients at 36 dB (19.7 before). The gate works on the level after urfd's `RxGain`, so keep that near 0 and let the AGC set the level; a strongly negative `RxGain` puts quiet FM speech below the gate. A squelch tail is louder than any gate: `SQL_TAIL_ELIM` in the SvxLink node's receiver removes it at the source.

**Noise suppression** (`AGCAnalogNoiseSuppress`): the gate only works between words, the FM noise under the speech stays. The vocoders model speech, AMBE above all, and turn that noise into a warbling, robotic sound on DMR. So SVX and USRP input first goes through the speexdsp preprocessor (the noise suppression of many VoIP phones), at urfd's 160 samples / 8 kHz, without added delay. It keeps one noise estimate per module, so the next transmission of the same node starts clean, and follows a different node within a second or two. Its own AGC, VAD and dereverb are off, the level stays the AGC's job. Live in the monitor, 0 switches it off. The defaults come from QSOs through an SvxLink node to DMR (2026-10-15): at -15 dB, with the gate floor at -12 dB on top, the pauses went dead silent, the comfort noise was missing and quiet syllable onsets and endings sounded cut off, since speexdsp takes them for noise; -10 dB with the gate floor at 0 keeps a quiet, natural noise floor and whole syllables, and the robotic sound stays away. Stronger values remove more noise but cut syllables and leave "musical" artifacts.

### Monitoring and logging

| Key | Template | Meaning |
|---|---|---|
| `Monitor` | `true` | web dashboard and stats socket on/off |
| `MonitorBindAddress` | `127.0.0.1` | address of web dashboard and stats socket; `0.0.0.0` opens them to the network (no password, the page can change and save levels, so only on a trusted LAN) |
| `MonitorHttpPort` | `8080` | web dashboard |
| `MonitorStatsPort` | `8081` | stats socket (JSON, for `tcdmon`) |
| `StatsLog` | `false` | per-stream CSV files for AGC tuning |
| `StatsLogDir` | `/tmp/tcd-stats` | where they go |
| `StatsLogRetain` | `24` | hours before they are deleted |

## Audio levels

Every conversion goes through PCM:

```
D-Star/DMR (DVSI hardware):
  AMBE --[DVSI GainIn]--> PCM --[AGC]--> PCM --[OutputGain]--> [DVSI GainOut] --> target AMBE

M17/P25 (software vocoder):
  codec --[decode]--> PCM --[AGC]--> PCM --[OutputGain]--> target codec

USRP/SVX (PCM):
  PCM --[noise suppression]--> PCM --[analog AGC]--> PCM --[OutputGain]--> target codec   (RxGain/TxGain per connection in urfd.ini)

DMR -> DMR/YSF with DmrGainMode = reencode:
  AMBE2+ --[decode]--> PCM --[AGC]--> PCM --[OutputGainDMR]--> [DmrReencodeGain] --> MD380 --> AMBE2+

DMR -> DMR/YSF with DmrGainMode = ambe:
  AMBE2+ --[decode]--> PCM --[AGC] (gain only)
  AMBE2+ --[b2: AGC gain + AmbeGainDb + OutputGainDMR]--> AMBE2+
```

**With the AGC**, the static gains can usually all stay at 0, except `OutputGainDMR = -18` for the level DMR radios expect: it evens out different microphones, radios and codecs per transmission. Without it, every route needs its own static gain.

Where the AGC acts: it always measures the decoded PCM of the incoming stream. Between different codecs it is applied to that PCM before encoding. DMR → DMR/YSF gets it in the AMBE2+ bitstream through b2 (`DmrGainMode = ambe`) or by re-encoding (`reencode`). D-Star → D-Star, M17 → M17 and P25 → P25 pass through unchanged.

The AGC works per stream (per transmission):
- RMS over a sliding window of 3 frames (60 ms), following syllables rather than single sounds
- a slow average (about 2 s) of the gain each speaker needs; in pauses the gain drifts towards it, so the first syllable after a pause starts at the right level
- faster release for the first 5 frames after the gate opens
- a noise gate at `AGCDigitalNoiseGate` / `AGCAnalogNoiseGate`: it closes 60 ms after the level fell below it and then fades the gain by 6 dB per frame to at most the gate floor (`AGCDigitalGateFloor` / `AGCAnalogGateFloor`), so word endings stay and noise is not amplified; within 6 dB above it the gain is not raised
- the gain ramps sample by sample across each frame instead of stepping every 20 ms
- a peak limiter just below full scale that cuts the applied gain at once, so it never clips

The gain limits are asymmetric on purpose: attenuating is safe, amplifying also lifts noise. Typical DMR input sits around -35 dBFS, so about +20 dB are needed to reach a -16 dBFS target.

## Monitoring

### Web dashboard

`http://localhost:8080` on the tcd machine, from elsewhere through `ssh -L 8080:localhost:8080 <tcd-host>`, or `http://<tcd-host>:8080` with `MonitorBindAddress = 0.0.0.0`:

- in the look of the urfd dashboard (since 0.4.0): an amber LCD per module with the input codec, the targets and the AGC state, light and dark theme; the Saira font is built into the page, so it loads nothing from outside
- LCD bar graphs before and after the AGC, with peak hold and the AGC target marked
- signal path of the active stream with the level at each stage
- AGC and gain controls that apply immediately, and a button to save them to `tcd.ini`
- DVSI devices: serial, type, role, channels in use and their modules, buffer depth
- MD380 status and counters, reflector connection and packet counters

### tcdmon

Terminal monitor, handy over SSH:

```bash
tcdmon                      # this machine, port 8081
tcdmon 192.0.2.20 8081      # another machine
```

Shows level bars, signal flow, AGC state, the DVSI channels with their modules, the MD380 counters and the output gains. `q` quits.

### Stats CSV and agc-analyze

With `StatsLog = true`, tcd writes one CSV file per stream to `StatsLogDir` (e.g. `F_20260402_003512_43521.csv`, columns `ms,module,codec,rms_in,peak_in,rms_out,peak_out,agc_gain,gate`). `agc-analyze` evaluates them:

```bash
agc-analyze               # details, default directory /tmp/tcd-stats
agc-analyze -s            # one line per stream
agc-analyze -s -c dmr     # by codec: dstar, dmr, p25, usrp, svx
agc-analyze -s -m F       # by module
agc-analyze file.csv      # one file
```

It shows duration, speech and gated frames, input and output level, the AGC gain range and clipping per stream.

## Operation and troubleshooting

```bash
sudo systemctl status tcd
sudo systemctl restart tcd
journalctl -u tcd -f
```

| Symptom | Check |
|---|---|
| `Could not find an AMBE device!` | `tcd --list-devices`; with `DeviceSerial` set, it must match exactly (see [above](#selecting-the-ambe-devices)) |
| `No USB serial devices found` | no device is plugged in, or the `ftdi_sio` driver is missing (`lsmod \| grep ftdi_sio`) |
| `USB latency timer is 16 ms` warning | tcd could not set the latency timer; it has to run as root (the systemd unit does) |
| `Lost the AMBE device ..., shutting down` | the device was unplugged or reset; systemd restarts tcd after 5 s and it finds the device again |
| the reflector shows no transcoder | `ServerAddress` and `Port` in `tcd.ini`, the same `Modules` on both sides, and port 10100/tcp reachable from the tcd machine |

## Changes from upstream

### Hardware and devices
- **Serial access without libftd2xx**: through the kernel's `ftdi_sio` driver and `/dev/serial/by-id` (POSIX backend by Jason Kendall), RTS/CTS flow control, USB latency timer at 1 ms; devices left unbound by an older tcd are bound back; a lost device restarts tcd through systemd
- **Device selection**: `--list-devices`, `DeviceSerial` with exact matching
- **Mixed mode DV3000 + DV3003**: two modules at once with per-channel codec setup
- **DV3003 D-Star**: companding off, serial flush, per-channel encoding
- **SVX codec path**: its own `ECodecType::svx` for SVX audio

### Audio
- **Output gains per codec**: after the AGC, for D-Star, M17/Codec2, DMR/YSF, P25/NXDN and USRP, applied on copies (the shared buffer is never changed)
- **AMBE2+ bitstream gain**: `AmbeGainDb` adjusts the b2 parameter in the AMBE2+ frame, with Golay(24,12) FEC recomputed for the A partition and the B partition unscrambled and scrambled again with the new A data (0.2.3; before, the decoder lost the B partition); since 0.2.5 driven by the AGC and computed in the gamma domain, so the gain no longer depends on the level, also with a single device and the MD380
- **AGC v3**: sliding RMS window, per-speaker tracking, gate with decay, fast release after the gate, asymmetric limits, noise gate, peak limiter that cuts the applied gain at once (0.2.3; before, loud onsets after quiet speech clipped), live changes from the dashboard; thread safe since 0.2.3, as every codec's decode thread uses it. Since 0.2.4 the gate holds 60 ms and fades out instead of switching (word endings are no longer chopped), noise just above it no longer pumps the gain up, and the gain ramps across each frame instead of stepping every 20 ms
- **AGC digital and AGC analog** (0.3.0): SVX and USRP input have their own gate, gain limit and gain while the gate is closed (`AGCAnalogNoiseGate`, `AGCAnalogMaxGainUp`, `AGCAnalogGateFloor`), so the FM noise between words is lowered instead of raised by up to 30 dB; the other inputs use the same three keys as `AGCDigital…` (0.3.1 renamed `AGCNoiseGate` and `AGCMaxGainUp` to `AGCDigitalNoiseGate` and `AGCDigitalMaxGainUp`, added `AGCDigitalGateFloor`, and `agc-analyze` names the key of the stream's profile); since 0.3.2 the SVX and USRP gains per connection are only in `urfd.ini` (`UsrpGainIn`/`UsrpGainOut` are gone); without the monitor tcd keeps the ini settings (before, it fell back to the built-in defaults after a second)
- **No crackling with OutputGainDMRBitstream** (0.7.1): the b2 gain of encoded audio moves by at most 1.2 dB per frame, so quiet frames no longer alternate between two coarse b2 steps
- **Version in the monitor** (0.7.0): the web page shows the running tcd version in its top bar, like the urfd dashboard, and tcdmon in its header; the stats JSON carries it as `version`, all from one constant in `Version.h`; `OutputGainDMRBitstream` became a split in dB
- **DMR level in the bitstream for other codecs** (0.6.0, experimental): `OutputGainDMRBitstream` applies `OutputGainDMR` to D-Star, M17, P25, USRP and SVX audio through b2 after the encoder instead of before it; 0.6.0 had a switch for all of it, which left artifacts on quiet syllables, since 0.7.0 it is the number of dB in b2 (-24 to 0, default 0 = off), live in the monitor
- **Noise suppression for analog input** (0.5.0): SVX and USRP go through the speexdsp preprocessor before the AGC analog (`AGCAnalogNoiseSuppress`, default -15 dB, live in the monitor; 0.5.1 lowered it to -10 dB and `AGCAnalogGateFloor` from -12 to 0, the noise suppression lowers the pauses), so the FM noise under the speech no longer turns into robotic AMBE artifacts on DMR; new build dependency `libspeexdsp-dev`
- **DMR re-encode** (`md380=true`): `DmrGainMode = reencode` with AGC and `DmrReencodeGain` on DMR → DMR/YSF
- **MD380 stream isolation**: encoder state saved per stream, MD380 calls serialized; since 0.2.7 a new stream starts from the state right after `md380_init()` instead of the previous stream's; since 0.2.9 for every MD380 use, also the primary DMR codec of a single device setup
- **Mixed mode output gains** (0.2.3): the DV3003's D-Star channel uses `OutputGainDStar`, not `OutputGainDMR`; M17 at 1600 bit/s no longer runs the second half of each packet through the AGC twice; since 0.2.11 a module missing from the module list is not sent to a DMR channel

### Monitoring and configuration
- **Web dashboard**: embedded HTTP/WebSocket server, in the urfd dashboard's look since 0.4.0, with signal flow, LCD level meters, gain and AGC controls, device and MD380 status, save to INI
- **tcdmon**: terminal monitor
- **Stats CSV** with automatic cleanup and the `agc-analyze` tool; since 0.2.9 it reports gate flicker as openings under 100 ms (noise crossing the threshold, raise the profile's noise gate), as the gate holds before it closes since 0.2.4
- **Renamed keys**: `DmrGainIn/Out` (was `DmrYsfGainIn/Out`), `UsrpGainIn/Out` (was `UsrpRxGain/TxGain`, gone since 0.3.2), gains extended to ±40 dB

### Reliability
- **Monitor on loopback by default**: `MonitorBindAddress` (default `127.0.0.1`) for the web dashboard and stats socket, which have no password
- **Optional MD380**: without it, two AMBE devices are needed and re-encode is unavailable; since 0.2.5 `ambe` covers DMR → DMR/YSF without it
- **Performance**: blocking reads with a timeout instead of busy polling; built with `-O2` since 0.2.12, a transcoded stream (Codec2, IMBE and AGC in software) takes about 7 % of one core on a Raspberry Pi 4 and 3 % on a Pi 5, so a Pi 4 is plenty for two modules without MD380
- **Thread safety**: lock around the IMBE vocoder, SIGPIPE ignored for clean reconnects
- **Error handling**: retries towards the reflector with backoff, dropping on queue overflow, timeout recovery while reading
- **Reconnect backoff**: 0.5 s growing to 5 s while the reflector is unreachable
- **No Nagle delay**: `TCP_NODELAY` on the reflector connection; a frame returns after about 23 ms instead of 60 ms
- **Build**: part of the urfd repository, `build.sh` builds as the checkout owner

## Copyright

- Copyright (c) 2021-2023 Thomas A. Early N7TAE, Doug McLain AD8DP
- POSIX serial backend: Jason Kendall (2026)
- Copyright (c) 2026 Jens-Christian Merg DL4JC (urfd-ng improvements)
