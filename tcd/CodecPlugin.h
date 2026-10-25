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
// DMR/YSF (and DmrGainMode = reencode).
//
// The interface is plain C, so a plugin needs none of tcd's classes and does not
// have to be rebuilt with every tcd update; tcd checks TCD_CODEC_API only.
// A shared object exports one function:
//
//   extern "C" const struct tcd_codec_plugin *tcd_codec_plugin(void);
//
// tcd calls a plugin from one thread at a time. A frame is 20 ms, 160 samples of
// 8 kHz PCM. Each stream gets a state of its own (open/close), so streams do not
// mix their vocoder state; tcd closes the state of a stream that has been quiet
// for 30 s.
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TCD_CODEC_API 1

// the codecs, also as bits in tcd_codec_plugin.codecs
#define TCD_CODEC_DSTAR 0x01u   // D-Star AMBE 3600x2400, 9 bytes with FEC
#define TCD_CODEC_DMR   0x02u   // DMR/YSF/NXDN AMBE+2 3600x2450, 9 bytes (72 bits with FEC)
#define TCD_CODEC_P25   0x04u   // P25 IMBE 4400, 11 bytes

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
};

typedef const struct tcd_codec_plugin *(*tcd_codec_plugin_func)(void);

#ifdef __cplusplus
}
#endif
