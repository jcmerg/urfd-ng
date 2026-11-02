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
#include <functional>
#include <unordered_map>
#include <deque>
#include <vector>
#include <mutex>
#include <condition_variable>

#include "PacketQueue.h"
#include "DVSIPacket.h"
#include "DeviceIO.h"
#include "G711.h"

class CDVDevice
{
public:
	CDVDevice(Encoding t);
	virtual ~CDVDevice();

	// node is the device node, desc its name (/dev/serial/by-id); baud as Probe found it
	bool OpenDevice(const std::string &node, const std::string &desc, Edvtype dvtype, unsigned long baud, int8_t in_gain, int8_t out_gain);
	// opens the device at baud, resets it, asks for its product ID (GetProductID)
	// and closes it again: true when it cannot be opened (opened false) or does not
	// answer. The control packets are the same for both types, so any device object
	// can probe a device of either type.
	bool Probe(const std::string &node, const std::string &desc, Edvtype dvtype, unsigned long baud, bool &opened);
	// Configure a single channel (call after OpenDevice for mixed-mode)
	bool ConfigureChannel(uint8_t pkt_ch, Encoding enc, int8_t in_gain, int8_t out_gain);
	void Start();
	void CloseDevice();
	void AddPacket(const std::shared_ptr<CTranscoderPacket> packet);
	// a packet for a channel the controller chose (a DV3000 has only channel 0)
	void AddPacketToChannel(const std::shared_ptr<CTranscoderPacket> packet, uint8_t channel);
	// called once from the reader thread when the device is lost (unplugged, or it
	// stopped answering); without a handler tcd stops, as before
	void SetLostHandler(std::function<void()> handler) { m_onLost = std::move(handler); }
	std::string GetProductID() { return productid; }
	Edvtype GetType() const { return devtype; }
	unsigned int GetBufferDepth() const { return buffer_depth.load(std::memory_order_relaxed); }
	// output gain per encoding: a DV3003 in mixed mode encodes D-Star and DMR
	void SetOutputGains(int32_t dstar_num, int32_t dmr_num) { m_outputGainDStar.store(dstar_num, std::memory_order_relaxed); m_outputGainDmr.store(dmr_num, std::memory_order_relaxed); }
	void SetStatsIndex(int idx) { m_statsIndex = idx; }
	// DeviceCompand for its speech packets (G711.h), before OpenDevice
	void SetCompand(int c) { compand = c; }

	// Virtual hooks for mixed-mode support
	virtual Encoding GetChannelEncoding(uint8_t /*channel*/) const { return type; }
	virtual uint8_t MapPacketToChannel(const std::shared_ptr<CTranscoderPacket> &packet) const;

protected:
	const Encoding type;
	Edvtype devtype;
	std::unique_ptr<IDeviceIO> device;
	std::atomic<unsigned int> buffer_depth;
	std::mutex buffer_mutex;
	std::condition_variable buffer_cv;
	std::atomic<bool> keep_running;
	// steady clock ms of the oldest packet sent since the last answer, 0 for none: a
	// device that leaves one unanswered for DEVICE_SILENCE_MS is taken as lost. (The
	// last packet sent against the last answer took an idle device as lost when the
	// reader's wait ran out between a new stream's first packet and its answer.)
	std::atomic<int64_t> m_unanswered{0};
	// the log of lost answers: at most 5 lines a minute, then how many more
	int64_t m_logWindow = 0;
	int m_logged = 0, m_suppressed = 0;
	bool LogLost(void);
	CPacketQueue input_queue;
	std::future<void> feedFuture, readFuture;
	std::string description, productid;
	std::atomic<int32_t> m_outputGainDStar{256}, m_outputGainDmr{256};
	int m_statsIndex = -1;
	int compand = COMPAND_LINEAR;	// the speech packets: 16-bit linear, or 8-bit µ-law or A-law
	// the 160 samples of a speech packet as 16-bit linear: big-endian words, or bytes
	// of the companding
	void SpeechIn(const void *samples, int16_t *pcm) const;
	// and for a speech packet: the bytes of its samples after num_samples, and their size
	size_t SpeechOut(const int16_t *audio, void *samples) const;
	bool probing = false;	// Probe: fewer messages
	std::function<void()> m_onLost;
	// the channel AddPacketToChannel chose for a packet, until FeedDevice takes it
	mutable std::unordered_map<CTranscoderPacket *, uint8_t> target_channel;
	mutable std::mutex target_mutex;
	// the channel of a packet from AddPacketToChannel (and forgets it), 0xFF without one
	uint8_t TargetChannel(const std::shared_ptr<CTranscoderPacket> &packet) const;
	void Lost(void);

	// the packets a vocoder channel works on, oldest first: a device answers them in
	// order, but nothing in an answer says which packet it is for. buffer_depth is
	// their number, over all channels
	struct SWaiting
	{
		std::shared_ptr<CTranscoderPacket> packet;
		int64_t sent;	// steady clock ms
		bool decode;	// AMBE in, audio expected back
	};
	std::deque<SWaiting> waiting[3];
	std::mutex waiting_mx;
	void PushWaiting(uint8_t channel, const std::shared_ptr<CTranscoderPacket> &packet, bool decode);
	// the packet the next answer of the channel is for (nullptr when none waits);
	// decode tells what it was sent for
	std::shared_ptr<CTranscoderPacket> PopWaiting(uint8_t channel, bool *decode = nullptr);
	// an answer of the channel was lost (a byte of it dropped on the serial line): its
	// packet goes on with a silent frame, the next answer belongs to the next packet
	void LostAnswer(uint8_t channel, const char *why);
	// packets left unanswered for LOST_ANSWER_MS: their answers were lost. Mostly the
	// last ones of a stream, which no later answer of their channel follows; while a
	// stream runs, a lost answer whose channel is not known costs it 20 ms until then
	void Resync(void);
	// a packet with this device's output set, on to the controller
	void Deliver(const std::shared_ptr<CTranscoderPacket> &packet, Encoding ch_enc);

	// reading the answers while running: bytes read ahead first, then the device
	std::deque<uint8_t> rx;
	bool rx_error = false;	// a read failed: the device is gone
	bool ReadBytes(uint8_t *dst, size_t n);	// false when they do not come (read timeout) or on an error
	bool ReadAhead(size_t n, int wait_ms);	// at least n bytes in rx, waiting up to wait_ms for them
	// the answers that came, in order (a lost start byte gives two at once); a lost
	// answer goes on through LostAnswer. False on a read error or timeout
	bool ReadAnswers(std::vector<SDV_Packet> &answers);
	// the vocoder channel of an answer with this header, -1 when it is none this device
	// sends while it runs (a start byte in the data of a packet whose start was lost)
	virtual int AnswerChannel(uint16_t length, uint8_t type, uint8_t field) const = 0;

	bool ConfigureVocoder(uint8_t pkt_ch, Encoding type, int8_t in_gain, int8_t out_gain);
	bool checkResponse(SDV_Packet &responsePacket, uint8_t response) const;
	bool GetResponse(SDV_Packet &packet);
	bool OpenLine(const std::string &node, const std::string &desc, Edvtype dvtype, unsigned long baud);
	bool InitDevice();
	void FeedDevice();
	void ReadDevice();
	void FTDI_Error(const char *where, int err) const;
	void dump(const char *title, const void *data, int length) const;

	// pure virtual methods unique to the device type
	virtual void ProcessPacket(const SDV_Packet &p) = 0;
	virtual bool SendAudio(const uint8_t channel, const int16_t *audio) const = 0;
	virtual bool SendData(const uint8_t channel, const uint8_t *data) const = 0;
};
