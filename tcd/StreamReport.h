#pragma once

// CStreamReports -- what the audio of a stream was like, told to the reflector
// after its last frame (STCReport, TCPacketDef.h): its level before and after the
// AGC, how much of it was clipped, and how its AMBE frames came through their FEC.
// The dashboard shows it with the stream's last heard entry, so a station sees
// whether it overdrives and what tcd corrected. Nothing in it names the vocoder.
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

#include "TCPacketDef.h"

class CStreamReports
{
public:
	static constexpr float kSpeechDb = -50.0f;	// a frame above this input level has speech
	static constexpr float kClipDb = -1.0f;		// an input peak at or above this is clipped

	// a frame from the reflector (the reader thread). fec_errors: the bits the FEC of
	// an AMBE frame corrected, -1 for a codec without that check
	void FrameIn(char module, uint16_t sid, ECodecType codec, bool last, int fec_errors, bool uncorrectable)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto &a = Stream(module, sid, codec);
		a.seen = Clock::now();
		a.last_in = a.seen;
		if (a.frames_in++ == 0)
			a.first_in = a.seen;
		a.last = a.last || last;
		if (fec_errors >= 0)
		{
			a.fec_frames++;
			a.fec_bits += uint32_t(fec_errors);
			if (fec_errors > 0)
				a.fec_corrected++;
			if (uncorrectable)
				a.fec_uncorrectable++;
		}
	}

	// a frame's levels in dBFS once the AGC had it (the audio threads); gain_db is the
	// AGC's, of no meaning when agc is false
	void Levels(char module, uint16_t sid, float rms_in, float peak_in, float rms_out, float gain_db, bool agc)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = m_streams.find(module);
		if (it == m_streams.end() || it->second.sid != sid)
			return;	// a frame of a stream already reported
		auto &a = it->second;
		a.seen = Clock::now();
		a.frames++;
		a.peak_all = std::max(a.peak_all, peak_in);
		a.agc = a.agc || agc;
		if (rms_in < kSpeechDb)
			return;
		// the peak and the clipping of the same frames: those with speech
		a.voice++;
		a.peak = std::max(a.peak, peak_in);
		a.power_in += std::pow(10.0, rms_in / 10.0);
		a.power_out += std::pow(10.0, rms_out / 10.0);
		if (peak_in >= kClipDb)
			a.clipped++;
		if (agc)
		{
			a.gain_sum += gain_db;
			a.gain_min = std::min(a.gain_min, gain_db);
			a.gain_max = std::max(a.gain_max, gain_db);
			a.gain_frames++;
		}
	}

	// how a codec plugin decoded a frame of the stream (TCD_FRAME_REPEAT, TCD_FRAME_MUTE);
	// a DVSI device does not say, its frames count only by their FEC
	void Decoded(char module, uint16_t sid, bool repeat, bool mute)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = m_streams.find(module);
		if (it == m_streams.end() || it->second.sid != sid)
			return;
		it->second.decoded = true;
		if (repeat)
			it->second.repeats++;
		if (mute)
			it->second.mutes++;
	}

	// the reports that are due go to send: 300 ms after the last frame of a stream
	// (its audio is through by then), 2 s after anything of one whose last frame got lost
	void Flush(const std::function<void(const STCPacket &)> &send)
	{
		std::vector<STCPacket> due;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			const auto now = Clock::now();
			due.swap(m_due);
			for (auto it = m_streams.begin(); it != m_streams.end();)
			{
				const auto quiet = now - it->second.seen;
				if ((it->second.last && quiet >= std::chrono::milliseconds(300)) || quiet >= std::chrono::seconds(2))
				{
					Report(it->first, it->second, due);
					it = m_streams.erase(it);
				}
				else
					it++;
			}
		}
		for (const auto &p : due)
			send(p);
	}

private:
	using Clock = std::chrono::steady_clock;
	struct SStream
	{
		uint16_t sid = 0;
		ECodecType codec = ECodecType::none;
		Clock::time_point first_in, last_in, seen;
		bool last = false, agc = false, decoded = false;
		uint32_t frames_in = 0, frames = 0, voice = 0, clipped = 0, gain_frames = 0;
		float peak = -100.0f, peak_all = -100.0f, gain_min = 1000.0f, gain_max = -1000.0f;
		double power_in = 0.0, power_out = 0.0, gain_sum = 0.0;
		uint32_t fec_frames = 0, fec_corrected = 0, fec_uncorrectable = 0, fec_bits = 0;
		uint32_t repeats = 0, mutes = 0;
	};

	// one stream per module: a new one on the module ends the one before. So does a
	// frame after the last one of the same id: DMR+ keeps its stream id for the next
	// transmission
	SStream &Stream(char module, uint16_t sid, ECodecType codec)
	{
		auto it = m_streams.find(module);
		if (it != m_streams.end() && (it->second.sid != sid || it->second.last))
		{
			Report(module, it->second, m_due);
			m_streams.erase(it);
			it = m_streams.end();
		}
		if (it == m_streams.end())
		{
			it = m_streams.emplace(module, SStream()).first;
			it->second.sid = sid;
			it->second.codec = codec;
		}
		return it->second;
	}

	static int16_t Tenths(double v)
	{
		return int16_t(std::lround(std::max(-1000.0, std::min(1000.0, v * 10.0))));
	}

	static void Report(char module, const SStream &a, std::vector<STCPacket> &out)
	{
		if (a.frames < 5)
			return;	// no audio of it measured, or a late frame after the last one (100 ms at least)
		STCReport r;
		memset(&r, 0, sizeof(r));
		r.magic = TC_REPORT_MAGIC;
		r.version = 1;
		r.codec_in = uint8_t(a.codec);
		r.flags = (a.agc && a.gain_frames ? TC_REPORT_AGC : 0) | (a.fec_frames ? TC_REPORT_FEC : 0) | (a.decoded ? TC_REPORT_DEC : 0);
		r.ms = uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(a.last_in - a.first_in).count()) + 20;
		r.frames = a.frames;
		r.voice = a.voice;
		r.clipped = a.clipped;
		r.level_in = Tenths(a.voice ? 10.0 * std::log10(a.power_in / a.voice) : -100.0);
		r.peak_in = Tenths(a.voice ? a.peak : a.peak_all);
		r.level_out = Tenths(a.voice ? 10.0 * std::log10(a.power_out / a.voice) : -100.0);
		if (a.gain_frames)
		{
			r.gain_avg = Tenths(a.gain_sum / a.gain_frames);
			r.gain_min = Tenths(a.gain_min);
			r.gain_max = Tenths(a.gain_max);
		}
		r.fec_frames = a.fec_frames;
		r.fec_corrected = a.fec_corrected;
		r.fec_uncorrectable = a.fec_uncorrectable;
		r.fec_bits = a.fec_bits;
		r.repeats = a.repeats;
		r.mutes = a.mutes;

		STCPacket p;
		memset(&p, 0, sizeof(p));
		p.module = module;
		p.streamid = a.sid;
		p.codec_in = ECodecType::ping;
		memcpy(p.usrp, &r, sizeof(r));
		out.push_back(p);
	}

	std::mutex m_mutex;
	std::map<char, SStream> m_streams;	// by module
	std::vector<STCPacket> m_due;	// ended by a new stream on their module
};

extern CStreamReports g_Reports;
