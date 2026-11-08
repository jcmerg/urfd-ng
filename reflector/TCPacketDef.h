#pragma once

// Copyright © 2021 Thomas A. Early N7TAE
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include <cstdint>
#include <cstring>

enum class ECodecType : std::uint8_t { none = 0, dstar = 1, dmr = 2, c2_1600 = 3, c2_3200 = 4, p25 = 5, usrp = 6, svx = 7, ping = 0xff };

using STCPacket = struct tcpacket_tag {
	uint32_t sequence;
	char module;
	bool is_last;
	uint16_t streamid;
	ECodecType codec_in;
	uint8_t dstar[9];
	uint8_t dmr[9];
	uint8_t m17[16];
	uint8_t p25[11];
	int16_t usrp[160];
};

// tcd's report of a stream, sent after its last frame: a packet of codec_in ping
// for the stream's module and streamid, with this at the start of usrp. A urfd
// older than this drops every ping unread, so it is safe to send to any urfd.
// Host byte order, like the rest of STCPacket (tcd and urfd are built from the
// same source); a later format gets another version, which GetTCReport drops
constexpr uint32_t TC_REPORT_MAGIC = 0x31505254u;	// "TRP1"
constexpr uint8_t TC_REPORT_AGC = 0x01u;	// the AGC was on: the gain fields mean something
constexpr uint8_t TC_REPORT_FEC = 0x02u;	// AMBE frames: the fec fields mean something
constexpr uint8_t TC_REPORT_DEC = 0x04u;	// a codec plugin decoded the stream: repeats and mutes mean something

struct STCReport
{
	uint32_t magic;			// TC_REPORT_MAGIC
	uint8_t  version;		// 1
	uint8_t  codec_in;		// ECodecType of the stream
	uint8_t  flags;			// TC_REPORT_*
	uint8_t  reserved;
	uint32_t ms;			// from the first frame to the last
	uint32_t frames;		// audio frames measured
	uint32_t voice;			// of them with speech: input above -50 dBFS
	uint32_t clipped;		// of them with an input peak of -1 dBFS or more
	int16_t  level_in;		// dBFS x 10: the mean power of the speech frames, before the AGC
	int16_t  peak_in;		// dBFS x 10: the highest peak of the speech frames before the AGC (of all frames without speech)
	int16_t  level_out;		// dBFS x 10: the mean power of the speech frames after the AGC
	int16_t  gain_avg;		// dB x 10: the AGC's gain over the speech frames
	int16_t  gain_min;
	int16_t  gain_max;
	uint32_t fec_frames;	// AMBE frames checked
	uint32_t fec_corrected;	// of them with bits the FEC corrected
	uint32_t fec_uncorrectable;	// of them the FEC could not correct (the decoder repeats or mutes)
	uint32_t fec_bits;		// the bits the FEC corrected
	uint32_t repeats;		// frames the decoder played as a repeat of the last good one (lost-frame codes too)
	uint32_t mutes;			// frames the decoder muted (too many errors or repeats in a row)
};
static_assert(sizeof(STCReport) <= sizeof(STCPacket::usrp), "STCReport fits into a packet");

// the report in a packet from tcd, false if it is none (a ping, or another packet)
inline bool GetTCReport(const STCPacket &packet, STCReport &report)
{
	if (packet.codec_in != ECodecType::ping)
		return false;
	memcpy(&report, packet.usrp, sizeof(report));
	return report.magic == TC_REPORT_MAGIC && report.version == 1;
}
