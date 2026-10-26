/*
 *   Copyright (C) 2014 by Jonathan Naylor G4KLX and John Hays K7VE
 *   Copyright 2016 by Jeremy McDermond (NH6Z)
 *   Copyright 2021 by Thomas Early N7TAE
 *
 *   Adapted by K7VE from G4KLX dv3000d
 */

// tcd - a hybid transcoder using DVSI hardware and Codec2 software
// Copyright © 2022-2023 Thomas A. Early N7TAE

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
#include <csignal>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <cerrno>
#include <thread>

#include "DVSIDevice.h"
#include "Configure.h"
#include "TcdStats.h"

// a device with packets waiting that answers nothing for this long is taken as lost,
// like an unplugged one: tcd stops and its service restarts it without the device
#define DEVICE_SILENCE_MS 3000

static int64_t SteadyMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

extern CConfigure g_Conf;

CDVDevice::CDVDevice(Encoding t) : type(t), devtype(Edvtype::dv3000), buffer_depth(0), keep_running(true)
{
}

uint8_t CDVDevice::MapPacketToChannel(const std::shared_ptr<CTranscoderPacket> &packet) const
{
	const std::string modules(g_Conf.GetTCMods());
	auto index = modules.find(packet->GetModule());
	return (index != std::string::npos) ? (uint8_t)index : 0xFF;
}

bool CDVDevice::ConfigureChannel(uint8_t pkt_ch, Encoding enc, int8_t in_gain, int8_t out_gain)
{
	return ConfigureVocoder(pkt_ch, enc, in_gain, out_gain);
}

CDVDevice::~CDVDevice()
{
	CloseDevice();
}

void CDVDevice::CloseDevice()
{
	input_queue.Shutdown();
	keep_running = false;

	// free threads stuck in a write held back by flow control; ReadDevice
	// wakes up within its wait timeout and sees keep_running
	if (device)
		device->Abort();
	if (feedFuture.valid())
		feedFuture.get();
	if (readFuture.valid())
		readFuture.get();
	if (device)
		device->Close();
}

void CDVDevice::FTDI_Error(const char *where, int err) const
{
	std::cerr << "SERIAL ERROR on " << description << ": " << where << ": " << strerror(err) << std::endl;
}

bool CDVDevice::checkResponse(SDV_Packet &p, uint8_t response) const
{
	if(p.start_byte != PKT_HEADER || p.header.packet_type != PKT_CONTROL || p.field_id != response)
		return true;

	return false;
}

// the serial line of the device: 8N1, RTS/CTS, at baud
bool CDVDevice::OpenLine(const std::string &node, const std::string &desc, Edvtype dvtype, unsigned long baud)
{
	device.reset(CreatePosixDevice());
	std::string opened;
	if (!device->Open(node, opened))
		return true;

	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	device->Purge();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	if (!device->SetDataCharacteristics(8, 1, 0))
	{
		FTDI_Error("SetDataCharacteristics", errno);
		return true;
	}

	if (!device->SetFlowControl(1))	// RTS/CTS
	{
		FTDI_Error("SetFlowControl", errno);
		return true;
	}

	if (!device->SetRTS(true))
	{
		FTDI_Error("SetRTS", errno);
		return true;
	}

	if (std::string::npos == desc.find("DF2ET"))
	{
		//for usb-3012 pull DTR high to take AMBE3003 out of reset.
		//for other devices noting is connected to DTR so it is a dont care
		if (!device->SetDTR(false))
		{
			FTDI_Error("ClrDTR", errno);
			return true;
		}
	}
	else
	{
		// for DF2ET-3003 interface pull DTR low to take AMBE3003 out of reset.
		const bool ok = device->SetDTR(true);
		const int err = errno;
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		if (!ok) {
			FTDI_Error("SetDTR", err);
			return true;
		}
	}

	if (!device->SetBaudRate(baud))
	{
		FTDI_Error("SetBaudRate", errno);
		return true;
	}

	description.assign(desc);
	description.append(" ");
	description.append(node);

	if (!probing)
		std::cout << "Opened " << description << " at " << baud << " baud" << std::endl;

	devtype = dvtype;
	return false;
}

