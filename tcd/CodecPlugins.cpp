// CodecPlugins -- the software vocoders: tcd's own and those of the CodecPlugin lines in tcd.ini
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include <algorithm>
#include <unordered_set>
#include <dlfcn.h>
#include <iostream>

#include "CodecPlugins.h"
#include "BuiltinCodecs.h"

CCodecPlugins g_Codecs;

// a stream quiet this long loses its state; at most this many states are kept,
// more than streams can run at once (one per stream and direction, on every
// module); beyond it the one quiet longest goes first
#define STATE_TIMEOUT std::chrono::seconds(30)
#define STATE_MAX     128	// a stream decoded and encoded has two, coded directly one more
// a state this long without a frame starts afresh at the next: a transmission
// before whose last frame got lost, or a target on the audio path only after
// its transmission went the direct path a while. More than urfd's stream
// timeout (1.6 s), a pause within a transmission keeps its states
#define STATE_IDLE    std::chrono::seconds(2)

CSoftCodec::~CSoftCodec()
{
	std::lock_guard<std::mutex> lock(*m_Mutex);
	for (auto &s : m_States)
		m_Plugin->close(s.second.state);
}

CSoftCodec::SState *CSoftCodec::State(uint32_t key, uint32_t sequence)
{
	const uint16_t streamid = uint16_t(key);
	const char module = char((key >> 16) & 0xffu);
	const auto now = std::chrono::steady_clock::now();
	auto it = m_States.find(key);
	if (it == m_States.end())
	{
		// a new stream: first make room, the stale ones and then the oldest go
		for (auto si = m_States.begin(); si != m_States.end(); )
		{
			if (now - si->second.last_used > STATE_TIMEOUT)
			{
				m_Plugin->close(si->second.state);
				si = m_States.erase(si);
			}
			else
				++si;
		}
		while (m_States.size() >= STATE_MAX)
		{
			auto oldest = m_States.begin();
			for (auto si = m_States.begin(); si != m_States.end(); ++si)
				if (si->second.last_used < oldest->second.last_used)
					oldest = si;
			m_Plugin->close(oldest->second.state);
			m_States.erase(oldest);
		}
		void *state = m_Plugin->open(m_Codec);
		if (!state)
		{
			// asked again at every frame: said once per stream and direction
			if (m_Failed.size() >= STATE_MAX)
				m_Failed.clear();
			if (m_Failed.insert(key).second)
				std::cerr << "Codec plugin " << m_Plugin->name << ": no state for stream " << streamid << " of module " << module
				          << ((key & 0x40000000u) ? ", it is coded from the audio" : ", its frames stay empty") << std::endl;
			return nullptr;
		}
		m_Failed.erase(key);
		it = m_States.emplace(key, SState{ state, now, sequence, false, kUnused, 0, 0 }).first;
	}
	if (it->second.running && (NewTransmission(it->second.sequence, sequence) || now - it->second.last_used > STATE_IDLE))
		it->second.running = false;
	if (!it->second.running)
	{
		if (m_Plugin->start)
			m_Plugin->start(it->second.state);
		it->second.running = true;
		it->second.use = kUnused;
		it->second.start = ++m_Starts;
	}
	it->second.last_used = now;
	it->second.sequence = sequence;
	return &it->second;
}

void CSoftCodec::Stop(SState *s, bool last)
{
	if (s && last && s->running)
	{
		if (m_Plugin->stop)
			m_Plugin->stop(s->state);
		s->running = false;
	}
}

bool CSoftCodec::Encode(uint32_t stream, uint32_t sequence, bool last, const int16_t *pcm, uint8_t *frame)
{
	std::lock_guard<std::mutex> lock(*m_Mutex);
	SState *s = State(Key(stream, true), sequence);
	const bool ok = s && 0 == m_Plugin->encode(s->state, pcm, frame);
	Stop(s, last);
	return ok;
}

bool CSoftCodec::Decode(uint32_t stream, uint32_t sequence, bool last, const uint8_t *frame, int16_t *pcm, tcd_frame_info *info, bool hold)
{
	std::lock_guard<std::mutex> lock(*m_Mutex);
	SState *s = State(Key(stream, false), sequence);
	tcd_frame_info none{ 0, 0 };
	if (!info)
		info = &none;
	*info = none;
	bool ok = false;
	if (s)
		ok = 0 == (m_Plugin->api >= 3 && m_Plugin->decode_info ? m_Plugin->decode_info(s->state, frame, pcm, info) : m_Plugin->decode(s->state, frame, pcm));
	Stop(s, last && !hold);
	return ok;
}

