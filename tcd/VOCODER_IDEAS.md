# Vocoder Ideas – MD380 Cache and More Transcoding Channels

Two parts: how the MD380 software vocoder keeps per-stream state, and the
options for more transcoded modules (MD380, other AMBE software, OP25).

## MD380 Vocoder Cache – Design Notes

> Status (tcd 0.2.9): with two AMBE devices the MD380 is optional. Since 0.2.5
> `DmrGainMode = ambe` gives DMR → DMR/YSF the AGC in the bitstream without
> re-encoding, and it sounds better than the MD380 re-encode. The MD380 is still
> needed as the DMR/YSF codec of a single device setup. Since 0.2.9 every MD380
> use goes through `Md380SelectStream()`, and a new stream starts from the RAM
> saved right after `md380_init()`.

### Current Approach: memcpy-based State Swapping

The MD380 firmware is linked to fixed memory addresses:

```
.firmware = 0x0800C000   (code, read-only)
.sram     = 0x20000000   (RAM, 128 KB)
```

The ARM code references absolute addresses for global variables, buffers, and
codec state. This means only **one firmware instance** can exist within a single
process address space. Running multiple instances (via fork or loading the
library twice) is not possible because both would map to the same virtual
address.

Since the AMBE2+ codec is stateful (pitch tracking, filter coefficients, and
synthesis state persist across frames), each active DMR stream needs its own
encoder state. The solution is a per-stream cache that saves and restores the
entire 128 KB SRAM via `memcpy` on stream switches.

#### Why this works well enough

- With 2x AMBE3003 transcoders, the practical limit is ~3 concurrent streams
- 3 streams = 384 KB cache memory, negligible
- A 128 KB memcpy takes ~50-100 us on a Raspberry Pi
- DMR frames arrive every 60 ms, so even 2 swaps per frame use < 0.5% of the budget
- The mutex serialization is not a bottleneck at this stream count

#### When it would become a problem

- 10-15+ simultaneous active streams (swap overhead and mutex contention)
- Aggressive cache eviction (30s timeout) causing state loss under heavy load
- Scenarios where every frame triggers a stream switch across many streams

Given the hardware constraints (2x AMBE3003), none of these are realistic.

---

### Alternative Approaches Considered

#### 1. mmap Page Swapping

Instead of copying 128 KB, use `mmap` with `MAP_FIXED` to swap page table
entries at `0x20000000` between per-stream memory backings.

- **Pro:** O(1) kernel page table update instead of memcpy
- **Con:** Still need backing storage per stream; complexity of managing
  fd-backed or anonymous mappings; marginal gain at 128 KB
- **Verdict:** Not worth the added complexity for 3 streams

#### 2. Multi-Process Worker Pool

Spawn N worker processes, each with its own virtual address space. Each process
owns one stream and runs its own firmware instance. Communication via Unix
sockets or shared memory.

- **Pro:** True parallelism, no swapping needed, clean isolation
- **Con:** IPC overhead, architectural complexity, process management
- **Verdict:** Good option if stream count ever grows significantly

#### 3. Multiple Dynarmic Instances (x86_64 / aarch64 only)

On non-ARM platforms, the MD380 firmware runs inside dynarmic (ARM Cortex-M JIT
emulator). Dynarmic virtualizes memory internally, so multiple emulator
instances could each have their own independent SRAM at the emulated address
`0x20000000` without conflict.

- **Pro:** Cleanest solution, true parallelism, no page tricks needed
- **Con:** Only works with dynarmic, not on native ARM; requires changes to
  the md380_vocoder_dynarmic library
- **Verdict:** Most elegant approach for x86/aarch64 if needed in the future

---

### Conclusion

The current memcpy-based cache is simple, correct, and performant within the
hardware constraints. No changes needed unless the number of concurrent streams
grows well beyond what 2x AMBE3003 can handle.

---

## More Transcoding Channels – Ideas (2026-10-18, parked)

Decision: a second DV3003 replaces the DV3000 (2x DV3003, up to 3 modules, no
code change, no MD380). The options below were looked at and set aside; they
stay here in case more modules are wanted without more hardware.

### What limits the number of modules

