// SvxAudio -- pieces the SvxReflector client and the SVX server share
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include <sys/socket.h>
#include <poll.h>
#include <cerrno>
#include <cmath>
#include <regex>
#include <chrono>
#include <algorithm>

#include "SvxAudio.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define SVX_MAX_TCP_FRAME   32768   // SvxReflector's MAX_POSTAUTH_FRAME_SIZE
#define SVX_MAX_CONCEAL     3       // lost packets concealed at most, longer gaps stay gaps
#define SVX_SEND_TIMEOUT_MS 2000    // a TCP message must be out by then

////////////////////////////////////////////////////////////////////////////////////////
// OPUS out

OpusEncoder *SvxCreateEncoder(void)
{
	int err;
	OpusEncoder *enc = opus_encoder_create(8000, 1, OPUS_APPLICATION_VOIP, &err);
	if (err != OPUS_OK)
		return nullptr;
	opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
	opus_encoder_ctl(enc, OPUS_SET_BITRATE(16000));
	return enc;
}

////////////////////////////////////////////////////////////////////////////////////////
// OPUS in

CSvxOpusRx::~CSvxOpusRx()
{
	if (m_Decoder)
		opus_decoder_destroy(m_Decoder);
}

bool CSvxOpusRx::Init(void)
{
	int err;
	m_Decoder = opus_decoder_create(8000, 1, &err);
	return err == OPUS_OK;
}

bool CSvxOpusRx::CheckSeq(uint16_t seq)
{
	if (m_HaveSeq)
	{
		int16_t diff = (int16_t)(seq - m_NextSeq);
		if (diff < 0 && diff > -100)
			return false;               // late or repeated
		if (diff > 0 && diff <= SVX_MAX_CONCEAL)
			m_Lost = diff;
		else
			m_Lost = 0;                 // in order, or a jump (restart) we don't fill
	}
	m_HaveSeq = true;
	m_NextSeq = seq + 1;
	return true;
}

bool CSvxOpusRx::Decode(const uint8_t *data, int len, std::vector<SvxPcmFrame> &out)
{
	if (!m_Decoder)
		return false;
	int16_t pcm[960];                   // 120 ms at 8 kHz, the longest OPUS packet

	// conceal packets lost in the middle of a talker, not before its first one
	if (m_Lost > 0 && m_Active)
	{
		for (int i = 0; i < m_Lost; i++)
		{
			int n = opus_decode(m_Decoder, nullptr, 0, pcm, m_LastSamples, 0);
			if (n > 0)
				m_Pcm.insert(m_Pcm.end(), pcm, pcm + n);
		}
	}
	m_Lost = 0;

	int n = opus_decode(m_Decoder, data, len, pcm, 960, 0);
	if (n <= 0)
		return false;
	m_LastSamples = n;
	m_Active = true;
	m_Pcm.insert(m_Pcm.end(), pcm, pcm + n);
	TakeFrames(out);
	return true;
}

void CSvxOpusRx::TakeFrames(std::vector<SvxPcmFrame> &out)
{
	size_t pos = 0;
	while (m_Pcm.size() - pos >= 160)
	{
		SvxPcmFrame f;
		std::copy(m_Pcm.begin() + pos, m_Pcm.begin() + pos + 160, f.begin());
		out.push_back(f);
		pos += 160;
	}
	m_Pcm.erase(m_Pcm.begin(), m_Pcm.begin() + pos);
}

bool CSvxOpusRx::Flush(SvxPcmFrame &last)
{
	bool have = !m_Pcm.empty();
	if (have)
	{
		last.fill(0);
		std::copy(m_Pcm.begin(), m_Pcm.end(), last.begin());
		m_Pcm.clear();
	}
	m_Active = false;
	m_Lost = 0;
	if (m_Decoder)
		opus_decoder_ctl(m_Decoder, OPUS_RESET_STATE);
	return have;
}

////////////////////////////////////////////////////////////////////////////////////////
// TCP framing