bool CDVDevice::Probe(const std::string &node, const std::string &desc, Edvtype dvtype, unsigned long baud, bool &opened)
{
	probing = true;
	opened = !OpenLine(node, desc, dvtype, baud);
	const bool failed = !opened || InitDevice();
	probing = false;
	if (device)
		device->Close();
	device.reset();
	return failed;
}

bool CDVDevice::OpenDevice(const std::string &node, const std::string &desc, Edvtype dvtype, unsigned long baud, int8_t in_gain, int8_t out_gain)
{
	if (OpenLine(node, desc, dvtype, baud))
		return true;

	if (InitDevice())
		return true;

	const uint8_t limit = (Edvtype::dv3000 == dvtype) ? PKT_CHANNEL0 : PKT_CHANNEL2;
	for (uint8_t ch=PKT_CHANNEL0; ch<=limit; ch++)
	{
		Encoding ch_enc = GetChannelEncoding(ch - PKT_CHANNEL0);
		// Use caller's gains for default codec, look up alternate gains for mixed channels
		int8_t ig = in_gain, og = out_gain;
		if (ch_enc != type)
		{
			// Mixed channel: use D-Star gains from config
			ig = int8_t(g_Conf.GetGain(ch_enc == Encoding::dstar ? EGainType::dstarin : EGainType::dmrin));
			og = int8_t(g_Conf.GetGain(ch_enc == Encoding::dstar ? EGainType::dstarout : EGainType::dmrout));
		}
		if (ConfigureVocoder(ch, ch_enc, ig, og))
			return true;
	}
	return false;
}

