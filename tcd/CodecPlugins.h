#pragma once

// CodecPlugins -- the software vocoders: tcd's own (BuiltinCodecs.h) and those
// of the CodecPlugin lines in tcd.ini, see CodecPlugin.h
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include <chrono>
#include <map>
#include <memory>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "CodecPlugin.h"

// one codec of one plugin, with a state per stream
class CSoftCodec
{
public:
	CSoftCodec(const tcd_codec_plugin *plugin, uint32_t codec, std::shared_ptr<std::mutex> mutex, bool builtin) : m_Plugin(plugin), m_Codec(codec), m_Mutex(mutex), m_Builtin(builtin) {}
	~CSoftCodec();

	const char *GetName(void) const { return m_Plugin->name; }
	const tcd_codec_plugin *GetPlugin(void) const { return m_Plugin; }
	uint32_t GetCodec(void) const { return m_Codec; }
	// tcd's own vocoder (OP25, Codec2), not a plugin of a CodecPlugin line
	bool IsBuiltin(void) const { return m_Builtin; }
	// false on an error of the plugin; the frame has the size of the codec.
	// sequence and last of the packet: where a transmission begins and ends
	bool Encode(uint32_t stream, uint32_t sequence, bool last, const int16_t *pcm, uint8_t *frame);
	// info, when not nullptr, gets how the frame was decoded (a plugin before
	// api 3 reports nothing: errors 0, no flags)
	// hold: a last frame does not stop the state yet, Transcode still needs it
	// (End ends it after)
	bool Decode(uint32_t stream, uint32_t sequence, bool last, const uint8_t *frame, int16_t *pcm, tcd_frame_info *info = nullptr, bool hold = false);
	// the transmission of a stream ended after a frame that did not reach this
	// codec (M17 1600 decodes the first packet of a payload, urfd marks the
	// second as the last): stop its state
	void End(uint32_t stream, bool encode);
	// api 4: the frame this codec's decoder state of the stream just decoded,
	// coded by the plugin's codec `to` (another codec of the same plugin) from
	// the frame itself, the level changed by gain_db; false when the plugin
	// cannot (CanTranscode) or fails, or the transmission is not on the direct
	// path (it takes one only from its first frame on, and keeps it only while
	// every frame comes here: after a frame that did not, or a failure, the rest
	// goes the audio path). sequence and last as for Encode
	bool HasTranscode(void) const { return m_Plugin->api >= 4 && m_Plugin->transcode; }
	bool SamePlugin(const CSoftCodec *other) const { return other && other->m_Plugin == m_Plugin; }
	bool CanTranscode(const CSoftCodec *to) const { return SamePlugin(to) && HasTranscode() && to->m_Codec != m_Codec; }
	bool Transcode(uint32_t stream, uint32_t sequence, bool last, CSoftCodec *to, float gain_db, uint8_t *frame);
	// a frame of the stream not given to Transcode for `to` (not decoded, Transcode
	// switched to audio): a transmission on the direct path leaves it for its rest,
	// stopped at its last frame. Transcode also sees a frame missing by its
	// sequence, this tells it at the frame itself
	void LeaveDirect(uint32_t stream, bool last, CSoftCodec *to);
	// the streams it codes right now (a frame in the last 2 s)
	int Streams(void);
	// urfd counts the frames of a transmission from 0: frame 0, or a lower count
	// than the last on the same stream id, is a new transmission (the last frame
	// of the one before got lost). tcd's own vocoders tell transmissions apart the same way
	static bool NewTransmission(uint32_t last_sequence, uint32_t sequence) { return sequence == 0 || sequence < last_sequence; }
	// its entry in g_Stats.sw, -1 for none
	int stats = -1;

private:
	// running: between the plugin's start and stop; sequence: of the last frame;
	// start: which start of this codec's states it is at (m_Starts);
	// of a direct state: use, whether its transmission is on the direct path
	// (kTranscode) or left it for the audio path (kAudio), and source, the start
	// of the decoder's state it codes from (another start there: a new transmission)
	enum EUse : uint8_t { kUnused, kTranscode, kAudio };
	struct SState { void *state; std::chrono::steady_clock::time_point last_used; uint32_t sequence; bool running; EUse use; uint64_t start; uint64_t source; };
	// a state per stream and direction: decoding and encoding the same stream (DMR
	// reencode) each begin and end on their own, a start resets the plugin's state
	// stream: CTranscoderPacket::GetStreamKey, the module above the stream id (bits 16-23)
	static uint32_t Key(uint32_t stream, bool encode) { return (encode ? 0x80000000u : 0u) | stream; }
	// the target's encoder state for Transcode from codec `from` (TCD_CODEC_*, bits
	// 24-28): one of its own, used by the decoder's thread only, while Encode's is
	// the codec thread's (shared, the decoder's newer frames made Encode start
	// afresh at nearly every frame, tcd 0.22.5)
	static uint32_t DirectKey(uint32_t stream, uint32_t from) { return 0x40000000u | (from << 24) | stream; }
	SState *State(uint32_t key, uint32_t sequence);	// with m_Mutex held; started
	void Stop(SState *s, bool last);	// after the frame, with m_Mutex held

	const tcd_codec_plugin *m_Plugin;
	uint32_t m_Codec;
	std::shared_ptr<std::mutex> m_Mutex;	// the plugin's: one call at a time, for all its codecs
	bool m_Builtin;
	std::unordered_map<uint32_t, SState> m_States;	// by Key()
	std::unordered_set<uint32_t> m_Failed;	// the keys whose open failed, logged once
	uint64_t m_Starts = 0;	// the starts of its states so far
};

class CCodecPlugins
{
public:
	~CCodecPlugins();

	// tcd's own vocoders first, then the shared objects, which get the lines of
	// tcd.ini tcd does not know (key, value); true on an error. Lines no plugin
	// takes are warned about. choices: the name CodecP25 and CodecM17 give, by
	// TCD_CODEC_P25 and TCD_CODEC_M17: tcd's own for the codec is left out only
	// when not named in paths and a plugin that does it is chosen there.
	bool Load(const std::vector<std::string> &paths, const std::vector<std::pair<std::string, std::string>> &options, const std::map<uint32_t, std::string> &choices);
	// the software codec for a TCD_CODEC_*: of the plugin with that name, or
	// without a name of the first plugin that has it; nullptr when there is none
	CSoftCodec *Get(uint32_t codec, const std::string &name = std::string());
	// every plugin's software codec for a TCD_CODEC_*: tcd's own first, then in the order of the CodecPlugin lines
	std::vector<CSoftCodec *> All(uint32_t codec);

private:
	// handle: nullptr for tcd's own
	struct SLoaded { std::string path; void *handle; const tcd_codec_plugin *plugin; bool ready; std::shared_ptr<std::mutex> mutex; };
	std::vector<SLoaded> m_Loaded;
	std::vector<CSoftCodec *> m_Codecs;	// made by Get, one per plugin and codec
};

extern CCodecPlugins g_Codecs;
