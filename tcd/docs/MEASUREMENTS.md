# tcd measurements

**English** | [Deutsch](MEASUREMENTS.de.md)

The measurements and listening tests behind tcd's defaults, for anyone who wants to know why a setting is what it is, or to tune it. The settings themselves are explained in the [README](../README.md).

## Contents

- [DMR level](#dmr-level)
- [OutputGainDMRBitstream](#outputgaindmrbitstream)
- [b2 gain accuracy (`DmrGainMode = ambe`)](#b2-gain-accuracy-dmrgainmode--ambe)
- [AGC on analog sources](#agc-on-analog-sources)
- [Noise suppression](#noise-suppression)
- [Speech-driven AGC](#speech-driven-agc)
- [Lost answers on the DV3003 and `DeviceCompand`](#lost-answers-on-the-dv3003-and-devicecompand)
- [Direct transcoding](#direct-transcoding)
- [P25 to M17](#p25-to-m17)
- [CPU load and latency](#cpu-load-and-latency)

## DMR level

DMR radios expect a lower level than the AGC target of -16 dBFS. Speech on BrandMeister, measured on TG91: median -29 dBFS, -39 to -23 dBFS between the talkers. `OutputGainDMR = -14` brings every path to DMR/YSF to about -30 dBFS, in line with it; -18 left DMR/YSF about 4 dB below it. The offset belongs in `OutputGainDMR`, not in `AmbeGainDb`/`DmrReencodeGain`: those only act on DMR → DMR/YSF, so putting -18 there left D-Star → DMR 18 dB too loud.

## OutputGainDMRBitstream

With `OutputGainDMR = -14` as PCM gain, the AMBE2+ encoder gets the audio of the other codecs at about -34 dBFS, far below the level it is made for. `OutputGainDMRBitstream` moves part of the gain behind the encoder, into the b2 gain parameter of the frame.

- **All of it in b2** sounded a little better over an SvxLink node to DMR (2026-10-15), but with artifacts here and there: b2 moves quiet frames in coarse steps of 3-16 dB, so a large shift cannot be met on quiet syllables and the error is made up in the next frames. Hence a split in dB: e.g. -9 gives the encoder -25 instead of -34 dBFS and b2 only half the shift.
- **Crackling at -6**: between two coarse b2 steps the tracker made up the rounding error in the next frame, so the gain of quiet frames alternated by 3-6 dB every 20 ms. The gain of this path now moves by at most 1.2 dB per frame; in a simulation with speech-like b2 sequences at -4 dB no jump above 3 dB is left (68 before) and loud frames still meet the target. With that, -4 and -6 sounded the same and clean over SvxLink to DMR (2026-10-15); -6 became the default. DMR → DMR keeps the unlimited tracker, it was clean in tests.
- **b2 choice with look-ahead**, simulated and left out (2026-10-15): a beam search over the next frames with a quadratic cost on gain changes removes the jumps above 3 dB at -6 (61 → 0) only with 6 frames of look-ahead, about 100 ms, and reaches about -8 dB with as many jumps as the plain limit at -6; without look-ahead it misses the target by about 3 dB. 2 dB more level for the encoder are not worth 60-100 ms more delay on every frame (tcd sends a packet once all codecs are done), while -4 and -6 already sound clean: most counted jumps sit at syllable boundaries and are masked. The b2 table itself is part of AMBE2+ and fixed in every decoder.

## b2 gain accuracy (`DmrGainMode = ambe`)

Moving b2 by a fixed number of steps changed quiet audio by 3-16 dB per step and loud audio by about 1.2 dB, so the gain depended on the level. tcd now tracks γ for the original and the modified stream and chooses the b2 that comes closest to the wanted gain. Measured with an AMBE2+ software decoder on input at -40, -30 and -16 dBFS: gains of -12 to +6 dB arrive within 0.8 dB.

## AGC on analog sources

FM audio is not silent between words but carries the receiver's noise, often -45 to -35 dBFS, which the gate for digital sources never closes on.

- **Simulation** through `AGC.h`, a good FM signal (noise at -45 dBFS, speech at -28 dBFS): with the digital profile the speech-to-noise distance fell from 17 to 11 dB, the AGC raised the noise between words by up to 30 dB. With the analog profile's defaults it rises to 25 dB, and noisier signals are no longer made worse.
- **20 minutes of FM-Funknetz traffic** recorded at an SvxLink node: on RF stations the distance fell from 34 to 19 dB with the digital profile and stays at 32 dB with the analog one; for app and RoIP clients 36 dB (19.7 with the digital profile).

## Noise suppression

QSOs through an SvxLink node to DMR (2026-10-15): at -15 dB noise suppression, with the gate floor at -12 dB on top, the pauses went dead silent, the comfort noise was missing and quiet syllable onsets and endings sounded cut off, since speexdsp takes them for noise. -10 dB with the gate floor at 0 keeps a quiet, natural noise floor and whole syllables, and the robotic AMBE sound on DMR stays away. Stronger values remove more noise but cut syllables and leave "musical" artifacts.

The DVSI chips have a noise suppressor of their own (`NS_ENABLE` in `ECMODE`, on by default); tcd switches it off (`ECMODE = 0`). It sits in the encoder channel and would act on everything that channel encodes, also clean D-Star, M17 and P25 audio, can't tell an FM source from the others and is set per channel at start, not per stream. speexdsp works where the analog input comes in, only on SVX and USRP, the same for every target codec.

## Speech-driven AGC

The AGC tracks the level in dB on the upper envelope of the speech and only raises the gain on clearly voiced frames well above a noise floor measured per stream. Measured on 18 FM-Funknetz transmissions of RF stations (2026-10-23) through the analog path (noise suppression and analog AGC) against the earlier RMS-window AGC:

- the stations end up as close together as before (spread 1.2 dB), at about the same level (-16.4 against -15.6 dBFS)
- the gain moves a third as much from frame to frame (0.20 against 0.55 dB)
- the speech stands 2.5 dB further above the noise in the pauses

## Lost answers on the DV3003 and `DeviceCompand`

Raspberry Pi 5 with a DVstick-33 at 921600 baud (2026-11-02): one answer in some 30 000 to 60 000 lost to an FTDI receive overrun, about 20 in 1.5 hours of traffic on a reflector in service; never with the DV3000 at 460800 baud. Only decoder answers were hit (327 bytes of audio each); encoder answers are 16 bytes.

With `DeviceCompand` (8-bit G.711 instead of 16-bit linear, half the bytes): 0 overruns in 20 minutes of the load that had some 7 before. Audio quality (PESQ), linear / µ-law / A-law:

| Path | linear | µ-law | A-law |
|---|---|---|---|
| a DMR clip decoded on the chip | 2.811 | 2.807 | 2.800 |
| through tcd, DMR in, its PCM out | 2.946 | 2.942 | 2.946 |
| DMR → D-Star (decoded and encoded on the chips) | 2.207 | 2.180 | 2.175 |

The difference cannot be heard behind the AMBE codec. A byte that is still lost shifts the samples of one frame by one sample (an eighth of a millisecond) instead of turning them into noise.

## Direct transcoding

D-Star, DMR/YSF and P25 share the MBE vocoder model; the second analysis of synthesized speech is what a transcoder loses most to. Coding the target frame from the source frame's parameters (`Transcode = direct`), measured over 19 clips: each of the six directions gains 0.09 to 0.24 PESQ over decode and encode, confirmed through a DV3000 for D-Star and DMR, at a fifth of the processing time.

## P25 to M17

The IMBE decoder puts extra energy below 120 Hz into its audio, which threw Codec2's pitch estimation off: PESQ about 1.6. A 300 Hz high-pass (4th order) in front of the Codec2 encoder for P25 audio brings it to 2.5.

## CPU load and latency

- Built with `-O2`, a transcoded stream (Codec2, IMBE and AGC in software) takes about 7 % of one core on a Raspberry Pi 4 and 3 % on a Pi 5.
- tcd's own P25 vocoder (OP25) with link-time optimization, `-O3` and a 64-bit pitch search, on a Raspberry Pi 4: encode 0.37 ms a frame (1.06 before), decode 0.15 ms (0.27), bit-identical frames. A Pi 4 keeps up with 26 modules talking P25 at once (99 % of the frames within 15 ms); before it fell behind at 18.
- `TCP_NODELAY` on the reflector connection: a frame returns after about 23 ms instead of 60 ms.
- Module workers, on a Raspberry Pi 5 (4 workers), a codec plugin with parallel calls for D-Star, DMR, P25 and M17, every module transcoding at once for 30 s from a test client in place of urfd, latency from a frame in to its frame of every codec out:

  | Load | before: one thread per codec | module workers |
  |---|---|---|
  | 26 modules, all six input codecs mixed | 43 % of a core, 99 % within 0.8 ms, at most 4.8 ms | 36 %, 0.8 ms, at most 3.7 ms |
  | the same, all 26 frames at the same instant | 38 %, 99 % within 8.0 ms, at most 9.9 ms | 37 %, 3.6 ms, at most 6.5 ms |
  | 26 modules of USRP (heaviest: every codec encoded) | 60 %, 0.6 ms, at most 7.0 ms | 50 %, 0.5 ms, at most 3.3 ms |

  No frame was lost. One thread per codec behaved the same on one core as on four: every codec thread waited for the plugin, which tcd called from one thread at a time.