bool CDVDevice::InitDevice()
{
	SDV_Packet responsePacket, ctrlPacket;

	// ********** flush serial buffer (needed for DV3003) *************
	if (Edvtype::dv3003 == devtype)
	{
		uint8_t zeros[350];
		memset(zeros, 0, sizeof(zeros));
		if (device->Write(zeros, sizeof(zeros)) < 0)
			FTDI_Error("flushing the serial buffer", errno);
		// let the zeros go out before anything is flushed (a flush of the output
		// would drop them), then drop what the device sent before the reset
		if (!device->Drain(100))
			std::cerr << "The flush zeros did not leave for " << description << " within 100 ms" << std::endl;
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		device->PurgeInput();
	}

	// ********** soft reset *************
    ctrlPacket.start_byte = PKT_HEADER;
    ctrlPacket.header.payload_length = htons(3);
    ctrlPacket.header.packet_type = PKT_CONTROL;
    ctrlPacket.field_id = PKT_RESET;
	ctrlPacket.payload.ctrl.data.paritymode[0] = PKT_PARITYBYTE;
	ctrlPacket.payload.ctrl.data.paritymode[1] = 0x3U ^ PKT_RESET ^ PKT_PARITYBYTE;
	// a second try when the first reset gets no answer, the same packet at the same rate
	for (int attempt = 1; ; attempt++)
	{
		auto written = device->Write(&ctrlPacket, 7);
		if (written < 0)
		{
			FTDI_Error("Error writing soft reset packet", errno);
			return true;
		}
		else if (7 != written)
		{
			std::cerr << "Incomplete soft reset packet write" << std::endl;
			return true;
		}

		if (! GetResponse(responsePacket))
			break;
		if (attempt == 2)
		{
			std::cerr << "Error receiving response to reset" << std::endl;
			return true;
		}
		std::cerr << "No response to the reset of " << description << ", trying again" << std::endl;
		device->PurgeInput();
	}

	if (checkResponse(responsePacket, PKT_READY))
	{
	   std::cerr << "Invalid response to soft reset" << std::endl;
	   dump("Soft Reset Response Packet:", &responsePacket, packet_size(responsePacket));
	   return true;
	}
	if (!probing)
		std::cout << "Successfully did a soft reset on " << description << std::endl;

	// ********** turn off parity *********
	ctrlPacket.start_byte = PKT_HEADER;
	ctrlPacket.header.payload_length = htons(4);
	ctrlPacket.header.packet_type = PKT_CONTROL;
	ctrlPacket.field_id = PKT_PARITYMODE;
	ctrlPacket.payload.ctrl.data.paritymode[0] = 0;
	ctrlPacket.payload.ctrl.data.paritymode[1] = PKT_PARITYBYTE;
	ctrlPacket.payload.ctrl.data.paritymode[2] = 0x4U ^ PKT_PARITYMODE ^ PKT_PARITYBYTE;
	auto written = device->Write(&ctrlPacket, 8);
	if (written < 0)
	{
		FTDI_Error("Error writing parity control packet", errno);
		return true;
	}
	else if (8 != written)
	{
		std::cerr << "Incomplete disable parity packet write" << std::endl;
		return true;
	}

	if (GetResponse(responsePacket))
	{
		std::cerr << "Error receiving response to parity set" << std::endl;
		return true;
	}

	if (checkResponse(responsePacket, PKT_PARITYMODE))
	{
		std::cerr << "Invalid response to parity control" << std::endl;
		dump("Parity Ctrl Response Packet:", &responsePacket, packet_size(responsePacket));
		return true;
	}

	if (!probing)
		std::cout << "Successfully disabled parity on " << description << std::endl;

	// ********* Product ID and Version *************
	ctrlPacket.start_byte = PKT_HEADER;
	ctrlPacket.header.payload_length = htons(1);
	ctrlPacket.header.packet_type = PKT_CONTROL;
	ctrlPacket.field_id = PKT_PRODID;

	written = device->Write(&ctrlPacket, 5);
	if (written < 0)
	{
		FTDI_Error("Error writing Product ID packet", errno);
		return true;
	}
	else if (5 != written)
	{
		std::cerr << "Incomplete Product ID Packet write" << std::endl;
		return true;
	}

	if (GetResponse(responsePacket))
	{
		std::cerr << "Error receiving response to Product ID request" << std::endl;
		return true;
	}

	if (checkResponse(responsePacket, PKT_PRODID))
	{
	   std::cerr << "Invalid response to Product ID query" << std::endl;
	   dump("Product ID Response Packet", &responsePacket, packet_size(responsePacket));
	   return true;
	}
	productid.assign(responsePacket.payload.ctrl.data.prodid);

	ctrlPacket.field_id = PKT_VERSTRING;
	written = device->Write(&ctrlPacket, 5);
	if (written < 0)
	{
		FTDI_Error("Error writing Version packet", errno);
		return true;
	}
	else if (5 != written)
	{
		std::cerr << "Incomplete Version packet write" << std::endl;
		return true;
	}

	if (GetResponse(responsePacket))
	{
		std::cerr << "Error receiving response to Version request" << std::endl;
		return true;
	}

	if (checkResponse(responsePacket, PKT_VERSTRING))
	{
	   std::cerr << "Invalid response to Version query" << std::endl;
	   dump("Product Version Response Packet:", &responsePacket, packet_size(responsePacket));
	   return true;
	}
	const std::string version(responsePacket.payload.ctrl.data.version);
	std::cout << description << ": ID=" <<  productid << " Version=" << version << std::endl;

	return false;
}

void CDVDevice::Start()
{
	m_lastAnswer.store(SteadyMs());
	feedFuture = std::async(std::launch::async, &CDVDevice::FeedDevice, this);
	readFuture = std::async(std::launch::async, &CDVDevice::ReadDevice, this);
}

