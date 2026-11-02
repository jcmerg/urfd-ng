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
#include <algorithm>

#include "DVSIDevice.h"
#include "Configure.h"
#include "TcdStats.h"
#include "Controller.h"

// a device that leaves a packet unanswered for this long is taken as lost, like an
// unplugged one: a codec plugin takes its streams (or tcd stops, without one). An
// answer takes some 20-100 ms; within 1 s the plugin takes over before the radios
// end the transmission for the pause (some after 1.5-2 s)
#define DEVICE_SILENCE_MS 1000

// a packet unanswered for this long lost its answer (bytes of it dropped on the
// serial line, an FTDI receive overrun, and with them the channel it was for); a
// channel answers in order, the busiest within some 60 ms
#define LOST_ANSWER_MS 300

// what goes on for a lost answer: the silence frames of the vocoders, silent audio
static const uint8_t DSTAR_SILENCE[9] { 0x9EU, 0x8DU, 0x32U, 0x88U, 0x26U, 0x1AU, 0x3FU, 0x61U, 0xE8U };
static const uint8_t DMR_SILENCE[9]   { 0xB9U, 0xE8U, 0x81U, 0x52U, 0x61U, 0x73U, 0x00U, 0x2AU, 0x6BU };

static int64_t SteadyMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

extern CConfigure g_Conf;
extern CController g_Cont;

CDVDevice::CDVDevice(Encoding t) : type(t), devtype(Edvtype::dv3000), buffer_depth(0), keep_running(true)
{
}

uint8_t CDVDevice::TargetChannel(const std::shared_ptr<CTranscoderPacket> &packet) const
{
	std::lock_guard<std::mutex> lock(target_mutex);
	auto it = target_channel.find(packet.get());
	if (it == target_channel.end())
		return 0xFF;
	const uint8_t ch = it->second;
	target_channel.erase(it);
	return ch;
}

void CDVDevice::AddPacketToChannel(const std::shared_ptr<CTranscoderPacket> packet, uint8_t channel)
{
	{
		std::lock_guard<std::mutex> lock(target_mutex);
		target_channel[packet.get()] = channel;
	}
	AddPacket(packet);
}

// the device is gone or hung: the controller hands its channels to a codec plugin,
// or, without a handler, tcd stops and its service restarts it without the device
void CDVDevice::Lost(void)
{
	if (m_onLost)
		m_onLost();
	else
		kill(getpid(), SIGINT);
}

uint8_t CDVDevice::MapPacketToChannel(const std::shared_ptr<CTranscoderPacket> &packet) const
{
	const uint8_t target = TargetChannel(packet);
	if (target != 0xFF)
		return target;
	const std::string modules(g_Conf.GetTCMods());
	auto index = modules.find(packet->GetModule());
	const size_t channels = (Edvtype::dv3000 == devtype) ? 1 : 3;
	return (index < channels) ? (uint8_t)index : 0xFF;
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
	m_unanswered.store(0);
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

	// companding as DeviceCompand says (linear by default), on every device: after a reset
	// it follows the board's CP_ENABLE pin, which a DVstick-33 pulls up (µ-law); the DV3000
	// tested starts with it off, nothing says how another board has it
	{
		SDV_Packet compandPkt, compandResp;
		compandPkt.start_byte = PKT_HEADER;
		compandPkt.header.payload_length = htons(3);
		compandPkt.header.packet_type = PKT_CONTROL;
		compandPkt.field_id = pkt_ch;
		compandPkt.payload.ctrl.data.paritymode[0] = PKT_COMPAND;
		compandPkt.payload.ctrl.data.paritymode[1] = uint8_t(compand);
		if (device->Write(&compandPkt, 7) < 0)
			FTDI_Error("writing PKT_COMPAND", errno);
		// the answer as to the codec config: the channel, its status, then the field and its status (40 00 32 00)
		const bool ok = !GetResponse(compandResp) && compandResp.field_id == pkt_ch && 0 == compandResp.payload.ctrl.data.paritymode[0]
			&& PKT_COMPAND == compandResp.payload.ctrl.data.paritymode[1] && 0 == compandResp.payload.ctrl.data.paritymode[2];
		if (!ok)
		{
			std::cerr << (COMPAND_LINEAR == compand ? "Warning: " : "") << "no valid response to PKT_COMPAND on channel " << (unsigned int)(pkt_ch - PKT_CHANNEL0)
				<< " of " << description << (COMPAND_LINEAR == compand ? " (if the device still compands, its audio is distorted)" : "") << std::endl;
			// a device that kept other packets than tcd reads would give silence for good
			if (COMPAND_LINEAR != compand)
				return true;
			// an answer that comes late must not wait for the reader as if it were a frame's
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			device->PurgeInput();
		}
	}

	std::cout << description << " channel " << (unsigned int)(pkt_ch - PKT_CHANNEL0) << " is now configured for " << ((Encoding::dstar == type) ? "D-Star" : "DMR/YSF")
		<< (COMPAND_LINEAR != compand ? std::string(", speech in ") + CompandName(compand) : std::string()) << std::endl;

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
	// hex and a fill of 0 only here: the next "Opened ... at 921600 baud" in decimal
	const std::ios_base::fmtflags flags(std::cout.flags());
	const char fill = std::cout.fill();

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
	std::cout.flags(flags);
	std::cout.fill(fill);
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
					Encoding ch_enc = GetChannelEncoding(index);
					const bool needs_audio = (Encoding::dstar==ch_enc) ? packet->DStarIsSet() : packet->DMRIsSet();
					PushWaiting(index, packet, needs_audio);

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

					// stamped before it goes out, so its answer cannot come first
					int64_t none = 0;
					m_unanswered.compare_exchange_strong(none, SteadyMs());

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
				}
			}
		}
	}
}

