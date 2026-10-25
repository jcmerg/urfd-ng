#pragma once

// CodecPlugins -- the software vocoders of the CodecPlugin lines in tcd.ini,
// see CodecPlugin.h
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include <chrono>
#include <memory>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "CodecPlugin.h"

// one codec of one plugin, with a state per stream
class CSoftCodec
{
public:
	CSoftCodec(const tcd_codec_plugin *plugin, uint32_t codec, std::shared_ptr<std::mutex> mutex) : m_Plugin(plugin), m_Codec(codec), m_Mutex(mutex) {}
	~CSoftCodec();

	const char *GetName(void) const { return m_Plugin->name; }
	const tcd_codec_plugin *GetPlugin(void) const { return m_Plugin; }
	uint32_t GetCodec(void) const { return m_Codec; }
	// false on an error of the plugin; the frame has the size of the codec
	bool Encode(uint16_t streamid, const int16_t *pcm, uint8_t *frame);
	bool Decode(uint16_t streamid, const uint8_t *frame, int16_t *pcm);
	// the streams it codes right now (a frame in the last 2 s)
	int Streams(void);
	// its entry in g_Stats.sw, -1 for none
	int stats = -1;

private:
	void *State(uint16_t streamid);	// with m_Mutex held

	struct SState { void *state; std::chrono::steady_clock::time_point last_used; };
	const tcd_codec_plugin *m_Plugin;
	uint32_t m_Codec;
	std::shared_ptr<std::mutex> m_Mutex;	// the plugin's: one call at a time, for all its codecs
	std::unordered_map<uint16_t, SState> m_States;
};

class CCodecPlugins
{
public:
	~CCodecPlugins();

	// loads the shared objects and hands them the lines of tcd.ini tcd does not
	// know (key, value); true on an error. Lines no plugin takes are warned about.
	bool Load(const std::vector<std::string> &paths, const std::vector<std::pair<std::string, std::string>> &options);
	// the software codec for a TCD_CODEC_*: of the plugin with that name, or
	// without a name of the first plugin that has it; nullptr when there is none
	CSoftCodec *Get(uint32_t codec, const std::string &name = std::string());
	// every plugin's software codec for a TCD_CODEC_*, in the order of the CodecPlugin lines
	std::vector<CSoftCodec *> All(uint32_t codec);

private:
	struct SLoaded { std::string path; void *handle; const tcd_codec_plugin *plugin; bool ready; std::shared_ptr<std::mutex> mutex; };
	std::vector<SLoaded> m_Loaded;
	std::vector<CSoftCodec *> m_Codecs;	// made by Get, one per plugin and codec
};

extern CCodecPlugins g_Codecs;