bool CDVDevice::ConfigureVocoder(uint8_t pkt_ch, Encoding type, int8_t in_gain, int8_t out_gain)
{
	SDV_Packet controlPacket, responsePacket;
	const uint8_t ecmode[] { PKT_ECMODE, 0x0, 0x0 };
	const uint8_t dcmode[] { PKT_DCMODE, 0x0, 0x0 };
	const uint8_t  dstar[] { PKT_RATEP, 0x01U, 0x30U, 0x07U, 0x63U, 0x40U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x48U };
	const uint8_t    dmr[] { PKT_RATEP, 0x04U, 0x31U, 0x07U, 0x54U, 0x24U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x6FU, 0x48U };
	const uint8_t  chfmt[] { PKT_CHANFMT, 0x0, 0x0 };
	const uint8_t  spfmt[] { PKT_SPCHFMT, 0x0, 0x0 };
	const uint8_t   gain[] { PKT_GAIN, uint8_t(in_gain), uint8_t(out_gain) };
	const uint8_t   init[] { PKT_INIT, 0x3U };
	const uint8_t   resp[] { 0x0, PKT_ECMODE, 0x0, PKT_DCMODE, 0x0, PKT_RATEP, 0x0, PKT_CHANFMT, 0x0, PKT_SPCHFMT, 0x0, PKT_GAIN, 0x0, PKT_INIT, 0x0 };


	controlPacket.start_byte = PKT_HEADER;
	controlPacket.header.payload_length = htons(1 + sizeof(SDV_Packet::payload.codec));
	controlPacket.header.packet_type = PKT_CONTROL;
	controlPacket.field_id = pkt_ch;
	memcpy(controlPacket.payload.codec.ecmode, ecmode, 3);
	memcpy(controlPacket.payload.codec.dcmode, dcmode, 3);
	if (type == Encoding::dstar)
	{
		memcpy(controlPacket.payload.codec.ratep, dstar, 13);
	}
	else
	{
		memcpy(controlPacket.payload.codec.ratep, dmr, 13);
	}
	memcpy(controlPacket.payload.codec.chfmt, chfmt, 3);
	memcpy(controlPacket.payload.codec.spfmt, spfmt, 3);
	memcpy(controlPacket.payload.codec.gain, gain, 3);
	memcpy(controlPacket.payload.codec.init, init, 2);

	// write packet
	const ssize_t size = packet_size(controlPacket);
	auto written = device->Write(&controlPacket, size);
	if (written < 0)
	{
		FTDI_Error("error writing codec config packet", errno);
		return true;
	}
	else if (size != written)
	{
		std::cerr << "Incomplete Configuration packet write" << std::endl;
		return true;
	}

	if (GetResponse(responsePacket))
	{
		std::cerr << "Error reading Configuration response packet" << std::endl;
		return true;
	}

	if ((ntohs(responsePacket.header.payload_length) != 16) || (responsePacket.field_id != pkt_ch) || (0 != memcmp(responsePacket.payload.ctrl.data.resp, resp, sizeof(resp))))
	{
		std::cerr << "Config response packet failed" << std::endl;
		dump("Configuration Response Packet:", &responsePacket, packet_size(responsePacket));
		return true;
	};

	// DV3003 defaults to companding ON (CP_ENABLE pullup) — force linear PCM for all channels
	if (Edvtype::dv3003 == devtype)
	{
		SDV_Packet compandPkt, compandResp;
		compandPkt.start_byte = PKT_HEADER;
		compandPkt.header.payload_length = htons(3);
		compandPkt.header.packet_type = PKT_CONTROL;
		compandPkt.field_id = pkt_ch;
		compandPkt.payload.ctrl.data.paritymode[0] = PKT_COMPAND;
		compandPkt.payload.ctrl.data.paritymode[1] = 0x0;  // companding OFF
		if (device->Write(&compandPkt, 7) < 0)
			FTDI_Error("writing PKT_COMPAND", errno);
		if (GetResponse(compandResp))
			std::cerr << "Warning: no response to PKT_COMPAND on channel " << (unsigned int)(pkt_ch - PKT_CHANNEL0) << std::endl;
	}

	std::cout << description << " channel " << (unsigned int)(pkt_ch - PKT_CHANNEL0) << " is now configured for " << ((Encoding::dstar == type) ? "D-Star" : "DMR/YSF") << std::endl;

	return false;
}

