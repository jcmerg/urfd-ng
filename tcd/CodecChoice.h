#pragma once

// tcd -- the vocoder choices: who codes D-Star, DMR, P25 (IMBE) and M17 (Codec2)
// for the streams that start from now on. One row per choice; the tcd.ini
// line, the monitor, /api/codec and the log take its names from here, and
// tcd keeps one entry per choice in arrays indexed by ECodecChoice.
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include <cstdint>
#include <string>

#include "CodecPlugin.h"

enum ECodecChoice : int { kChoiceDStar, kChoiceDMR, kChoiceP25, kChoiceM17, kNumChoices };

struct SCodecChoice
{
	uint32_t codec;         // TCD_CODEC_* of the plugins that may do it
	const char *key;        // in /api/stats, /api/codec and the monitor
	const char *ini;        // the line of tcd.ini, e.g. CodecDStar
	const char *name;       // short, for the start-up errors
	const char *label;      // for the log and the monitor
	// tcd's own vocoder for every module ("op25", "codec2") and its row in the
	// monitor's vocoder table; nullptr: the AMBE devices ("dvsi") for theirs
	const char *own;
	const char *own_row;
};

inline constexpr SCodecChoice g_CodecChoices[kNumChoices] = {
	{ TCD_CODEC_DSTAR, "dstar", "CodecDStar", "D-Star", "D-Star",             nullptr,  nullptr  },
	{ TCD_CODEC_DMR,   "dmr",   "CodecDMR",   "DMR",    "DMR",                nullptr,  nullptr  },
	{ TCD_CODEC_P25,   "p25",   "CodecP25",   "P25",    "IMBE (P25, YSF VW)", "op25",   "OP25"   },
	{ TCD_CODEC_M17,   "m17",   "CodecM17",   "M17",    "Codec2 (M17)",       "codec2", "Codec2" },
};

// the choice a TCD_CODEC_* goes with (M17 1600 with M17), -1 for none
inline int ChoiceOfCodec(uint32_t codec)
{
	if (codec == TCD_CODEC_M17_1600)
		return kChoiceM17;
	for (int k = 0; k < kNumChoices; k++)
		if (g_CodecChoices[k].codec == codec)
			return k;
	return -1;
}

// the choice of a line of tcd.ini (CodecDStar, ...), -1 for none
inline int ChoiceOfIni(const std::string &ini)
{
	for (int k = 0; k < kNumChoices; k++)
		if (ini == g_CodecChoices[k].ini)
			return k;
	return -1;
}

// the choice of a key of /api/codec, -1 for none
inline int ChoiceOfKey(const std::string &key)
{
	for (int k = 0; k < kNumChoices; k++)
		if (key == g_CodecChoices[k].key)
			return k;
	return -1;
}