Every transcoded module needs one D-Star channel (AMBE 2400) and one DMR/YSF
channel (AMBE+2 2450). The MD380 only does AMBE+2, so D-Star always needs a
DVSI channel. With DV3000 + DV3003 (4 hardware channels):

| Split | D-Star | DMR/YSF | Modules |
|---|---|---|---|
| mixed mode (today) | 2 HW | 2 HW | 2 |
| MD380 as DMR codec of one module | 3 HW | 1 HW + MD380 | 3 |
| MD380 as DMR codec of all modules | 4 HW | MD380 only | 4 (one serialized MD380 for all) |

"DV3000 + DV3003 + MD380" is not implemented: `Controller.cpp` knows single
device + MD380, two same-type devices, and mixed mode only. It would need a new
channel assignment, plus stats and monitor. The CPU cost of several parallel
streams through one MD380 under dynarmic on a Pi 4 has not been measured.

Untested side idea: the MD380 firmware is Cortex-M Thumb-2 code that runs
natively on armhf. A Pi 4 can execute AArch32, so an armhf build of tcd might
run the MD380 without dynarmic (needs 32-bit libraries on the host).

### Other AMBE software

| Library | Does | Encodes? | Fit |
|---|---|---|---|
| md380_vocoder (used today) | AMBE+2 from the Tytera MD380 firmware | yes | DMR/YSF only |
| mbelib, mbelib-neo (DSD, DSD-FME) | AMBE, AMBE+2, IMBE | no, decode only | no: transcoding has to encode |
| DVSI software library | AMBE, AMBE+2 | yes | commercial, licence per channel |
| OP25 (see below) | AMBE 2400 and AMBE+2 2450 | yes | possible, quality unknown |

### OP25 software AMBE (boatbod/op25, checked at c5d14a6, 2026-10-17)

In `op25/gr-op25_repeater/lib/`:

- `ambe_encoder.cc` / `ambe_encoder.h` (Max H. Parke KA1RBI, 2016, GPLv3): a
  software AMBE encoder with two modes: `set_dstar_mode()` gives AMBE 2400 for
  D-Star including the D-Star interleave to the 72-bit frame
  (`set_alt_dstar_interleave()`), `set_49bit_mode()` gives the 49-bit AMBE+2
  frame of DMR/YSF and P25 phase 2. `set_gain_adjust()` sets a level offset.
- How it encodes: the IMBE analysis of `imbe_vocoder` (the same base as tcd's
  P25 vocoder) computes the model parameters (pitch, voicing, amplitudes),
  `encode_ambe()` quantizes them to AMBE parameters. It is not DVSI's
  algorithm but its own quantization onto the AMBE format.
- Decoding: the bundled mbelib (`mbe_dequantizeAmbe2400Parms`, the 2450
  variant) plus OP25's `software_imbe_decoder` for synthesis.
- `d2460.cc` (`op25-d2460`): a software DV3000 on UDP port 2460 speaking the
  DV3K protocol (like an AMBEserver). D-Star only, one channel, tones not
  handled (FIXME in the code), `GAIN_ADJUST` 7 dB attenuation.

What it would mean for tcd:

- A plain C++ object per stream: no fixed addresses as with the MD380, so one
  instance per module, truly parallel, no state swapping.
- CPU: the IMBE analysis already runs in tcd for P25 (a whole transcoded stream
  is about 7 % of a Pi 4 core); AMBE would add a similar load, not measured.
- Licence: GPLv3 fits tcd. Patents: the same grey area as md380 and mbelib.
- Work: a new "software AMBE" device type next to DV3000, DV3003 and MD380,
  with the OP25 sources built as a library; days, not an evening.

Open, and decisive: audio quality. Encoders that go through IMBE parameters are
generally said to sound worse than the DVSI chips, and mbelib decoding is known
to sound a bit metallic; neither has been checked here. For D-Star and DMR
users the encoder matters most, as their radios decode with real DVSI code.

Test to run before any integration: encode the same speech recordings once with
OP25 and once with a DV3003, decode both with the DV3003 (what a radio hears),
separately for D-Star and DMR, and compare by ear. About one to two hours on
zaphod with the existing hardware.
