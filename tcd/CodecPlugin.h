#pragma once

// CodecPlugin -- a software vocoder tcd loads from a shared object at start
//
// tcd.ini names the shared objects:
//
//   CodecPlugin = /usr/local/lib/tcd/foo.so
//   CodecPlugin = ...                         (as many as needed)
//
// tcd does the codecs it can do itself (Codec2, IMBE, PCM) and uses DVSI
// hardware for AMBE. A plugin brings a codec in software, for a codec tcd has no
// device for: with one DVSI device for D-Star, a plugin with TCD_CODEC_DMR does
// DMR/YSF (and DmrGainMode = reencode). Since tcd 0.14.0 a plugin with
// TCD_CODEC_P25 can do P25 instead of tcd's own OP25 vocoder (CodecP25).
//
// The interface is plain C, so a plugin needs none of tcd's classes and does not
// have to be rebuilt with every tcd update; tcd checks the plugin's api only, and
// loads plugins from TCD_CODEC_API_MIN to TCD_CODEC_API.
// A shared object exports one function:
//
//   extern "C" const struct tcd_codec_plugin *tcd_codec_plugin(void);
//
// tcd calls a plugin from one thread at a time. A frame is 20 ms, 160 samples of
// 8 kHz PCM. Each stream gets a state of its own (open/close), so streams do not
// mix their vocoder state; tcd closes the state of a stream that has been quiet
// for 30 s. start and stop tell the plugin where a transmission begins and ends:
// a state can carry several transmissions one after the other (the same stream
// id again), each between a start and a stop.
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TCD_CODEC_API     3	// 2: start and stop; 3: decode_info, frame_format
#define TCD_CODEC_API_MIN 2	// the oldest api tcd still loads

// the codecs, also as bits in tcd_codec_plugin.codecs
#define TCD_CODEC_DSTAR 0x01u   // D-Star AMBE 3600x2400, 9 bytes with FEC
#define TCD_CODEC_DMR   0x02u   // DMR/YSF/NXDN AMBE+2 3600x2450, 9 bytes (72 bits with FEC)
#define TCD_CODEC_P25   0x04u   // P25 IMBE 4400, 11 bytes

// how a frame was decoded (api 3), as bits in tcd_frame_info.flags
#define TCD_FRAME_REPEAT  0x01u   // played as a repeat of the last good frame
#define TCD_FRAME_MUTE    0x02u   // muted: too many bit errors or repeats in a row
#define TCD_FRAME_TONE    0x04u   // a tone frame (DTMF or another signalling tone)
#define TCD_FRAME_SILENCE 0x08u   // the codec's silence frame

struct tcd_frame_info
{
	int32_t errors;             // bit errors the FEC corrected (0 for a codec without FEC)
	uint32_t flags;             // TCD_FRAME_*
};

struct tcd_codec_plugin
{
	uint32_t api;               // TCD_CODEC_API
	const char *name;           // short, for the log and the monitor, e.g. "mbelib-neo"
	const char *description;    // one line
	uint32_t codecs;            // TCD_CODEC_* it can do

	// a line of tcd.ini that tcd does not know: 1 = the plugin's (taken),
	// 0 = not the plugin's, -1 = the plugin's but a bad value. Called before init.
	int (*option)(const char *key, const char *value);
	// 0 = ready; on an error it writes why to stderr
	int (*init)(void);
	void (*shutdown)(void);

	// a stream's state for one codec, NULL on an error
	void *(*open)(uint32_t codec);
	void (*close)(void *state);
	// 0 = ok; frame has the size of the codec
	int (*encode)(void *state, const int16_t *pcm, uint8_t *frame);
	int (*decode)(void *state, const uint8_t *frame, int16_t *pcm);

	// both may be NULL. start: before the first frame of a transmission, the
	// state is to begin afresh (a new state gets a start too). stop: after its
	// last frame. A transmission whose last frame got lost gets no stop: the
	// next start on the state, or close, ends it.
	void (*start)(void *state);
	void (*stop)(void *state);

	// api 3: tcd reads these only from a plugin built for api 3 or later; each
	// may be NULL.
	// decode_info: decode, and say how the frame came out (info is never NULL);
	// tcd uses it instead of decode for its statistics.
	int (*decode_info)(void *state, const uint8_t *frame, int16_t *pcm, struct tcd_frame_info *info);
	// frame_format: bytes and 8 kHz samples per frame of a codec, 0 = ok. NULL:
	// 9 bytes for D-Star and DMR, 11 for P25, 160 samples (20 ms). tcd carries
	// only these and refuses a plugin that reports another format.
	int (*frame_format)(uint32_t codec, uint32_t *bytes, uint32_t *samples);
};

typedef const struct tcd_codec_plugin *(*tcd_codec_plugin_func)(void);

#ifdef __cplusplus
}
#endif