// reads one packet; a read that times out (no data within the device's read
// timeout) counts as a failure, as with the blocking reads before
bool CDVDevice::GetResponse(SDV_Packet &packet)
{
	ssize_t bytes_read;

	// get the start byte
	for (unsigned i = 0U; i < USB3XXX_MAXPACKETSIZE+2; ++i) {
		bytes_read = device->Read(&packet.start_byte, 1);
		if (bytes_read < 0)
		{
			FTDI_Error("Reading packet start byte", errno);
			return true;
		}
		if (0 == bytes_read)
			return true;

		if (packet.start_byte == PKT_HEADER)
			break;
	}

	if (packet.start_byte != PKT_HEADER) {
		std::cerr << "Couldn't find start byte!" << std::endl;
		return true;
	}

	// get the packet size and type (three bytes)
	uint8_t *dst = reinterpret_cast<uint8_t *>(&packet.header);
	size_t bytesLeft = sizeof(packet.header);
	while (bytesLeft > 0) {
		bytes_read = device->Read(dst, bytesLeft);
		if (bytes_read < 0)
		{
			FTDI_Error("Error reading response packet header", errno);
			return true;
		}
		if (0 == bytes_read)
			return true;
		bytesLeft -= bytes_read;
		dst += bytes_read;
	}

	bytesLeft = ntohs(packet.header.payload_length);
	if (bytesLeft > 1 + sizeof(packet.payload)) {
		std::cerr << "AMBEserver: Serial payload exceeds buffer size: " << int(bytesLeft) << std::endl;
		device->Purge();
		return true;
	}

	dst = &packet.field_id;
	while (bytesLeft > 0) {
		bytes_read = device->Read(dst, bytesLeft);
		if (bytes_read < 0)
		{
			FTDI_Error("Error reading packet payload", errno);
			return true;
		}
		if (0 == bytes_read)
			return true;

		bytesLeft -= bytes_read;
		dst += bytes_read;
	}

	return false;
}

void CDVDevice::AddPacket(const std::shared_ptr<CTranscoderPacket> packet)
{
	auto size = input_queue.push(packet);
	if (size > 200)
	{
		std::cerr << ((type==Encoding::dstar) ? "DStar" : "DMR/YSF") << " inQ size=" << size << ", dropping packet" << std::endl;
	}
}

void CDVDevice::dump(const char *title, const void *pointer, int length) const
{
	const uint8_t *data = (const uint8_t *)pointer;

	std::cout << title << std::endl;

	unsigned int offset = 0U;

	while (length > 0) {

		unsigned int bytes = (length > 16) ? 16U : length;

		for (unsigned i = 0U; i < bytes; i++) {
			if (i)
				std::cout << " ";
			std::cout << std::hex << std::setw(2) << std::right << std::setfill('0') << int(data[offset + i]);
		}

		for (unsigned int i = bytes; i < 16U; i++)
			std::cout << "   ";

		std::cout << "   *";

		for (unsigned i = 0U; i < bytes; i++) {
			uint8_t c = data[offset + i];

			if (::isprint(c))
				std::cout << c;
			else
				std::cout << '.';
		}

		std::cout << '*' << std::endl;

		offset += 16U;

		if (length >= 16)
			length -= 16;
		else
			length = 0;
	}
}