bool CSoftCodec::Transcode(uint32_t stream, uint32_t sequence, bool last, CSoftCodec *to, float gain_db, uint8_t *frame)
{
	if (!CanTranscode(to))
		return false;
	std::lock_guard<std::mutex> lock(*m_Mutex);	// the plugin's, shared by both codecs
	auto it = m_States.find(Key(stream, false));
	if (it == m_States.end())
		return false;	// nothing decoded to code from
	const uint32_t key = DirectKey(stream, m_Codec);
	auto dt = to->m_States.find(key);
	if (dt != to->m_States.end() && dt->second.running && dt->second.source == it->second.start)
	{
		// the transmission it codes: on directly while no frame went the audio path
		// (a gap in the sequence), else its rest goes the audio path
		if (dt->second.use == kTranscode && sequence > dt->second.sequence + 1)
			dt->second.use = kAudio;
		if (dt->second.use != kTranscode)
		{
			to->Stop(&dt->second, last);
			return false;
		}
	}
	else
	{
		// one before, whose last frame was not coded here: done
		if (dt != to->m_States.end() && dt->second.running)
			to->Stop(&dt->second, true);
		// the direct path from the first frame on only: the audio path's encoder
		// has the frames before, the rest would be coded from two histories
		if (sequence != 0)
			return false;
	}
	SState *s = to->State(key, sequence);
	if (!s)
		return false;
	if (s->use == kUnused)
	{
		// started here: at frame 0 (else after STATE_IDLE, its frames went elsewhere)
		s->use = sequence == 0 ? kTranscode : kAudio;
		s->source = it->second.start;
	}
	bool ok = false;
	if (s->use == kTranscode)
	{
		ok = 0 == m_Plugin->transcode(it->second.state, s->state, gain_db, frame);
		if (!ok)
			s->use = kAudio;
	}
	to->Stop(s, last);
	return ok;
}

void CSoftCodec::LeaveDirect(uint32_t stream, bool last, CSoftCodec *to)
{
	if (!CanTranscode(to))
		return;
	std::lock_guard<std::mutex> lock(*m_Mutex);
	auto dt = to->m_States.find(DirectKey(stream, m_Codec));
	if (dt == to->m_States.end() || !dt->second.running)
		return;
	dt->second.use = kAudio;
	to->Stop(&dt->second, last);
}

void CSoftCodec::End(uint32_t stream, bool encode)
{
	std::lock_guard<std::mutex> lock(*m_Mutex);
	auto it = m_States.find(Key(stream, encode));
	if (it != m_States.end())
		Stop(&it->second, true);
}

int CSoftCodec::Streams(void)
{
	// the transmissions not ended with a frame in the last 2 s: a state stays 30 s
	// after its stream ended, and a transmission whose last frame got lost gets no stop
	std::lock_guard<std::mutex> lock(*m_Mutex);
	const auto cutoff = std::chrono::steady_clock::now() - std::chrono::seconds(2);
	std::unordered_set<uint32_t> active;	// a stream decoded and encoded counts once
	for (const auto &s : m_States)
		if (s.second.running && s.second.last_used > cutoff)
			active.insert(s.first & 0x00ffffffu);
	return int(active.size());
}

CCodecPlugins::~CCodecPlugins()
{
	for (auto *c : m_Codecs)
		delete c;
	m_Codecs.clear();
	// the shared objects stay loaded, tcd ends here anyway
	for (auto &l : m_Loaded)
		if (l.ready && l.plugin->shutdown)
			l.plugin->shutdown();
}