void CDVDevice::ReadDevice()
{
	int64_t resynced = 0;
	while (keep_running)
	{
		// lost answers: per channel, also while the other channels are busy
		if (SteadyMs() - resynced >= 50)
		{
			Resync();
			resynced = SteadyMs();
		}
		// a device that is still there but gives no answer (a hung chip, or one that
		// sends what is no answer) would hold the modules forever: give up as for a
		// lost one, so tcd restarts without it and a codec plugin or a standby tcd
		// takes over. Frames still going to it and none answered for DEVICE_SILENCE_MS
		const int64_t unanswered = m_unanswered.load();
		if (unanswered && SteadyMs() - unanswered > DEVICE_SILENCE_MS)
		{
			std::cerr << "The AMBE device " << description << " has not answered for " << DEVICE_SILENCE_MS << " ms" << std::endl;
			Lost();
			return;
		}
		// wait for the next answer
		while (rx.empty() && keep_running)
		{
			const int ready = device->WaitReadable((buffer_depth > 0) ? 50 : 500);
			if (ready < 0)
			{
				// unplugged or reset: a codec plugin takes over until it is back, or tcd
				// stops and its service restarts it
				std::cerr << "Lost the AMBE device " << description << std::endl;
				Lost();
				return;
			}
			if (ready > 0 || !keep_running)
				break;

			Resync();
			resynced = SteadyMs();
			const int64_t unanswered = m_unanswered.load();
			if (unanswered && SteadyMs() - unanswered > DEVICE_SILENCE_MS)
				break;	// to the check above
		}
		if (!keep_running)
			return;

		std::vector<SDV_Packet> answers;
		const bool ok = ReadAnswers(answers);
		for (const auto &p : answers)
			ProcessPacket(p);
		if (!ok && rx_error && keep_running)
		{
			std::cerr << "Lost the AMBE device " << description << std::endl;
			Lost();
			return;
		}
	}
}

void CDVDevice::SpeechIn(const void *samples, int16_t *pcm) const
{
	if (COMPAND_LINEAR == compand)
	{
		const uint8_t *b = (const uint8_t *)samples;
		for (int i = 0; i < 160; i++)
			pcm[i] = int16_t((b[2 * i] << 8) | b[2 * i + 1]);
	}
	else
	{
		const uint8_t *b = (const uint8_t *)samples;
		for (int i = 0; i < 160; i++)
			pcm[i] = CompandToLinear(compand, b[i]);
	}
}

size_t CDVDevice::SpeechOut(const int16_t *audio, void *samples) const
{
	uint8_t *b = (uint8_t *)samples;
	if (COMPAND_LINEAR == compand)
	{
		for (int i = 0; i < 160; i++)
		{
			b[2 * i] = uint8_t(uint16_t(audio[i]) >> 8);
			b[2 * i + 1] = uint8_t(audio[i]);
		}
		return 320;
	}
	for (int i = 0; i < 160; i++)
		b[i] = LinearToCompand(compand, audio[i]);
	return 160;
}

void CDVDevice::PushWaiting(uint8_t channel, const std::shared_ptr<CTranscoderPacket> &packet, bool decode)
{
	std::lock_guard<std::mutex> lock(waiting_mx);
	waiting[channel].push_back(SWaiting{ packet, SteadyMs(), decode });
	buffer_depth++;
}