bool SvxTcpSendFrame(int fd, const uint8_t *data, uint32_t len)
{
	if (fd < 0)
		return false;

	// one buffer, so header and payload never go out apart
	std::vector<uint8_t> out;
	out.reserve(4 + len);
	SvxPack32(out, len);
	out.insert(out.end(), data, data + len);

	// the sockets may be non-blocking: wait for room instead of sending half a frame
	size_t sent = 0;
	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(SVX_SEND_TIMEOUT_MS);
	while (sent < out.size())
	{
		ssize_t n = send(fd, out.data() + sent, out.size() - sent, MSG_NOSIGNAL);
		if (n > 0)
		{
			sent += (size_t)n;
			continue;
		}
		if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
			return false;
		auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
		if (left <= 0)
			return false;
		struct pollfd pfd = { fd, POLLOUT, 0 };
		poll(&pfd, 1, (int)left);
	}
	return true;
}

int SvxTcpTakeFrame(std::vector<uint8_t> &buf, std::vector<uint8_t> &frame)
{
	if (buf.size() < 4)
		return 0;
	uint32_t len = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) | ((uint32_t)buf[2] << 8) | buf[3];
	if (len > SVX_MAX_TCP_FRAME)
		return -1;
	if (buf.size() < 4 + (size_t)len)
		return 0;
	frame.assign(buf.begin() + 4, buf.begin() + 4 + len);
	buf.erase(buf.begin(), buf.begin() + 4 + len);
	return 1;
}

////////////////////////////////////////////////////////////////////////////////////////
// packing

void SvxPack16(std::vector<uint8_t> &buf, uint16_t val)
{
	buf.push_back((val >> 8) & 0xFF);
	buf.push_back(val & 0xFF);
}

void SvxPack32(std::vector<uint8_t> &buf, uint32_t val)
{
	buf.push_back((val >> 24) & 0xFF);
	buf.push_back((val >> 16) & 0xFF);
	buf.push_back((val >> 8) & 0xFF);
	buf.push_back(val & 0xFF);
}

void SvxPackString(std::vector<uint8_t> &buf, const std::string &str)
{
	SvxPack16(buf, (uint16_t)str.size());
	buf.insert(buf.end(), str.begin(), str.end());
}

uint16_t SvxUnpack16(const std::vector<uint8_t> &buf, size_t &pos)
{
	if (pos + 2 > buf.size()) { pos = buf.size(); return 0; }
	uint16_t val = ((uint16_t)buf[pos] << 8) | buf[pos + 1];
	pos += 2;
	return val;
}

uint32_t SvxUnpack32(const std::vector<uint8_t> &buf, size_t &pos)
{
	if (pos + 4 > buf.size()) { pos = buf.size(); return 0; }
	uint32_t val = ((uint32_t)buf[pos] << 24) | ((uint32_t)buf[pos+1] << 16)
	             | ((uint32_t)buf[pos+2] << 8) | buf[pos+3];
	pos += 4;
	return val;
}

std::string SvxUnpackString(const std::vector<uint8_t> &buf, size_t &pos)
{
	uint16_t len = SvxUnpack16(buf, pos);
	if (pos + len > buf.size()) { pos = buf.size(); return ""; }
	std::string str((const char *)&buf[pos], len);
	pos += len;
	return str;
}

////////////////////////////////////////////////////////////////////////////////////////

// Pattern: 1-2 letters + 1+ digits + 1-4 letters, else everything before the first dash
std::string SvxExtractCallsign(const std::string &svxCallsign)
{
	static const std::regex csRegex("^([A-Z]{1,2}[0-9]+[A-Z]{1,4})");
	std::smatch match;
	if (std::regex_search(svxCallsign, match, csRegex))
		return match[1].str();
	auto dash = svxCallsign.find('-');
	if (dash != std::string::npos && dash >= 3)
		return svxCallsign.substr(0, dash);
	return svxCallsign;
}