bool CCodecPlugins::Load(const std::vector<std::string> &paths, const std::vector<std::pair<std::string, std::string>> &options, const std::map<uint32_t, std::string> &choices)
{
	// tcd's own vocoders, by a CodecPlugin line with their name instead of a path,
	// and the codec each is for
	const std::pair<const tcd_codec_plugin *, uint32_t> builtins[] = { { &g_BuiltinOp25, TCD_CODEC_P25 }, { &g_BuiltinCodec2, TCD_CODEC_M17 } };
	auto builtin = [&](const std::string &name) -> const tcd_codec_plugin * {
		for (const auto &b : builtins)
			if (name == b.first->name)
				return b.first;
		return nullptr;
	};
	// the name chooses the vocoder (CodecDStar ... CodecM17, the monitor), so it must be unique
	auto loaded = [&](const std::string &name) -> const SLoaded * {
		for (const auto &l : m_Loaded)
			if (name == l.plugin->name)
				return &l;
		return nullptr;
	};
	auto add_builtin = [&](const tcd_codec_plugin *plugin) { m_Loaded.push_back({ "built in", nullptr, plugin, false, std::make_shared<std::mutex>() }); };
	for (const auto &path : paths)
	{
		if (const auto *plugin = builtin(path))
		{
			if (loaded(path))
			{
				std::cerr << "ERROR: CodecPlugin " << path << " is there twice" << std::endl;
				return true;
			}
			add_builtin(plugin);
			continue;
		}
		void *handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
		if (!handle)
		{
			std::cerr << "ERROR: CodecPlugin " << path << ": " << dlerror() << std::endl;
			return true;
		}
		auto func = reinterpret_cast<tcd_codec_plugin_func>(dlsym(handle, "tcd_codec_plugin"));
		const tcd_codec_plugin *plugin = func ? func() : nullptr;
		if (!plugin)
		{
			std::cerr << "ERROR: CodecPlugin " << path << " is no tcd codec plugin (tcd_codec_plugin is missing)" << std::endl;
			dlclose(handle);
			return true;
		}
		if (plugin->api < TCD_CODEC_API_MIN || plugin->api > TCD_CODEC_API)
		{
			std::cerr << "ERROR: CodecPlugin " << path << " was built for codec API " << plugin->api << ", this tcd loads " << TCD_CODEC_API_MIN << " to " << TCD_CODEC_API << std::endl;
			dlclose(handle);
			return true;
		}
		if (!plugin->name || !*plugin->name || std::string(plugin->name).find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos)
		{
			std::cerr << "ERROR: CodecPlugin " << path << ": its name must be lower case letters, digits, '_' and '-'" << std::endl;
			dlclose(handle);
			return true;
		}
		if (!plugin->init || !plugin->open || !plugin->close || !plugin->encode || !plugin->decode)
		{
			std::cerr << "ERROR: CodecPlugin " << path << " lacks a function" << std::endl;
			dlclose(handle);
			return true;
		}
		// tcd carries frames of the formats CodecPlugin.h names only; a plugin
		// with M17 has to say its format
		const bool has_format = plugin->api >= 3 && plugin->frame_format;
		if (has_format || (plugin->codecs & (TCD_CODEC_M17 | TCD_CODEC_M17_1600)))
		{
			bool unusable = false;
			for (const auto &[bit, bytes, samples] : { std::make_tuple(TCD_CODEC_DSTAR, 9u, 160u), std::make_tuple(TCD_CODEC_DMR, 9u, 160u),
			                                           std::make_tuple(TCD_CODEC_P25, 11u, 160u), std::make_tuple(TCD_CODEC_M17, 8u, 160u),
			                                           std::make_tuple(TCD_CODEC_M17_1600, 8u, 320u) })
			{
				uint32_t b = 0, n = 0;
				if ((plugin->codecs & bit) && (!has_format || plugin->frame_format(bit, &b, &n) || b != bytes || n != samples))
					unusable = true;
			}
			if (unusable)
			{
				std::cerr << "ERROR: CodecPlugin " << path << " codes frames of a format tcd cannot carry" << std::endl;
				dlclose(handle);
				return true;
			}
		}
		const SLoaded *other = loaded(plugin->name);
		if (other || builtin(plugin->name))
		{
			std::cerr << "ERROR: CodecPlugin " << path << ": " << plugin->name << (other && other->handle ? " is loaded already (from " + other->path + ")" : std::string(" is the name of one of tcd's own vocoders")) << std::endl;
			dlclose(handle);
			return true;
		}
		m_Loaded.push_back({ path, handle, plugin, false, std::make_shared<std::mutex>() });
	}

	// without its line tcd's own is left out only where CodecP25/CodecM17 names a
	// plugin that does all of the codec (for M17 Codec2 1600 too, else codec2
	// decodes the voice+data streams); so a tcd.ini of before 0.22.0, without
	// the op25 and codec2 lines, keeps P25 and M17 on them as before
	for (const auto &[plugin, codec] : builtins)
	{
		if (loaded(plugin->name))
			continue;
		const auto c = choices.find(codec);
		const SLoaded *chosen = c == choices.end() || c->second.empty() ? nullptr : loaded(c->second);
		const uint32_t all = plugin->codecs;	// P25, or M17 and M17 1600
		if (!chosen || (chosen->plugin->codecs & all) != all)
		{
			const char *ini = codec == TCD_CODEC_P25 ? "CodecP25" : "CodecM17";
			const std::string named = c == choices.end() ? std::string() : c->second;
			std::cout << "CodecPlugin " << plugin->name << " is loaded without its line: ";
			if (named.empty())
				std::cout << ini << " chooses no plugin" << std::endl;
			else if (named == plugin->name)
				std::cout << ini << " chooses it" << std::endl;
			else if (chosen)
				std::cout << ini << " = " << named << " does not do all of " << (codec == TCD_CODEC_P25 ? "P25" : "M17 (Codec2 1600)") << std::endl;
			else
				std::cout << ini << " = " << named << " is no loaded plugin" << std::endl;
			add_builtin(plugin);
		}
	}
	// tcd's own first in every list, whatever the order of the lines
	std::stable_partition(m_Loaded.begin(), m_Loaded.end(), [](const SLoaded &l) { return !l.handle; });

	// the lines tcd does not know, to the plugins
	bool rval = false;
	for (const auto &[key, value] : options)
	{
		bool taken = false;
		for (const auto &l : m_Loaded)
		{
			const int r = l.plugin->option ? l.plugin->option(key.c_str(), value.c_str()) : 0;
			if (r < 0)
			{
				std::cerr << "ERROR: " << key << " = '" << value << "' is a bad value for codec plugin " << l.plugin->name << std::endl;
				rval = true;
			}
			if (r != 0)
			{
				taken = true;
				break;
			}
		}
		if (!taken)
			std::cout << "WARNING: Unexpected parameter: '" << key << "'" << std::endl;
	}
	if (rval)
		return true;

	for (auto &l : m_Loaded)
	{
		if (l.plugin->init())
		{
			std::cerr << "ERROR: CodecPlugin " << l.plugin->name << " (" << l.path << ") did not start" << std::endl;
			return true;
		}
		l.ready = true;
		if (!l.handle)
		{
			std::cout << "CodecPlugin " << l.plugin->name << ": " << l.plugin->description << std::endl;
			continue;
		}
		std::string codecs;
		for (const auto &[bit, name] : { std::make_pair(TCD_CODEC_DSTAR, "D-Star"), std::make_pair(TCD_CODEC_DMR, "DMR"), std::make_pair(TCD_CODEC_P25, "P25"),
		                                 std::make_pair(TCD_CODEC_M17, "M17"), std::make_pair(TCD_CODEC_M17_1600, "M17 1600") })
			if (l.plugin->codecs & bit)
				codecs += (codecs.empty() ? "" : ", ") + std::string(name);
		std::cout << "CodecPlugin " << l.plugin->name << " (" << codecs << ") loaded from " << l.path << (l.plugin->description ? std::string(": ") + l.plugin->description : std::string()) << std::endl;
	}
	return false;
}

std::vector<CSoftCodec *> CCodecPlugins::All(uint32_t codec)
{
	std::vector<CSoftCodec *> all;
	for (auto &l : m_Loaded)
		if (l.ready && (l.plugin->codecs & codec))
			all.push_back(Get(codec, l.plugin->name));
	return all;
}

CSoftCodec *CCodecPlugins::Get(uint32_t codec, const std::string &name)
{
	for (auto &l : m_Loaded)
	{
		if (!l.ready || !(l.plugin->codecs & codec) || (!name.empty() && name != l.plugin->name))
			continue;
		for (auto *c : m_Codecs)
			if (c->GetPlugin() == l.plugin && c->GetCodec() == codec)
				return c;
		m_Codecs.push_back(new CSoftCodec(l.plugin, codec, l.mutex, !l.handle));
		return m_Codecs.back();
	}
	return nullptr;
}