std::shared_ptr<CTranscoderPacket> CDVDevice::PopWaiting(uint8_t channel, bool *decode)
{
	std::shared_ptr<CTranscoderPacket> packet;
	{
		std::lock_guard<std::mutex> lock(waiting_mx);
		if (channel < 3 && !waiting[channel].empty())
		{
			packet = waiting[channel].front().packet;
			if (decode)
				*decode = waiting[channel].front().decode;
			waiting[channel].pop_front();
			buffer_depth--;
		}
	}
	{	// FeedDevice may send the next one (taken, so it cannot miss the wake-up)
		std::lock_guard<std::mutex> lk(buffer_mutex);
	}
	buffer_cv.notify_one();
	return packet;
}

void CDVDevice::Deliver(const std::shared_ptr<CTranscoderPacket> &packet, Encoding ch_enc)
{
	if (Encoding::dstar == ch_enc)
	{
		std::lock_guard<std::mutex> lock(g_Cont.dstar_mux);
		g_Cont.RouteDstPacket(packet);
	}
	else
	{
		std::lock_guard<std::mutex> lock(g_Cont.dmrst_mux);
		g_Cont.RouteDmrPacket(packet);
	}
}

void CDVDevice::LostAnswer(uint8_t channel, const char *why)
{
	if (m_statsIndex >= 0 && m_statsIndex < CTcdStats::MAX_DEVICES)
		g_Stats.devices[m_statsIndex].errors++;
	bool decode = false;
	auto packet = PopWaiting(channel, &decode);
	char line[192];
	if (!packet)
	{
		snprintf(line, sizeof(line), " channel %u: an answer was lost (%s), no packet waited for it", unsigned(channel), why);
		if (LogLost())
			std::cerr << description << line << std::endl;
		return;
	}
	snprintf(line, sizeof(line), " channel %u: the answer for module %c stream 0x%04x frame %u was lost (%s), a silent frame instead",
		unsigned(channel), packet->GetModule(), unsigned(packet->GetStreamId()), unsigned(packet->GetSequence()), why);
	if (LogLost())
		std::cerr << description << line << std::endl;
	const Encoding ch_enc = GetChannelEncoding(channel);
	if (decode)
	{
		const int16_t silence[160] = {};
		packet->SetAudioSamples(silence, false);
	}
	else if (Encoding::dstar == ch_enc)
		packet->SetDStarData(DSTAR_SILENCE);
	else
		packet->SetDMRData(DMR_SILENCE);
	Deliver(packet, ch_enc);
}

bool CDVDevice::LogLost(void)
{
	const int64_t now = SteadyMs();
	if (now - m_logWindow >= 60000)
	{
		if (m_suppressed)
			std::cerr << description << ": " << m_suppressed << " more lost answers in the minute before, not logged" << std::endl;
		m_logWindow = now;
		m_logged = 0;
		m_suppressed = 0;
	}
	if (m_logged < 5)
	{
		m_logged++;
		return true;
	}
	m_suppressed++;
	return false;
}

void CDVDevice::Resync(void)
{
	const int64_t now = SteadyMs();
	for (uint8_t ch = 0; ch < 3; ch++)
	{
		for (;;)
		{
			{
				std::lock_guard<std::mutex> lock(waiting_mx);
				if (waiting[ch].empty() || now - waiting[ch].front().sent < LOST_ANSWER_MS)
					break;
			}
			LostAnswer(ch, "no answer came");
		}
	}
	// the last frames of a stream whose answers were lost: nothing waits any more, so
	// this is no hung device (one keeps getting frames, which then wait)
	{
		std::lock_guard<std::mutex> lock(waiting_mx);
		if (0 == buffer_depth)
			m_unanswered.store(0);
	}
}

bool CDVDevice::ReadBytes(uint8_t *dst, size_t n)
{
	while (n && !rx.empty())
	{
		*dst++ = rx.front();
		rx.pop_front();
		n--;
	}
	while (n)
	{
		const ssize_t got = device->Read(dst, n);
		if (got < 0)
		{
			if (keep_running)	// not CloseDevice's Abort()
			{
				FTDI_Error("Reading an answer", errno);
				rx_error = true;
			}
			return false;
		}
		if (0 == got)
			return false;	// the read timeout
		dst += got;
		n -= size_t(got);
	}
	return true;
}

bool CDVDevice::ReadAhead(size_t n, int wait_ms)
{
	while (rx.size() < n)
	{
		if (device->WaitReadable(wait_ms) <= 0)
			return false;
		uint8_t buf[16];
		const ssize_t got = device->Read(buf, std::min(sizeof(buf), n - rx.size()));
		if (got <= 0)
			return false;
		rx.insert(rx.end(), buf, buf + got);
	}
	return true;
}

