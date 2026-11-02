/*
 *   Copyright (C) 2014 by Jonathan Naylor G4KLX and John Hays K7VE
 *   Copyright 2016 by Jeremy McDermond (NH6Z)
 *   Copyright 2021 by Thomas Early N7TAE
 *
 *   Adapted by K7VE from G4KLX dv3000d
 */

// tcd - a hybid transcoder using DVSI hardware and Codec2 software
// Copyright © 2022 Thomas A. Early N7TAE

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include <sys/select.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <cerrno>
#include <thread>

#include "DV3000.h"
#include "Configure.h"
#include "Controller.h"

extern CController g_Cont;

CDV3000::CDV3000(Encoding t) : CDVDevice(t) {}

CDV3000::~CDV3000()
{
}

int CDV3000::AnswerChannel(uint16_t length, uint8_t type, uint8_t field) const
{
	const uint16_t speech = (COMPAND_LINEAR == compand) ? 322 : 162;
	if ((11 == length && PKT_CHANNEL == type && PKT_CHAND == field) || (speech == length && PKT_SPEECH == type && PKT_SPEECHD == field))
		return 0;
	return -1;
}

bool CDV3000::SendAudio(const uint8_t /*channel*/, const int16_t *audio) const
{
	// Create Audio packet based on input int8_ts
	SDV_Packet p;
	p.start_byte = PKT_HEADER;
	p.header.packet_type = PKT_SPEECH;
	p.field_id = PKT_SPEECHD;
	p.payload.audio3k.num_samples = 160U;
	p.header.payload_length = htons(uint16_t(2 + SpeechOut(audio, p.payload.audio3k.samples)));

	// send audio packet to DV3000
	const ssize_t size = packet_size(p);
	auto written = device->Write(&p, size);
	if (written < 0)
	{
		FTDI_Error("Error writing audio packet", errno);
		return true;
	}
	else if (size != written)
	{
		std::cerr << "Incomplete Speech Packet write on " << description << std::endl;
		return true;
	}

	return false;
}

bool CDV3000::SendData(const uint8_t /* channel */, const uint8_t *data) const
{
	// Create data packet
	SDV_Packet p;
	p.start_byte = PKT_HEADER;
	p.header.payload_length = htons(1 + sizeof(p.payload.ambe3k));
	p.header.packet_type = PKT_CHANNEL;
	p.field_id = PKT_CHAND;
	p.payload.ambe3k.num_bits = 72U;
	memcpy(p.payload.ambe3k.data, data, 9);

	// send data packet to DV3000
	const ssize_t size = packet_size(p);
	auto written = device->Write(&p, size);
	if (written < 0)
	{
		FTDI_Error("Error writing AMBE Packet", errno);
		return true;
	}
	else if (size != written)
	{
		std::cerr << "Incomplete AMBE Packet write on " << description << std::endl;
		return true;
	}

	return false;
}

void CDV3000::ProcessPacket(const SDV_Packet &p)
{
	auto packet = PopWaiting(0);
	if (!packet)
	{
		// a blocking wait here held up the reader for good: nothing that will come
		// answers it, and every module of the device stood still
		std::cerr << description << ": an answer without a packet waiting for it, dropped" << std::endl;
		return;
	}
	{
		if (PKT_CHANNEL == p.header.packet_type)
		{
			if (11!=ntohs(p.header.payload_length) || PKT_CHAND!=p.field_id || 72!=p.payload.ambe3k.num_bits)
				dump("Improper ambe packet:", &p, packet_size(p));
			if (Encoding::dstar == type)
				packet->SetDStarData(p.payload.ambe3k.data);
			else
				packet->SetDMRData(p.payload.ambe3k.data);

		}
		else if (PKT_SPEECH == p.header.packet_type)
		{
			if (((COMPAND_LINEAR == compand) ? 322 : 162)!=ntohs(p.header.payload_length) || PKT_SPEECHD!=p.field_id || 160!=p.payload.audio3k.num_samples)
				dump("Improper audio packet:", &p, packet_size(p));
			int16_t pcm[160];
			SpeechIn(p.payload.audio3k.samples, pcm);
			packet->SetAudioSamples(pcm, false);
		}
		else
		{
			dump("ReadDevice() ERROR: Read an unexpected device packet:", &p, packet_size(p));
			return;
		}
		Deliver(packet, type);	// a DMR or a DStar device
	}
}