void CDVDevice::FeedDevice()
{
	const std::string modules(g_Conf.GetTCMods());
	const auto n = modules.size();
	while (keep_running)
	{
		auto packet = input_queue.pop();	// blocks until there is something to pop, unless shutting down

		if (packet)
		{
			{	// wait until there is room in chip FIFO
				const unsigned int max_depth = (Edvtype::dv3003 == devtype) ? 5 : 2;
				std::unique_lock<std::mutex> lk(buffer_mutex);
				buffer_cv.wait_for(lk, std::chrono::milliseconds(50),
					[&]{ return !keep_running || buffer_depth < max_depth; });
			}

			if (keep_running)
			{
				auto index = MapPacketToChannel(packet);
				// save the packet in the vocoder's queue while the vocoder does its magic
				if (index == 0xFF)
				{
					std::cerr << "Module '" << packet->GetModule() << "' is not configured on " << description << std::endl;
				}
				else
				{
					PushWaitingPacket(index, packet);

					// Track channel activity in stats
					if (m_statsIndex >= 0 && m_statsIndex < CTcdStats::MAX_DEVICES
						&& index < DeviceStats::MAX_CHANNELS)
					{
						auto &ds = g_Stats.devices[m_statsIndex];
						ds.ch_module[index].store(packet->GetModule(), std::memory_order_relaxed);
						auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
							std::chrono::steady_clock::now().time_since_epoch()).count();
						ds.ch_last_ms[index].store(now, std::memory_order_relaxed);
					}

					Encoding ch_enc = GetChannelEncoding(index);
					const bool needs_audio = (Encoding::dstar==ch_enc) ? packet->DStarIsSet() : packet->DMRIsSet();

					if (needs_audio)
					{
						SendData(index, (Encoding::dstar==ch_enc) ? packet->GetDStarData() : packet->GetDMRData());
					}
					else
					{
						int32_t gn = ((Encoding::dstar==ch_enc) ? m_outputGainDStar : m_outputGainDmr).load(std::memory_order_relaxed);
						if (gn != 256)
						{
							int16_t buf[160];
							const int16_t *src = packet->GetAudioSamples();
							for (int i = 0; i < 160; i++)
							{
								int32_t v = ((int32_t)src[i] * gn) >> 8;
								if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
								buf[i] = (int16_t)v;
							}
							SendAudio(index, buf);
						}
						else
							SendAudio(index, packet->GetAudioSamples());
					}
					buffer_depth++;
					m_lastSent.store(SteadyMs());
				}
			}
		}
	}
}

void CDVDevice::ReadDevice()
{
	while (keep_running)
	{
		// Wait for incoming data, then let the bytes of a packet accumulate
		int RxBytes = device->BytesAvailable();

		while (RxBytes >= 0 && RxBytes < 5 && keep_running)  // minimum packet = 5 bytes (header)
		{
			if (device->WaitReadable((buffer_depth > 0) ? 50 : 500) < 0)
				RxBytes = -1;

			if (!keep_running)
				return;

			if (RxBytes >= 0)
				RxBytes = device->BytesAvailable();

			// After event, let remaining bytes of packet arrive
			if (RxBytes > 0 && RxBytes < 5)
				std::this_thread::sleep_for(std::chrono::microseconds(500));

			// Timeout recovery
			if (0 == RxBytes && buffer_depth > 0)
			{
				buffer_depth = 0;
				buffer_cv.notify_one();
			}

			// a device that is still there but answers nothing (a hung chip) would hold
			// the modules forever: give up as for a lost one, so tcd restarts without it
			// and a codec plugin or a standby tcd takes over
			if (0 == RxBytes && m_lastSent.load() - m_lastAnswer.load() > DEVICE_SILENCE_MS)
			{
				std::cerr << "The AMBE device " << description << " has not answered for " << DEVICE_SILENCE_MS / 1000 << " s, shutting down" << std::endl;
				kill(getpid(), SIGINT);
				return;
			}
		}

		if (RxBytes < 0)
		{
			// unplugged or reset: give up, systemd restarts tcd and it finds the device again
			std::cerr << "Lost the AMBE device " << description << ", shutting down" << std::endl;
			kill(getpid(), SIGINT);
			return;
		}

		SDV_Packet p;
		if (! GetResponse(p))
		{
			m_lastAnswer.store(SteadyMs());
			ProcessPacket(p);
		}
	}
}
