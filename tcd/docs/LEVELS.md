# Levels in depth

**English** | [Deutsch](LEVELS.de.md)

The details behind the AGC and the same-mode gain of [tcd/README.md](../README.md#gains).

## How the AGC works

The AGC works per stream (per transmission):
- a 75 Hz high-pass on the audio, so DC and hum neither drive the level nor reach the encoder
- the speech level follows the upper envelope of the speech, close to the vowels; noise floor, gate and voice detection look only at the band above 300 Hz, so a CTCSS tone or hum does not count
- rises are followed quickly; the level only falls (the gain only rises) on clearly voiced frames well above the noise floor, so breath, hiss and pauses do not make the gain pump
- the first voiced frames of a stream set its level fast, so the first words of a quiet speaker are not too quiet
- a noise floor per stream: weak, noisy signals are not lifted into their noise, clean sources get the full gain
- a noise gate that bridges the gaps between words (240 ms) and then fades the gain to the gate floor, so word endings stay and noise is not amplified
- the gain ramps sample by sample across each frame, and a peak limiter just below full scale keeps it from clipping

The gain limits are asymmetric on purpose: attenuating is safe, amplifying also lifts noise. Typical DMR input sits around -35 dBFS, so about +20 dB are needed to reach a -16 dBFS target.

## Analog sources and noise suppression

**Analog sources (SVX, USRP)** carry FM audio, whose pauses are not silent but the receiver's noise, often -45 to -35 dBFS. With the settings for digital sources the AGC would raise that noise between words by up to 30 dB. AGC analog has its own gate, gain limit and gate floor, so the speech stays well above the noise (on recorded FM-Funknetz traffic 32 dB speech-to-noise instead of 19, see [measurements](MEASUREMENTS.md#agc-on-analog-sources)). The gate works on the level after urfd's `RxGain`, so keep that near 0 and let the AGC set the level. A squelch tail is louder than any gate: `SQL_TAIL_ELIM` in the SvxLink node's receiver removes it at the source.

**Noise suppression** (`AGCAnalogNoiseSuppress`): the gate only works between words; the FM noise under the speech stays, and the vocoders, AMBE above all, turn it into a warbling, robotic sound. So SVX and USRP input first goes through the speexdsp preprocessor, without added delay, with one noise estimate per module. Its own AGC, VAD and dereverb are off. -10 dB keeps a quiet, natural noise floor and whole syllables; stronger values remove more noise but cut syllable onsets and leave "musical" artifacts. Live in the monitor, 0 switches it off. The AMBE chip's own noise suppressor is switched off by tcd: it would act on every codec that channel encodes, also on clean digital audio.

## Same mode: the gain in the frame

- **DMR (AMBE2+)**: the decoder computes γ = Δγ[b2] + 0.5·γ<sub>prev</sub>, and a frame's mean log2 amplitude is γ, so one unit of γ is 6 dB. The Δγ table is coarse for low b2 and fine for high b2. tcd tracks γ for the original and the modified stream and chooses, frame by frame, the b2 that comes closest to the wanted gain; the rounding error is made up in the next frame. So the gain does not depend on the level: gains of -12 to +6 dB arrive within 0.8 dB on input at -40 to -16 dBFS. Very quiet sources cannot be raised beyond what b2 = 31 allows.
- **D-Star (AMBE)**: b2 has 6 bits, and the decoder takes γ = 2·Δγ[b2] − 0.09 without the frame before, so each frame gets the b2 closest to its own γ plus the wanted gain. The steps are 0.7 to 1.8 dB, the same across the range: -12 to +3 dB arrive within 0.15 dB (0.2 to 0.5 dB from frame to frame), +6 dB and more fall short on loud speech, where b2 reaches its top.
- **P25 (IMBE)**: b2 sets G1, the mean of the log2 amplitudes; the decoder's prediction has its mean removed, so the level follows b2 with no memory. 64 steps of 1 to 1.5 dB; -12 to +6 dB arrive within 0.2 dB.
- **M17 (Codec2 3200 and 1600)**: the 5-bit frame energy, 1.56 dB per step (1600: both energies of the 40 ms frame; the data half stays). -12 dB arrives as -12.5 at most, as the steps fall.

P25 and M17 are the same with every decoder: measured with OP25 and mbelib-neo for IMBE, tcd's Codec2 for M17, against frames of either encoder.