// The answers come one after the other, each from a start byte. An FTDI receive
// overrun drops a byte: then a start byte is missing, or a packet is one byte short
// and takes the start byte of the next. Without care the reader skips an answer, and
// from then on every answer of that channel went to the packet before its own (+20 ms
// each time, a stream's last frames held until the next stream). So a start byte
// counts only with the header of an answer this device sends; the bytes skipped to
// it are an answer without its start byte (taken) or a lost one (LostAnswer)
bool CDVDevice::ReadAnswers(std::vector<SDV_Packet> &answers)
{
	std::vector<uint8_t> skipped;
	auto settle = [&]() {
		if (skipped.empty())
			return;
		int ch = -1;
		size_t length = 0;
		if (skipped.size() >= 4)
		{
			length = (size_t(skipped[0]) << 8) | skipped[1];
			ch = AnswerChannel(uint16_t(length), skipped[2], skipped[3]);
		}
		if (ch >= 0 && skipped.size() == length + 3)
		{
			SDV_Packet p;
			p.start_byte = PKT_HEADER;
			memcpy(&p.header, skipped.data(), skipped.size());
			answers.push_back(p);
			m_unanswered.store(0);
			if (LogLost())
				std::cerr << description << " channel " << ch << ": an answer without its start byte, taken" << std::endl;
		}
		else if (ch >= 0)
			LostAnswer(uint8_t(ch), "bytes of it were lost");
		else if (Edvtype::dv3000 == devtype && skipped.size() >= 8)
			LostAnswer(0, "bytes of it were lost");	// one channel: the bytes were its answer
		else if (LogLost())
			std::cerr << description << ": " << skipped.size() << " bytes that are no answer skipped" << std::endl;
		skipped.clear();
	};

	uint8_t h[4];	// the payload length, packet type and field id after a start byte
	int ch = -1;
	size_t scanned = 0;
	for (;;)
	{
		// a device that sends nothing but what is no answer: back to ReadDevice, whose
		// checks (lost answers, a device that gives no answer) run between the reads
		if (++scanned > 2 * sizeof(SDV_Packet))
		{
			settle();
			return false;
		}
		uint8_t b;
		if (!ReadBytes(&b, 1))
		{
			settle();
			return false;
		}
		if (PKT_HEADER == b)
		{
			if (!ReadBytes(h, 4))
			{
				settle();
				return false;
			}
			ch = AnswerChannel(uint16_t((h[0] << 8) | h[1]), h[2], h[3]);
			if (ch >= 0)
			{
				m_unanswered.store(0);	// it answers
				break;
			}
			rx.insert(rx.begin(), h, h + 4);	// a 0x61 in the data: look on after it
		}
		skipped.push_back(b);
		if (skipped.size() > 2 * sizeof(SDV_Packet))
			skipped.erase(skipped.begin());
	}
	settle();

	SDV_Packet p;
	p.start_byte = PKT_HEADER;
	memcpy(&p.header, h, sizeof(h));	// the header and the field id
	const size_t length = (size_t(h[0]) << 8) | h[1];
	if (!ReadBytes(&p.field_id + 1, length - 1))
	{
		LostAnswer(uint8_t(ch), "the rest of it did not come");
		return false;
	}

	// a packet with a start byte and the header of an answer in its last bytes is
	// short: bytes of it were lost and it took the start of the next answer
	const uint8_t *bytes = &p.start_byte;
	const size_t size = 4 + length;
	for (size_t j = (size > 69) ? size - 64 : 5; j < size; j++)
	{
		if (PKT_HEADER != bytes[j])
			continue;
		uint8_t n[4];
		const size_t inside = std::min<size_t>(4, size - j - 1);
		memcpy(n, bytes + j + 1, inside);
		if (inside < 4)
		{
			if (!ReadAhead(4 - inside, 20))
				continue;
			for (size_t t = inside; t < 4; t++)
				n[t] = rx[t - inside];
		}
		if (AnswerChannel(uint16_t((n[0] << 8) | n[1]), n[2], n[3]) < 0)
			continue;
		// and the whole answer that starts there, then a start byte or nothing yet: the
		// audio of a good answer can hold what looks like the header of a short one
		const size_t end = j + 4 + ((size_t(n[0]) << 8) | n[1]);	// where it ends, from bytes[0]
		if (end < size)
		{
			if (PKT_HEADER != bytes[end])
				continue;	// it lies in this packet, the next start byte too
		}
		else
		{
			const size_t rest = end - size;	// its bytes still to come
			ReadAhead(rest + 1, 20);	// a DV3000 at 460800 baud takes 7 ms for an audio answer
			if (rx.size() < rest || (rx.size() > rest && PKT_HEADER != rx[rest]))
				continue;
		}
		rx.insert(rx.begin(), bytes + j, bytes + size);
		LostAnswer(uint8_t(ch), "bytes of it were lost");
		return true;
	}
	answers.push_back(p);
	return true;
}
