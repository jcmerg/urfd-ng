# IMBE vocoder

The P25 IMBE 4400 vocoder (fixed point) by Pavel Yazev (2009), from osmocom
OP25, as packaged in [nostar/imbe_vocoder](https://github.com/nostar/imbe_vocoder)
(commit 03423cb, 2021-11-27). tcd builds it from here, so no extra library has
to be installed.

License: GNU General Public License version 3 or later, as stated in each file,
the same as tcd (see ../LICENSE). The files are unchanged apart from this README and one fix (tcd 0.23.3): the
32-bit arithmetic that wraps around on purpose (the overflow check of `L_add`,
`L_sub` and the carry versions, `L_deposit_h`, the phase accumulators of
`v_synt.cc`, one product in `pitch_est.cc`) is done in unsigned arithmetic
(`wrap32.h`). A signed overflow and the left shift of a negative value are
undefined in C++, the compiler may assume they do not happen; UBSan reported
them. The results are the same bits: encode and decode of 28 630 frames (speech
from -26 to +30 dB, clipped, full-scale noise, a square wave, a sweep, silence,
random frames) give the same output as before.
