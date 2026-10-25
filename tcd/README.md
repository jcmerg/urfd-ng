# tcd — Hybrid Digital Voice Transcoder

**English** | [Deutsch](README.de.md)

**Version 0.10.6** — the transcoder of [urfd-ng](../README.md), in the `tcd/` folder of the same repository. Based on [n7tae/tcd](https://github.com/n7tae/tcd).

tcd converts voice between the digital modes on a URF reflector, so a D-Star user hears a DMR user and the other way round. It uses DVSI AMBE hardware for D-Star (AMBE) and DMR/YSF (AMBE2+), and software vocoders for M17 (Codec2) and P25/NXDN (IMBE). [Codec plugins](#codec-plugins) can add D-Star and DMR/YSF in software. The reflector hands the audio of its transcoded modules to tcd over TCP and gets every other mode back.

## Contents

- [Install](#install)
- [Hardware](#hardware)
- [Codec plugins](#codec-plugins)
- [tcd in Docker](#tcd-in-docker)
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
sudo bash tcd/build.sh
```

`build.sh` installs the build tools, `libspeexdsp-dev` and `libwebsockets-dev`, builds tcd as the owner of the checkout and installs `tcd`, `tcdmon`, `agc-analyze` and the systemd unit. A running tcd is restarted. The first install puts the template at `/usr/local/etc/tcd.ini`; edit it (see [Configuration](#configuration-tcdini)) and start with `sudo systemctl start tcd`.

To update: `git pull` in the checkout and run `build.sh` again, or `sudo ./setup.sh update`.

### By hand

With `libspeexdsp-dev`, `libwebsockets-dev` and `libncurses-dev` installed:

```bash
cd urfd/tcd
make -j$(nproc)
sudo make install
```

Codec2 (`codec2/`) and the P25 IMBE vocoder from OP25 (`imbe/`) are part of the source, so no other library and no other repository is needed.

tcd builds against the urfd sources it shares with the reflector (`TCPacketDef.h`, `TCSocket.*`, ...) from `../reflector`, so both sides always speak the same protocol. `config/tcd.ini` is the commented template, `config/tcd.service` the systemd unit.

### Requirements

- Linux with systemd: Debian, Ubuntu or Raspberry Pi OS; x86_64, arm64 or armhf
- DVSI AMBE USB devices (see [Hardware](#hardware)); they use the kernel's `ftdi_sio` driver, no extra driver needed
- `libspeexdsp-dev` for the noise suppression of analog input and `libwebsockets-dev` for the web dashboard; `build.sh` installs them
- Low latency to the AMBE devices: with sticks tcd runs natively, not in Docker (software only, it can, see [tcd in Docker](#tcd-in-docker)). It can run on the reflector's machine or on another one, e.g. a Raspberry Pi next to the radio gear.

## Hardware

Each transcoded module needs a D-Star channel and a DMR/YSF channel. A DV3000 (ThumbDV, DVstick-30, USB-3000) has one channel, a DV3003 (DVstick-33, USB-3003) three. A codec without a device can come from a [codec plugin](#codec-plugins).

| Hardware | Codec plugin | Modules | Notes |
|---|---|---|---|
| 1× DV3000 or 1× DV3003 | one for DMR (or D-Star) | 1 | D-Star on the device, DMR/YSF in software |
| DV3000 + DV3003 | — | 2 | mixed mode, recommended |
| 2× DV3000 | — | 1 | one for D-Star, one for DMR/YSF |
| 2× DV3003 | — | up to 3 | one for D-Star, one for DMR/YSF |
| none | one for D-Star and DMR | any | everything in software |

A single device needs a codec plugin for the other codec; tcd refuses to start without one. With two devices no plugin is needed: since 0.2.5 `DmrGainMode = ambe` gives DMR → DMR/YSF the same AGC without re-encoding (see [`DmrGainMode`](#dmrysf--dmrysf-level-dmrgainmode)); a DMR plugin only adds `reencode` as an alternative. tcd detects the devices and picks the mode itself.

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

### Single device with a codec plugin

One module, D-Star on the DV3000 or DV3003 and DMR/YSF through a codec plugin. In this mode, DMR/YSF → DMR/YSF audio is **not re-encoded** after the AGC. Use the bitstream gain (`DmrGainMode = ambe`, which follows the AGC; in this mode since 0.2.5) for that path; all paths between different modes still get the full AGC. Re-encode on DMR/YSF → DMR/YSF needs a second AMBE device.

### DV3003 notes

- **Companding off**: every DV3003 channel gets companding explicitly disabled. The DV3003 defaults to companding on (CP_ENABLE pull-up), the DV3000 to off; without this, the DMR/YSF channels got µ-law PCM read as linear, which distorts the audio.
- **Serial flush**: before the soft reset, 350 zero bytes clear any stale serial state.
- **Per-channel codec**: one DV3003 can run D-Star and DMR/YSF channels at the same time (mixed mode).

## Codec plugins

A codec plugin is a shared object that brings D-Star (AMBE) and/or DMR/YSF (AMBE2+) in software. tcd loads it at start:

```ini
CodecPlugin = /usr/local/lib/tcd/foo.so   # as many lines as needed
CodecDStar  = dvsi                        # the vocoder new D-Star streams get: dvsi or a plugin's name
CodecDMR    = foo                         # the same for DMR/YSF
```

- **Which vocoder**: without `CodecDStar`/`CodecDMR` the devices come first and a plugin only does what no device does. A plugin's name there puts that codec in software from the start, even when a device could do it; with a single device, the device then takes the other codec.
- **More modules than channels**: the devices take the first modules of `Modules`, as many as they have channels (mixed mode 2, two DV3003 3, ...); the other modules always go to a plugin. With DV3000 + DV3003 and `Modules = FSABC`, F and S are transcoded on the devices and A, B and C in software. Without a plugin for the rest, tcd does not start.
- **Switching at run time**: the web dashboard (settings, "Vocoder") and `POST /api/codec` with `{"codec": "dstar", "use": "foo"}` choose the vocoder for the streams that start from then on: a running transmission keeps its vocoder. Everything loaded at start is offered: a device for the codec and every plugin that does it. "Save" writes the choice to `tcd.ini`.
- **Options**: lines of `tcd.ini` that tcd does not know go to the plugins, e.g. a path the plugin needs; a line nobody takes is warned about.
- **Interface**: plain C, [`CodecPlugin.h`](CodecPlugin.h): `open`/`close` a state per stream, `encode`/`decode` 160 samples of 8 kHz PCM to and from a 9 byte frame. tcd calls a plugin from one thread at a time and closes the state of a stream that has been quiet for 30 s. A plugin is built against this header only and does not have to be rebuilt with every tcd update; tcd checks `TCD_CODEC_API`.
- **Frames**: D-Star as the 9 data bytes of a DV frame, DMR/YSF as the 72 bits of an AMBE2+ frame in air order, MSB first, the formats the DVSI devices use.

No codec plugin is part of this repository: software implementations of AMBE and AMBE2+ may touch patents (AMBE2+ half rate, US 8,359,197, projected to run until 2028-05-20), so whether to build and use one is the operator's decision. A plugin for D-Star and DMR/YSF with [mbelib-neo](https://github.com/arancormonk/mbelib-neo) is [tcd-mbelib-neo-plugin](https://github.com/jcmerg/tcd-mbelib-neo-plugin); read its patent notice first.

## tcd in Docker

Without AMBE sticks, tcd can run in a container next to urfd: everything in software with a [codec plugin](#codec-plugins) for D-Star and DMR. The DVSI sticks are no case for Docker: their timing needs tcd to run natively (see [Requirements](#requirements)), so the image has no device access.

```bash
cd ~/urfd
git clone https://github.com/jcmerg/tcd-mbelib-neo-plugin.git tcd/plugins/mbelib-neo   # read its patent notice first
sudo mkdir -p /opt/urfd/tcd && sudo cp tcd/config/tcd.ini /opt/urfd/tcd/tcd.ini
sudo nano /opt/urfd/tcd/tcd.ini
cd docker && sudo docker compose --profile tcd build && sudo docker compose --profile tcd up -d
```

In `tcd.ini`: `ServerAddress = 127.0.0.1` (the container uses the host's network), `Modules` as many as wanted, `CodecPlugin = /usr/local/lib/tcd/mbelib_neo_codec.so`, no `DeviceSerial`, and a `MonitorHttpPort` that is free on the host. In `urfd.ini`: `[Transcoder] Modules = auto`, so tcd's list alone decides. The image builds every `tcd/plugins/<name>/` with a `Makefile` (git ignores that directory); `$URFD_DATA/tcd/` (default `/opt/urfd/tcd`) holds `tcd.ini`, writable for the monitor's Save. To update: `git pull`, then the same `build` and `up -d`. Without `--profile tcd`, `docker compose` keeps to urfd alone.

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
| `ServerAddress` | `127.0.0.1` | IP address or host name of the urfd reflector; a host name is resolved again on every reconnect (since 0.7.4) |
| `Port` | `10100` | the reflector's `[Transcoder] Port` |
| `Password` | — | the reflector's `[Transcoder] Password`, needed when it has one (since 0.8.0) |
| `Modules` | `FS` | transcoded modules; with the reflector's `[Transcoder] Modules = auto` (urfd 4.25.0) any of its modules, this list alone decides; else the same letters as there |
| `DeviceSerial` | — | AMBE device to use, one line each (see [above](#selecting-the-ambe-devices)) |

### Gains

All gains are in dB, -40 to +40.

| Key | Template | Applied to |
|---|---|---|
| `OutputGainDStar` | `0` | D-Star output, after the AGC |
| `OutputGainDMR` | `-18` | DMR/YSF output, after the AGC; every path to DMR/YSF, including DMR → DMR/YSF |
| `OutputGainDMRBitstream` | `-6` | -24 to 0 dB (0 = all of it before the encoder): for D-Star, M17, P25, USRP and SVX sources this much of `OutputGainDMR` is applied through b2 in the AMBE2+ frame after the encoder, the rest as PCM before it, so the encoder gets more level; live in the monitor |
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
| `reencode` | a DMR codec plugin and a DMR device | full decode and re-encode through the codec plugin, with AGC and `DmrReencodeGain`; an alternative to `ambe`, same level with the same value. Falls back to `ambe` without a DMR codec plugin. |

| Key | Template | Code default |
|---|---|---|
| `DmrGainMode` | `ambe` | `ambe` |
| `AmbeGainDb` | `0` | `0` |
| `DmrReencodeGain` | `0` | — |

DMR radios expect a lower level than the AGC target (`AGCTarget`, -16 dBFS). `OutputGainDMR = -18` brings every path to DMR/YSF down to it: the DVSI encoder for D-Star, M17, P25 and USRP sources, and b2 or the re-encode for DMR sources. `AmbeGainDb` and `DmrReencodeGain` only move DMR → DMR/YSF against the other paths and normally stay 0. (tcd 0.2.6 put the -18 into `AmbeGainDb`/`DmrReencodeGain`, which left D-Star → DMR 18 dB too loud.)

With the PCM gain the encoder gets the other codecs' audio at about -34 dBFS, far below the level it is made for. `OutputGainDMRBitstream` moves part of it behind the encoder: that many dB go through b2 in the frame afterwards, level independent like `DmrGainMode = ambe`, the rest stays a PCM gain before the encoder. All of it in b2 (0.6.0 had only that, as a switch) sounded a little better over an SvxLink node to DMR (2026-10-15) but with artifacts here and there: b2 moves quiet frames in coarse steps of 3-16 dB, so a large shift cannot be met on quiet syllables and the error is made up in the next frames. Since 0.7.0 it is a split, e.g. -9 gives the encoder -25 instead of -34 dBFS and b2 only half the shift. 0 is the plain PCM gain; it can be moved in the monitor during a QSO. Even -6 crackled: between two coarse b2 steps the tracker made up the rounding error in the next frame, so the gain of quiet frames alternated by 3-6 dB every 20 ms. Since 0.7.1 the gain of this path moves by at most 1.2 dB per frame; in a simulation with speech-like b2 sequences at -4 dB no jump above 3 dB is left (68 before) and loud frames still meet the target. Larger shifts stay limited by the format itself, the coarse bottom of the b2 table: keep the split small: with 0.7.1, -4 and -6 sounded the same and clean over SvxLink to DMR (2026-10-15), URF363 runs -6. DMR → DMR keeps the unlimited tracker, it was clean in tests. Since 0.7.1 the option is no longer experimental, since 0.7.2 -6 is the default (before: 0).

A b2 choice with look-ahead was simulated and left out (2026-10-15): a beam search over the next frames with a quadratic cost on gain changes removes the jumps above 3 dB at -6 (61 → 0) only with 6 frames of look-ahead, about 100 ms, and gets to about -8 dB with as many jumps as the plain limit at -6; without look-ahead it misses the target by about 3 dB. That is 2 dB more level for the encoder against 60-100 ms more delay for every frame of the packet (tcd sends a packet once all codecs are done) and more code in the packet path, while -4 and -6 already sounded the same and clean: most counted jumps sit at syllable boundaries and are masked. The b2 table itself can't be improved, it is part of AMBE2+ and fixed in every decoder.

How `ambe` works: the decoder computes γ = Δγ[b2] + 0.5·γ<sub>prev</sub>, and a frame's mean log2 amplitude is γ, so one unit of γ is 6 dB. The Δγ table is coarse for low b2 and fine for high b2, so moving b2 by a fixed number of steps (tcd up to 0.2.4) changed quiet audio by 3–16 dB per step and loud audio by about 1.2 dB. tcd now tracks γ for the original and the modified stream and chooses, frame by frame, the b2 that comes closest to the wanted gain; the rounding error is made up in the next frame. Measured with an AMBE2+ software decoder on input at -40, -30 and -16 dBFS, gains of -12 to +6 dB arrive within 0.8 dB. The resolution per frame is coarse, so very quiet sources cannot be raised beyond what b2 = 31 allows.

### AGC

| Key | Template | Code default | Meaning |
|---|---|---|---|
| `AGC` | `true` | `false` | automatic gain control on/off |
| `AGCTarget` | `-16` | `-16` | target level in dBFS |
| `AGCAttack` | `50` | `50` | time constant in ms for rises of the speech level up to 10 dB (larger jumps are caught within 2–3 frames) |
| `AGCRelease` | `500` | `500` | time constant in ms for falls of the speech level, applied only on clearly voiced speech well above the noise floor |
| `AGCMaxGainDown` | `24` | `24` | most attenuation in dB |

Gate and gain limit come in two profiles: **AGC digital** for D-Star, DMR/YSF, M17 and P25/NXDN input, **AGC analog** for SVX and USRP input.

| Key | Template | Code default | Meaning |
|---|---|---|---|
| `AGCDigitalNoiseGate` / `AGCAnalogNoiseGate` | `-55` / `-40` | `-55` / `-40` | gate threshold in dBFS: frames below it, or less than 9 dB above the noise floor tcd measures per stream, count as no speech; 240 ms later the gate closes and fades the gain to the gate floor. A safety net below the measured noise floor |
| `AGCDigitalMaxGainUp` / `AGCAnalogMaxGainUp` | `30` / `12` | `30` / `12` | most amplification in dB |
| `AGCDigitalGateFloor` / `AGCAnalogGateFloor` | `0` / `0` | `0` / `0` | gain while the gate is closed, in dB (0 = unity, at most) |
| `AGCAnalogNoiseSuppress` | `-10` | `-10` | SVX and USRP only: most attenuation of the noise by the speexdsp preprocessor before the AGC, in dB (-40 to 0, 0 = off) |

**Analog sources (SVX, USRP)** carry FM audio, whose pauses are not silent but the receiver's noise floor, often -45 to -35 dBFS. With the settings for digital sources the gate never closes on it, and the AGC raised the noise between words by up to 30 dB: on a good FM signal (noise at -45 dBFS, speech at -28 dBFS) the speech-to-noise distance fell from 17 to 11 dB. SVX and USRP therefore go through AGC analog with its own gate, gain limit and gate floor; target, attack, release and `AGCMaxGainDown` are shared. With the defaults the distance rises to 25 dB in the same case, and noisier signals are no longer made worse (simulation through `AGC.h`). 20 minutes of FM-Funknetz traffic recorded at an SvxLink node confirm it: on RF the distance fell from 34 to 19 dB with AGC digital and stays at 32 dB with AGC analog, for app and RoIP clients at 36 dB (19.7 before). The gate works on the level after urfd's `RxGain`, so keep that near 0 and let the AGC set the level; a strongly negative `RxGain` puts quiet FM speech below the gate. A squelch tail is louder than any gate: `SQL_TAIL_ELIM` in the SvxLink node's receiver removes it at the source.

**Noise suppression** (`AGCAnalogNoiseSuppress`): the gate only works between words, the FM noise under the speech stays. The vocoders model speech, AMBE above all, and turn that noise into a warbling, robotic sound on DMR. So SVX and USRP input first goes through the speexdsp preprocessor (the noise suppression of many VoIP phones), at urfd's 160 samples / 8 kHz, without added delay. It keeps one noise estimate per module, so the next transmission of the same node starts clean, and follows a different node within a second or two. Its own AGC, VAD and dereverb are off, the level stays the AGC's job. Live in the monitor, 0 switches it off. The defaults come from QSOs through an SvxLink node to DMR (2026-10-15): at -15 dB, with the gate floor at -12 dB on top, the pauses went dead silent, the comfort noise was missing and quiet syllable onsets and endings sounded cut off, since speexdsp takes them for noise; -10 dB with the gate floor at 0 keeps a quiet, natural noise floor and whole syllables, and the robotic sound stays away. Stronger values remove more noise but cut syllables and leave "musical" artifacts.

Why not the noise suppressor in the AMBE chip: the DVSI devices have one (on by default, `NS_ENABLE` in `ECMODE`), but tcd turns it off when it sets up the channels (`ECMODE = 0`). It sits in the encoder channel and would act on everything that channel encodes, so also on D-Star, M17 and P25 audio that is clean already, and it can't tell an FM source from the others; it is also set per channel at start, not per stream. speexdsp works where the analog input comes in, only on SVX and USRP, with its own setting, and the same for every target codec (DMR, D-Star, M17, P25).

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
  AMBE2+ --[decode]--> PCM --[AGC]--> PCM --[OutputGainDMR]--> [DmrReencodeGain] --> codec plugin --> AMBE2+

DMR -> DMR/YSF with DmrGainMode = ambe:
  AMBE2+ --[decode]--> PCM --[AGC] (gain only)
  AMBE2+ --[b2: AGC gain + AmbeGainDb + OutputGainDMR]--> AMBE2+
```

**With the AGC**, the static gains can usually all stay at 0, except `OutputGainDMR = -18` for the level DMR radios expect: it evens out different microphones, radios and codecs per transmission. Without it, every route needs its own static gain.

Where the AGC acts: it always measures the decoded PCM of the incoming stream. Between different codecs it is applied to that PCM before encoding. DMR → DMR/YSF gets it in the AMBE2+ bitstream through b2 (`DmrGainMode = ambe`) or by re-encoding (`reencode`). D-Star → D-Star, M17 → M17 and P25 → P25 pass through unchanged.

The AGC works per stream (per transmission):
- a 75 Hz high-pass (3rd order, 18 dB/octave) on the audio, so DC and hum neither drive the level nor reach the encoder
- the speech level in dB from the louder 10 ms half of each frame, over the whole band: it follows the upper envelope of the speech, close to the vowels
- noise floor, gate and voice detection run on a copy high-passed at 300 Hz, the band the radios transmit; a CTCSS tone or hum below it does not count as noise or voice
- a rise of more than 10 dB is caught by half the gap per frame, smaller rises with `AGCAttack`; the level only falls (the gain only rises) on clearly voiced frames (autocorrelation in the pitch range of 80–400 Hz) well above the noise floor, and by at most 10 dB per `AGCRelease`. Breath, hiss, fricatives and pauses hold the gain at the speaker's value, so it does not pump
- the first voiced frames of a stream set its level fast, so the first words of a quiet speaker are not too quiet
- a noise floor per stream: it follows the level on frames without speech, and the minimum of the last 2.4 s lifts it when the noise rose. The boost is scaled by (speech level − noise floor) / 20 dB, up to 1: clean sources keep the full gain, weak, noisy signals are not lifted into their noise
- a noise gate: frames below `AGCDigitalNoiseGate` / `AGCAnalogNoiseGate` or less than 9 dB above the noise floor are no speech; 240 ms later, which bridges the gaps between words, the gain fades by 6 dB per frame to at most the gate floor (`AGCDigitalGateFloor` / `AGCAnalogGateFloor`), so word endings stay and noise is not amplified
- the gain ramps sample by sample across each frame instead of stepping every 20 ms
- a peak limiter just below full scale that cuts the applied gain at once, so it never clips

The gain limits are asymmetric on purpose: attenuating is safe, amplifying also lifts noise. Typical DMR input sits around -35 dBFS, so about +20 dB are needed to reach a -16 dBFS target.

## Monitoring

### Web dashboard

`http://localhost:8080` on the tcd machine, from elsewhere through `ssh -L 8080:localhost:8080 <tcd-host>`, or `http://<tcd-host>:8080` with `MonitorBindAddress = 0.0.0.0`:

- in the look of the urfd dashboard (since 0.4.0): an amber LCD per module with the input codec, the targets and the AGC state, light and dark theme; the Saira font is built into the page, so it loads nothing from outside
- LCD bar graphs before and after the AGC, with peak hold and the AGC target marked
- for many modules a compact view (from five modules on, or chosen with "Compact"): a key per module with its codec and level, full cards only for the modules on the air and for the ones clicked to keep open; tcd keeps the view for every browser (`MonitorView`, written by Save), the pinned modules stay in the browser
- signal path of the active stream with the level at each stage
- AGC and gain controls that apply immediately, and a button to save them to `tcd.ini`
- DVSI devices: serial, type, role, channels in use and their modules, buffer depth
- codec plugins: their codec, counters and streams, and which vocoder new D-Star and DMR streams get (switchable, see [Codec plugins](#codec-plugins))
- reflector connection and packet counters

### tcdmon

Terminal monitor, handy over SSH:

```bash
tcdmon                      # this machine, port 8081
tcdmon 192.0.2.20 8081      # another machine
```

Shows level bars, signal flow, AGC state, the DVSI channels with their modules, the codec plugins and the output gains. `q` quits.

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
- **AMBE2+ bitstream gain**: `AmbeGainDb` adjusts the b2 parameter in the AMBE2+ frame, with Golay(24,12) FEC recomputed for the A partition and the B partition unscrambled and scrambled again with the new A data (0.2.3; before, the decoder lost the B partition); since 0.2.5 driven by the AGC and computed in the gamma domain, so the gain no longer depends on the level, also with a single device and a codec plugin
- **AGC v3**: sliding RMS window, per-speaker tracking, gate with decay, fast release after the gate, asymmetric limits, noise gate, peak limiter that cuts the applied gain at once (0.2.3; before, loud onsets after quiet speech clipped), live changes from the dashboard; thread safe since 0.2.3, as every codec's decode thread uses it. Since 0.2.4 the gate holds 60 ms and fades out instead of switching (word endings are no longer chopped), noise just above it no longer pumps the gain up, and the gain ramps across each frame instead of stepping every 20 ms
- **AGC digital and AGC analog** (0.3.0): SVX and USRP input have their own gate, gain limit and gain while the gate is closed (`AGCAnalogNoiseGate`, `AGCAnalogMaxGainUp`, `AGCAnalogGateFloor`), so the FM noise between words is lowered instead of raised by up to 30 dB; the other inputs use the same three keys as `AGCDigital…` (0.3.1 renamed `AGCNoiseGate` and `AGCMaxGainUp` to `AGCDigitalNoiseGate` and `AGCDigitalMaxGainUp`, added `AGCDigitalGateFloor`, and `agc-analyze` names the key of the stream's profile); since 0.3.2 the SVX and USRP gains per connection are only in `urfd.ini` (`UsrpGainIn`/`UsrpGainOut` are gone); without the monitor tcd keeps the ini settings (before, it fell back to the built-in defaults after a second)
- **OutputGainDMRBitstream = -6 by default** (0.7.2): clean at -4 and -6 over SvxLink to DMR, so D-Star, M17, P25, USRP and SVX audio reaches the AMBE2+ encoder 6 dB hotter by default; an ini with the key keeps its value
- **No crackling with OutputGainDMRBitstream** (0.7.1): the b2 gain of encoded audio moves by at most 1.2 dB per frame, so quiet frames no longer alternate between two coarse b2 steps
- **Version in the monitor** (0.7.0): the web page shows the running tcd version in its top bar, like the urfd dashboard, and tcdmon in its header; the stats JSON carries it as `version`, all from one constant in `Version.h`; `OutputGainDMRBitstream` became a split in dB
- **DMR level in the bitstream for other codecs** (0.6.0, experimental until 0.7.1): `OutputGainDMRBitstream` applies `OutputGainDMR` to D-Star, M17, P25, USRP and SVX audio through b2 after the encoder instead of before it; 0.6.0 had a switch for all of it, which left artifacts on quiet syllables, since 0.7.0 it is the number of dB in b2 (-24 to 0), live in the monitor; default -6 since 0.7.2 (0 before)
- **Noise suppression for analog input** (0.5.0): SVX and USRP go through the speexdsp preprocessor before the AGC analog (`AGCAnalogNoiseSuppress`, default -15 dB, live in the monitor; 0.5.1 lowered it to -10 dB and `AGCAnalogGateFloor` from -12 to 0, the noise suppression lowers the pauses), so the FM noise under the speech no longer turns into robotic AMBE artifacts on DMR; new build dependency `libspeexdsp-dev`
- **DMR re-encode** (with a DMR codec plugin): `DmrGainMode = reencode` with AGC and `DmrReencodeGain` on DMR → DMR/YSF
- **Codec plugins** (0.10.0): software vocoders for D-Star and DMR/YSF from shared objects with a plain C interface, a vocoder state per stream; replace the built-in MD380 support. The vocoder for new streams can be switched at run time between the devices and the plugins
- **Mixed mode output gains** (0.2.3): the DV3003's D-Star channel uses `OutputGainDStar`, not `OutputGainDMR`; M17 at 1600 bit/s no longer runs the second half of each packet through the AGC twice; since 0.2.11 a module missing from the module list is not sent to a DMR channel

### Monitoring and configuration
- **Web dashboard**: embedded HTTP/WebSocket server, in the urfd dashboard's look since 0.4.0, with signal flow, LCD level meters, gain and AGC controls, device and codec plugin status, vocoder choice, save to INI
- **tcdmon**: terminal monitor
- **Last stream in the monitor** (0.7.3): an idle module shows the codec and age of its last stream (seconds, then minutes and hours) instead of falling back to "No stream since tcd started" 2 s after each stream; the stats JSON carries it as `last_codec`
- **Stats CSV** with automatic cleanup and the `agc-analyze` tool; since 0.2.9 it reports gate flicker as openings under 100 ms (noise crossing the threshold, raise the profile's noise gate), as the gate holds before it closes since 0.2.4
- **Renamed keys**: `DmrGainIn/Out` (was `DmrYsfGainIn/Out`), `UsrpGainIn/Out` (was `UsrpRxGain/TxGain`, gone since 0.3.2), gains extended to ±40 dB

### Reliability
- **Monitor on loopback by default**: `MonitorBindAddress` (default `127.0.0.1`) for the web dashboard and stats socket, which have no password
- **No grey zone in the core** (0.10.0): the MD380 firmware is gone from tcd; without a codec plugin two AMBE devices are needed, and `ambe` covers DMR → DMR/YSF
- **Performance**: blocking reads with a timeout instead of busy polling; built with `-O2` since 0.2.12, a transcoded stream (Codec2, IMBE and AGC in software) takes about 7 % of one core on a Raspberry Pi 4 and 3 % on a Pi 5, so a Pi 4 is plenty for two modules
- **Thread safety**: lock around the IMBE vocoder, SIGPIPE ignored for clean reconnects
- **Error handling**: retries towards the reflector with backoff, dropping on queue overflow, timeout recovery while reading
- **Reconnect backoff**: 0.5 s growing to 5 s while the reflector is unreachable
- **Speech-driven AGC** (0.9.0): the level is tracked in dB on the upper envelope of the speech and only raised on clearly voiced frames well above a noise floor measured per stream; the boost shrinks on noisy signals, a 75 Hz high-pass cleans the audio and the analysis ignores everything below 300 Hz (CTCSS, hum). The gain no longer pumps on breath, hiss or pauses, the gate holds 240 ms across the gaps between words. Same keys and defaults in `tcd.ini`. Measured on 18 FM-Funknetz transmissions of RF stations (2026-10-23) through the analog path (noise suppression and AGC analog) against 0.8.0: the stations end up as close together (spread 1.2 dB) at about the same level (-16.4 against -15.6 dBFS), the gain moves a third as much from frame to frame (0.20 against 0.55 dB) and the speech stands 2.5 dB further above the noise in the pauses
- **Password for the reflector connection** (0.8.0, urfd 4.16.0): with `Password` in `tcd.ini` and `[Transcoder] Password` in `urfd.ini`, the reflector sends a random challenge after the module byte and tcd answers with HMAC-SHA256 over it; only then does the connection replace the module's current one. Without the password, anyone who reached port 10100 could take over a module. The link stays unencrypted. Without a password on both sides nothing changes; with it on one side only, the connection is refused with a message saying so
- **Host name as ServerAddress** (0.7.4): `ServerAddress` takes a host name as well as an IP address; before, the config check stopped tcd at anything but an IP address
- **No Nagle delay**: `TCP_NODELAY` on the reflector connection; a frame returns after about 23 ms instead of 60 ms
- **Web dashboard on libwebsockets** (0.10.0): the HTTP and WebSocket server is libwebsockets (MIT, as in urfd) instead of the bundled mongoose, whose GPL-2.0-only license does not go with tcd's GPLv3; same page, same `/api/...` and `/ws`
- **Build**: part of the urfd repository, `build.sh` builds as the checkout owner; since 0.10.0 the IMBE vocoder is part of the source (`imbe/`), no other repository is fetched

## Copyright

- Copyright (c) 2021-2023 Thomas A. Early N7TAE, Doug McLain AD8DP
- POSIX serial backend: Jason Kendall (2026)
- Copyright (c) 2026 Jens-Christian Merg DL4JC (urfd-ng improvements)
