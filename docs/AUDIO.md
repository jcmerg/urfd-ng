# Audio

**English** | [Deutsch](AUDIO.de.md)

What urfd and tcd do to the voice that passes through the reflector, how the dashboard rates a transmission, and the measurements behind it. The transcoder's own settings (AGC, gains, vocoders) are in [tcd/README.md](../tcd/README.md).

## Even levels across all modes

tcd's AGC brings every call on a transcoded module to the same level, whatever radio or vocoder it came from. It also covers DMR → DMR/YSF, where the AMBE+2 frames pass through without a vocoder: the AGC's gain goes into the frame's gain parameter (b2) instead of a decode and re-encode, so the audio stays as clean as the original.

The AGC follows the upper envelope of the speech and only rises on clearly voiced speech well above a noise floor it measures per stream, so the gain does not pump on breath, hiss or pauses. On FM-Funknetz traffic this made the gain move a third as much as a plain level AGC and left the speech 2.5 dB further above the noise, at the same level and spread between stations.

SvxLink and AllStar (SVX, USRP) go through an AGC of their own for analog sources: FM audio is not silent between words but carries the receiver's noise, which the AGC for digital sources raised by up to 30 dB. On 20 minutes of FM-Funknetz traffic the analog AGC kept 32 dB speech-to-noise, against 19 dB with the digital one.

Two DVSI devices, e.g. DV3000 + DV3003 for two modules, need no codec plugin; a Raspberry Pi 4 is plenty.

## FM levels from SvxLink and AllStar

The node's receiver must not clip, the AMBE encoder makes clipped audio sound harsh. A SHARI node (CM108) with `Mic Capture Volume` at 23 of 35 clipped every transmission (speech at -6 dBFS, peaks at full scale); at 15 the speech came at -16 dBFS without clipping:

```bash
amixer -c Device cset numid=8 15
alsactl store
```

Keep `RxGain` in `urfd.ini` at 0 and let tcd's analog AGC set the level. The FM noise under the speech turns robotic after the AMBE encoder on DMR; tcd removes most of it before encoding (`AGCAnalogNoiseSuppress`).

## Lost voice frames filled in

D-Star numbers its voice frames 0..20 and DMR its bursts A..F with three frames each. When up to 6 frames go missing on the way to the reflector, urfd puts frames in their place that carry each vocoder's own lost-frame code (D-Star b0 127, AMBE+2 b0 120 as an erasure, FEC included). Every decoder, tcd's codec plugins, the DVSI chips and the radios, repeats the last frame for them and mutes after three, as for a frame it could not correct, and the stream keeps its frame count, D-Star sync and DMR bursts. Without this the decoders would hear the next frame too early and D-Star receivers would lose the sync position.

- The filled frames go out at once, before the frame that showed the gap, so the stream's delay grows by the gap.
- A frame up to 6 positions behind that comes within 0.2 s is late or repeated and is dropped; its position went out already. Deeper reordering cannot be told from a loss.
- Longer gaps stay as they are. XLX peers (D-Star and DMR in one frame) and the echo module's playback are not filled.
- DMR+ uses one stream id per address, so a new transmission within a few frames after a lost end packet joins the old stream, with up to 6 filled frames between them.
- The log says `Module A: 5 lost voice frames filled in` when a stream closes.
- P25, NXDN, YSF, USRP and M17 carry no frame counter urfd could use (SVX conceals its own gaps).

## Repeated AMBE+2 frames muted

Some radios and hotspots end a DMR, YSF or NXDN transmission with the same AMBE+2 frame again and again, 6 to 30 times. On BrandMeister TG 262 and 91 this was a third of the transmissions, from a third of the stations, and every decoder played it as the last sound once more. Speech never has the same frame twice in a row, beyond the silence frame, so from the third one in a row urfd sends AMBE+2 silence instead, for every target; the log counts them per stream.

## Echo of a call's end from the master

BrandMeister (TG 262, from many stations) sends after most calls a second stream from the same ID, 0.6 to 1.2 s later, with the call's last 36 to 66 frames again (a bit or two different per frame), as if from a second path with a delay; everyone would hear the last second twice. The MMDVMClient holds a new stream from an ID that spoke on the module in the 3 s before until its first voice frames (60 ms mostly, at most 360 ms). When they are frames of that ID's call of the last 3 seconds, it drops the stream and logs `echo of the end of the call from ... dropped`. Other IDs keying up in between do not disturb the check. A new call from that ID with other speech goes through. Checked against an hour of BrandMeister TG 262: all 33 echoes dropped, no other stream.

## DMR calls without voice

Some devices key up on their own after a talkgroup change: a full transmission of about 3 s whose voice frames are all zero bytes. On `[MMDVM]` a call opens its stream only with its first voice frame that is not all zeros; one that ends without any is dropped and logged (`call from ... without voice ... ignored`), so it shows neither in last heard nor as 3 s of nothing on the module.

## Frames the transcoder lost

urfd matches each answer of tcd to the frame it waits for. When tcd never answers a frame (an AMBE device hung), the frames before the next answer are skipped and the transmission goes on, instead of losing the rest of it.

## YSF VW latency

VW (Voice Wide, IMBE full rate) speech reaches the other modes about 85 ms later than DN: the first VW frame after the header has 40 ms of speech.

## Audio report of each transmission

On a transcoded module tcd reports after every transmission how it sounded: input level and peak before the AGC, the share of speech frames with a peak of -1 dBFS or more (clipped), the level after the AGC and the AGC's gain, for AMBE streams the frames the FEC corrected or could not save, and with a codec plugin as decoder the frames it repeated or muted. urfd adds the voice frames lost on the way and filled in, and the length of the transmission (on every module). It is stored with the last heard entry and sent to the dashboard as `user_report` ([docs/WEBSOCKET.md](WEBSOCKET.md)). Nothing in it names the vocoder.

## How the dashboard rates a transmission

Last heard shows on transcoded modules a small meter of the input level with its peak, a word in green, orange or red, what the AGC changed and the frames lost or muted; the details are in the tooltip.

| Word | When |
|------|------|
| Short (grey) | under 1.5 s, or under 1 s of speech: too short to rate the level |
| No speech | 3 s or more without any speech (a stuck PTT, a dead microphone) |
| Overdriven | 5 % or more of the speech clipped, or level above -8 dBFS |
| Too quiet | level below -46 dBFS |
| Loud | 1 % or more clipped, or level above -12 dBFS |
| Quiet | level below -36 dBFS |
| Bit errors, Packet loss | 2 % (orange) or 5 % (red) of the frames lost to the FEC or the network, counted from 50 frames on |
| Good | otherwise |

The level bands follow the codec tests of 3GPP, which take -26 dBov as nominal and -16 to -36 dBov as normal; "Too quiet" starts 20 dB below nominal, where the AGC comes to its limit. DMR radios commonly arrive around -32 dBFS, which the AGC brings up without trouble.

**Hide kerchunks** leaves out ended transmissions under 1 s (by urfd's length, header to last packet), and longer ones with under 0.3 s of speech unless bit errors or packet loss muted them. A "73" of 1 s or more stays, and so does "No speech"; a station on the air always shows. The choice is kept in the browser.
