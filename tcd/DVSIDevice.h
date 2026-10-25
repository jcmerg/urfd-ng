#pragma once

// tcd - a hybid transcoder using DVSI hardware and Codec2 software
// Copyright © 2022 Thomas A. Early N7TAE
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

#include <netinet/in.h>
#include <string>
#include <sstream>
#include <future>
#include <atomic>
#include <memory>

#include "PacketQueue.h"
#include "DVSIPacket.h"
#include "DeviceIO.h"

class CDVDevice
{
public:
	CDVDevice(Encoding t);
	virtual ~CDVDevice();

	bool OpenDevice(const std::string &serialno, const std::string &desc, Edvtype dvtype, int8_t in_gain, int8_t out_gain);
	// Configure a single channel (call after OpenDevice for mixed-mode)
	bool ConfigureChannel(uint8_t pkt_ch, Encoding enc, int8_t in_gain, int8_t out_gain);
	void Start();
	void CloseDevice();
	void AddPacket(const std::shared_ptr<CTranscoderPacket> packet);
	std::string GetProductID() { return productid; }
	unsigned int GetBufferDepth() const { return buffer_depth.load(std::memory_order_relaxed); }
	// output gain per encoding: a DV3003 in mixed mode encodes D-Star and DMR
	void SetOutputGains(int32_t dstar_num, int32_t dmr_num) { m_outputGainDStar.store(dstar_num, std::memory_order_relaxed); m_outputGainDmr.store(dmr_num, std::memory_order_relaxed); }
	void SetStatsIndex(int idx) { m_statsIndex = idx; }

	// Virtual hooks for mixed-mode support
	virtual Encoding GetChannelEncoding(uint8_t channel) const { return type; }
	virtual uint8_t MapPacketToChannel(const std::shared_ptr<CTranscoderPacket> &packet) const;

protected:
	const Encoding type;
	Edvtype devtype;
	std::unique_ptr<IDeviceIO> device;
	std::atomic<unsigned int> buffer_depth;
	std::mutex buffer_mutex;
	std::condition_variable buffer_cv;
	std::atomic<bool> keep_running;
	CPacketQueue input_queue;
	std::future<void> feedFuture, readFuture;
	std::string description, productid;
	std::atomic<int32_t> m_outputGainDStar{256}, m_outputGainDmr{256};
	int m_statsIndex = -1;

	bool ConfigureVocoder(uint8_t pkt_ch, Encoding type, int8_t in_gain, int8_t out_gain);
	bool checkResponse(SDV_Packet &responsePacket, uint8_t response) const;
	bool GetResponse(SDV_Packet &packet);
	bool InitDevice();
	void FeedDevice();
	void ReadDevice();
	void FTDI_Error(const char *where, int err) const;
	void dump(const char *title, const void *data, int length) const;

	// pure virtual methods unique to the device type
	virtual void PushWaitingPacket(unsigned int channel, std::shared_ptr<CTranscoderPacket> packet) = 0;
	// the packet a vocoder answers, without waiting: nullptr when none waits on the
	// channel (an answer the device sends for nothing it was given)
	virtual std::shared_ptr<CTranscoderPacket> PopWaitingPacket(unsigned int channel) = 0;
	virtual void ProcessPacket(const SDV_Packet &p) = 0;
	virtual bool SendAudio(const uint8_t channel, const int16_t *audio) const = 0;
	virtual bool SendData(const uint8_t channel, const uint8_t *data) const = 0;
};
