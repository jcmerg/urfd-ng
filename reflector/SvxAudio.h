#pragma once

// SvxAudio -- pieces the SvxReflector client and the SVX server share:
// OPUS in and out at urfd's 8 kHz / 20 ms frames, gain, TCP framing and
// bounds checked unpacking of SvxReflector messages.
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include <opus/opus.h>
#include <cstdint>
#include <string>
#include <vector>
#include <array>

using SvxPcmFrame = std::array<int16_t, 160>;

// OPUS encoder for urfd audio: 8 kHz voice, the SvxLink side decodes it at its own rate
OpusEncoder *SvxCreateEncoder(void);

// OPUS receiver for one talker stream.
// SvxLink sends 20 ms frames by default but OPUS_ENC_FRAME_SIZE may set 10 to 60 ms,
// so decoded audio is collected and handed out in urfd's 160 sample frames.
// Every UDP message of the connection counts its sequence number, a gap while
// audio flows is lost audio and is concealed by the decoder.
class CSvxOpusRx
{
public:
	CSvxOpusRx() {}
	~CSvxOpusRx();
	CSvxOpusRx(const CSvxOpusRx &) = delete;
	CSvxOpusRx &operator=(const CSvxOpusRx &) = delete;

	bool Init(void);

	// sequence number of any UDP message from the peer; false for a late or repeated one
	bool CheckSeq(uint16_t seq);
	// decode one OPUS packet (whose sequence number passed CheckSeq), append whole frames to out
	bool Decode(const uint8_t *data, int len, std::vector<SvxPcmFrame> &out);
	// end of a talker: the rest as a last zero padded frame (if any), decoder reset
	bool Flush(SvxPcmFrame &last);
	// forget sequence state, e.g. after a reconnect
	void ResetSeq(void) { m_HaveSeq = false; m_Lost = 0; }

private:
	void TakeFrames(std::vector<SvxPcmFrame> &out);

	OpusDecoder *m_Decoder = nullptr;
	bool m_HaveSeq = false;
	uint16_t m_NextSeq = 0;
	int m_Lost = 0;                    // packets missing just before the next audio
	bool m_Active = false;             // a talker's audio is running
	int m_LastSamples = 160;           // size of the last decoded packet, for concealment
	std::vector<int16_t> m_Pcm;        // decoded, not yet a whole 160 sample frame
};

// helpers for SvxReflector TCP messages (4 byte big-endian length + payload)
bool SvxTcpSendFrame(int fd, const uint8_t *data, uint32_t len);

// takes one complete frame out of a receive buffer: 1 got one, 0 needs more data,
// -1 the peer sent garbage (frame too long) and the caller should disconnect
int SvxTcpTakeFrame(std::vector<uint8_t> &buf, std::vector<uint8_t> &frame);

// big-endian packing, unpacking stops at the end of the buffer (pos == size, 0 / "")
void SvxPack16(std::vector<uint8_t> &buf, uint16_t val);
void SvxPack32(std::vector<uint8_t> &buf, uint32_t val);
void SvxPackString(std::vector<uint8_t> &buf, const std::string &str);
uint16_t SvxUnpack16(const std::vector<uint8_t> &buf, size_t &pos);
uint32_t SvxUnpack32(const std::vector<uint8_t> &buf, size_t &pos);
std::string SvxUnpackString(const std::vector<uint8_t> &buf, size_t &pos);

// "DL4JC-APP" -> "DL4JC"
std::string SvxExtractCallsign(const std::string &svxCallsign);
