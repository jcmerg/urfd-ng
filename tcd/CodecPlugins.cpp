// CodecPlugins -- the software vocoders of the CodecPlugin lines in tcd.ini
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include <dlfcn.h>
#include <iostream>

#include "CodecPlugins.h"

CCodecPlugins g_Codecs;

// a stream quiet this long loses its state; at most this many states are kept,
// more than streams can run at once (one per module and direction)
#define STATE_TIMEOUT std::chrono::seconds(30)
#define STATE_MAX     64

CSoftCodec::~CSoftCodec()
{
	std::lock_guard<std::mutex> lock(*m_Mutex);
	for (auto &s : m_States)
		m_Plugin->close(s.second.state);
}

CSoftCodec::SState *CSoftCodec::State(uint16_t streamid, uint32_t sequence)
{
	const auto now = std::chrono::steady_clock::now();
	auto it = m_States.find(streamid);
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
			std::cerr << "Codec plugin " << m_Plugin->name << ": no state for stream " << streamid << ", its frames stay empty" << std::endl;
			return nullptr;
		}
		it = m_States.emplace(streamid, SState{ state, now, sequence, false }).first;
	}
	// urfd counts the frames of a transmission from 0: a lower count is the same
	// stream id again, after a transmission whose last frame got lost
	if (it->second.running && sequence < it->second.sequence)
		it->second.running = false;
	if (!it->second.running)
	{
		if (m_Plugin->start)
			m_Plugin->start(it->second.state);
		it->second.running = true;
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

bool CSoftCodec::Encode(uint16_t streamid, uint32_t sequence, bool last, const int16_t *pcm, uint8_t *frame)
{
	std::lock_guard<std::mutex> lock(*m_Mutex);
	SState *s = State(streamid, sequence);
	const bool ok = s && 0 == m_Plugin->encode(s->state, pcm, frame);
	Stop(s, last);
	return ok;
}

bool CSoftCodec::Decode(uint16_t streamid, uint32_t sequence, bool last, const uint8_t *frame, int16_t *pcm)
{
	std::lock_guard<std::mutex> lock(*m_Mutex);
	SState *s = State(streamid, sequence);
	const bool ok = s && 0 == m_Plugin->decode(s->state, frame, pcm);
	Stop(s, last);
	return ok;
}

int CSoftCodec::Streams(void)
{
	// the transmissions not ended with a frame in the last 2 s: a state stays 30 s
	// after its stream ended, and a transmission whose last frame got lost gets no stop
	std::lock_guard<std::mutex> lock(*m_Mutex);
	const auto cutoff = std::chrono::steady_clock::now() - std::chrono::seconds(2);
	int active = 0;
	for (const auto &s : m_States)
		if (s.second.running && s.second.last_used > cutoff)
			active++;
	return active;
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

bool CCodecPlugins::Load(const std::vector<std::string> &paths, const std::vector<std::pair<std::string, std::string>> &options)
{
	for (const auto &path : paths)
	{
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
		if (plugin->api != TCD_CODEC_API)
		{
			std::cerr << "ERROR: CodecPlugin " << path << " was built for codec API " << plugin->api << ", this tcd has " << TCD_CODEC_API << std::endl;
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
		// the name chooses the plugin (CodecDStar/CodecDMR, the monitor), so it must be unique
		for (const auto &l : m_Loaded)
			if (std::string(l.plugin->name) == plugin->name)
			{
				std::cerr << "ERROR: CodecPlugin " << path << ": a plugin named " << plugin->name << " is loaded already (from " << l.path << ")" << std::endl;
				dlclose(handle);
				return true;
			}
		m_Loaded.push_back({ path, handle, plugin, false, std::make_shared<std::mutex>() });
	}

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
		std::string codecs;
		for (const auto &[bit, name] : { std::make_pair(TCD_CODEC_DSTAR, "D-Star"), std::make_pair(TCD_CODEC_DMR, "DMR"), std::make_pair(TCD_CODEC_P25, "P25") })
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
		m_Codecs.push_back(new CSoftCodec(l.plugin, codec, l.mutex));
		return m_Codecs.back();
	}
	return nullptr;
}
